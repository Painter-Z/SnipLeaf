[CmdletBinding()]
param([switch]$SkipBuild,[string]$InnoCompiler)
$ErrorActionPreference='Stop'
$taskProjectRoot=Split-Path -Parent $PSScriptRoot
$taskProduct=Get-Content -LiteralPath (Join-Path $taskProjectRoot 'packaging\Product.json') -Raw -Encoding UTF8|ConvertFrom-Json
if($taskProduct.version -notmatch '^\d+\.\d+\.\d+$'){throw 'Invalid product version.'}
$taskExistingInstaller=Join-Path $taskProjectRoot ('out\release\SnipLeaf-'+$taskProduct.version+'-x64-Preview-Setup.exe')
if(Test-Path -LiteralPath $taskExistingInstaller){throw 'This version already has an archived installer. Increment the product/package versions before building a new release; do not overwrite historical artifacts.'}
if(-not $SkipBuild){& (Join-Path $PSScriptRoot 'Build.ps1') -Distribution}
$taskCompiler=$InnoCompiler
if(-not $taskCompiler){
    $taskInnoCommand=Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if($taskInnoCommand){$taskCompiler=$taskInnoCommand.Source}
    else{
        $taskCompiler=@(
            (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 7\ISCC.exe'),
            (Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Inno Setup 7\ISCC.exe'),
            (Join-Path $env:ProgramFiles 'Inno Setup 7\ISCC.exe')
        )|Where-Object {Test-Path -LiteralPath $_ -PathType Leaf}|Select-Object -First 1
    }
}
if(-not $taskCompiler -or -not(Test-Path -LiteralPath $taskCompiler -PathType Leaf)){throw 'Install Inno Setup 7.1 or later; use -InnoCompiler to specify ISCC.exe when needed.'}
. (Join-Path $PSScriptRoot 'Find-WindowsSdk.ps1')
$taskMakeAppx=Find-TaskWindowsSdkTool -Tool 'makeappx.exe'
$taskSignTool=Find-TaskWindowsSdkTool -Tool 'signtool.exe'
$taskOut=Join-Path $taskProjectRoot 'out'
$taskBuildId=[Guid]::NewGuid().ToString('N')
$taskStage=Join-Path $taskOut ('installer-stage\'+$taskProduct.version+'\'+$taskBuildId)
$taskIdentityStage=Join-Path $taskOut ('installer-identity\'+$taskProduct.version+'\'+$taskBuildId)
[IO.Directory]::CreateDirectory((Join-Path $taskStage 'Assets'))|Out-Null
[IO.Directory]::CreateDirectory($taskIdentityStage)|Out-Null
[xml]$taskManifest=Get-Content -LiteralPath (Join-Path $taskProjectRoot 'packaging\AppxManifest.xml') -Raw -Encoding UTF8
$taskManifest.Package.Identity.Name=$taskProduct.packageName
$taskManifest.Package.Identity.Publisher=$taskProduct.publisher
$taskManifest.Package.Identity.ProcessorArchitecture='neutral'
$taskManifest.Package.Identity.Version=$taskProduct.packageVersion
$taskManifest.Package.Properties.DisplayName=$taskProduct.displayName
$taskManifest.Package.Properties.PublisherDisplayName='SnipLeaf Project'
$taskExternal=$taskManifest.CreateElement('uap10','AllowExternalContent','http://schemas.microsoft.com/appx/manifest/uap/windows10/10')
$taskExternal.InnerText='true'
[void]$taskManifest.Package.Properties.AppendChild($taskExternal)
$taskManifest.Package.Applications.Application.Id=$taskProduct.applicationId
$taskManifest.Package.Applications.Application.Executable=$taskProduct.name+'.exe'
$taskManifest.Package.Applications.Application.VisualElements.DisplayName=$taskProduct.displayName
$taskManifest.Package.Applications.Application.VisualElements.SetAttribute('AppListEntry','none')
$taskManifest.Save((Join-Path $taskIdentityStage 'AppxManifest.xml'))
$taskIdentity=Join-Path $taskStage 'SnipLeaf.identity.msix'
& $taskMakeAppx pack /o /nv /d $taskIdentityStage /p $taskIdentity
if($LASTEXITCODE -ne 0){throw 'Preview identity packaging failed.'}
Copy-Item -LiteralPath (Join-Path $taskOut 'distribution-bin\SnipLeaf.exe') -Destination (Join-Path $taskStage 'SnipLeaf.exe')
foreach($taskSize in @(44,50,150)){Copy-Item -LiteralPath (Join-Path $taskProjectRoot "resources\Assets\Logo$taskSize.png") -Destination (Join-Path $taskStage "Assets\Logo$taskSize.png")}
$taskReadme=[IO.File]::ReadAllText((Join-Path $taskProjectRoot 'packaging\installer\Preview.txt'))
[IO.File]::WriteAllText((Join-Path $taskStage '使用说明.txt'),$taskReadme,[Text.UTF8Encoding]::new($true))
$taskCertificate=$null
try{
    $taskCertificate=New-SelfSignedCertificate -Type Custom -Subject $taskProduct.publisher -FriendlyName 'SnipLeaf invited preview only' -KeyAlgorithm RSA -KeyLength 2048 -KeyUsage DigitalSignature -KeyExportPolicy NonExportable -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3','2.5.29.19={text}CA=false') -CertStoreLocation 'Cert:\CurrentUser\My' -NotAfter (Get-Date).AddYears(1)
    $taskPublic=Join-Path $taskStage 'SnipLeaf-preview.cer'
    Export-Certificate -Cert $taskCertificate -FilePath $taskPublic|Out-Null
    foreach($taskTarget in @((Join-Path $taskStage 'SnipLeaf.exe'),$taskIdentity)){
        & $taskSignTool sign /fd SHA256 /sha1 $taskCertificate.Thumbprint /s My $taskTarget
        if($LASTEXITCODE -ne 0){throw 'Preview payload signing failed.'}
    }
    $taskRecords=@('SnipLeaf.exe','SnipLeaf.identity.msix','SnipLeaf-preview.cer')|ForEach-Object {@{name=$_;sha256=(Get-FileHash -LiteralPath (Join-Path $taskStage $_)).Hash}}
    @{packageName=$taskProduct.packageName;publisher=$taskProduct.publisher;certificateThumbprint=$taskCertificate.Thumbprint;previewOnly=$true;files=@($taskRecords)}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $taskStage 'installer-security.json') -Encoding UTF8
    & $taskCompiler ('/DStageDir='+$taskStage) ('/DProductVersion='+$taskProduct.version) (Join-Path $taskProjectRoot 'packaging\installer\SnipLeaf.iss')
    if($LASTEXITCODE -ne 0){throw 'EXE installer compilation failed.'}
    $taskInstaller=Join-Path $taskOut ('release\SnipLeaf-'+$taskProduct.version+'-x64-Preview-Setup.exe')
    & $taskSignTool sign /fd SHA256 /sha1 $taskCertificate.Thumbprint /s My $taskInstaller
    if($LASTEXITCODE -ne 0){throw 'Preview setup signing failed.'}
    $taskExpected=@('SnipLeaf.exe','SnipLeaf.identity.msix','SnipLeaf-preview.cer','installer-security.json','使用说明.txt','Assets/Logo44.png','Assets/Logo50.png','Assets/Logo150.png')
    $taskFiles=@(Get-ChildItem -LiteralPath $taskStage -File -Recurse|ForEach-Object {
        $taskRelative=$_.FullName.Substring($taskStage.Length+1).Replace('\','/')
        if($taskRelative -notin $taskExpected){throw ('Unexpected installer payload: '+$taskRelative)}
        @{path=$taskRelative;bytes=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName).Hash}
    })
    $taskHelpers=@('Deploy-App.ps1','Trust-PreviewCertificate.ps1','Initialize-OcrLanguages.ps1')|ForEach-Object {
        $taskHelper=Join-Path $taskProjectRoot ('packaging\installer\'+$_)
        @{path='Installer/'+$_;bytes=(Get-Item -LiteralPath $taskHelper).Length;sha256=(Get-FileHash -LiteralPath $taskHelper).Hash}
    }
    @{product=$taskProduct.displayName;version=$taskProduct.version;architecture='x64';previewOnly=$true;trustedProductionSignature=$false;privateKeyIncluded=$false;certificateThumbprint=$taskCertificate.Thumbprint;createdAt=(Get-Date -Format o);installer=@{path=$taskInstaller;bytes=(Get-Item -LiteralPath $taskInstaller).Length;sha256=(Get-FileHash -LiteralPath $taskInstaller).Hash};installedPayload=$taskFiles;installerHelpers=@($taskHelpers);generatedUninstallerIncluded=$true}|ConvertTo-Json -Depth 7|Set-Content -LiteralPath (Join-Path $taskOut ('release\SnipLeaf-'+$taskProduct.version+'-x64-Preview-manifest.json')) -Encoding UTF8
    Write-Output ('EXE setup: '+$taskInstaller)
}finally{
    if($taskCertificate){Remove-Item -LiteralPath ('Cert:\CurrentUser\My\'+$taskCertificate.Thumbprint) -DeleteKey -ErrorAction SilentlyContinue}
}
