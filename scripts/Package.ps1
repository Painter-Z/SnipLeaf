[CmdletBinding()]
param([switch]$IdentityOnly)
$ErrorActionPreference='Stop'
$taskProjectRoot=Split-Path -Parent $PSScriptRoot
$taskExe=Join-Path $taskProjectRoot 'build\Release\SnapLite.exe'
if(-not(Test-Path -LiteralPath $taskExe)){throw 'Run scripts\Build.ps1 first.'}
. (Join-Path $PSScriptRoot 'Find-WindowsSdk.ps1')
$taskMakeAppx=Find-TaskWindowsSdkTool -Tool 'makeappx.exe'
$taskOut=Join-Path $taskProjectRoot 'out'
[IO.Directory]::CreateDirectory($taskOut)|Out-Null
$taskTemplate=Join-Path $taskProjectRoot 'packaging\AppxManifest.xml'
$taskIdentityDir=Join-Path $taskOut 'identity'
[IO.Directory]::CreateDirectory((Join-Path $taskIdentityDir 'Assets'))|Out-Null
foreach($taskSize in @(44,50,150)){
    Copy-Item -LiteralPath (Join-Path $taskProjectRoot "resources\Assets\Logo$taskSize.png") -Destination (Join-Path $taskIdentityDir "Assets\Logo$taskSize.png")
}
[xml]$taskIdentityXml=Get-Content -LiteralPath $taskTemplate -Raw
$taskVersion=$taskIdentityXml.Package.Identity.Version -replace '\.\d+$',''
$taskIdentityXml.Package.Identity.ProcessorArchitecture='neutral'
$taskExternal=$taskIdentityXml.CreateElement('uap10','AllowExternalContent','http://schemas.microsoft.com/appx/manifest/uap/windows10/10')
$taskExternal.InnerText='true'
$taskIdentityXml.Package.Properties.AppendChild($taskExternal)|Out-Null
$taskIdentityXml.Package.Applications.Application.VisualElements.SetAttribute('AppListEntry','none')
$taskIdentityXml.Save((Join-Path $taskIdentityDir 'AppxManifest.xml'))
$taskIdentityPackage=Join-Path $taskOut 'SnapLite-identity-unsigned.msix'
& $taskMakeAppx pack /o /d $taskIdentityDir /nv /p $taskIdentityPackage
if($LASTEXITCODE -ne 0){throw 'Identity package creation failed.'}
if(-not $IdentityOnly){
    $taskFullDir=Join-Path $taskOut 'package'
    [IO.Directory]::CreateDirectory((Join-Path $taskFullDir 'Assets'))|Out-Null
    Copy-Item -LiteralPath $taskTemplate -Destination (Join-Path $taskFullDir 'AppxManifest.xml')
    Copy-Item -LiteralPath $taskExe -Destination (Join-Path $taskFullDir 'SnapLite.exe')
    foreach($taskSize in @(44,50,150)){
        Copy-Item -LiteralPath (Join-Path $taskProjectRoot "resources\Assets\Logo$taskSize.png") -Destination (Join-Path $taskFullDir "Assets\Logo$taskSize.png")
    }
    $taskExpected=@('SnapLite.exe','AppxManifest.xml','Assets/Logo44.png','Assets/Logo50.png','Assets/Logo150.png')
    $taskFileRecords=@(Get-ChildItem -LiteralPath $taskFullDir -File -Recurse | Sort-Object FullName | ForEach-Object {
        $taskRelative=$_.FullName.Substring($taskFullDir.Length+1).Replace('\','/')
        if($taskRelative -notin $taskExpected){throw ('Unexpected file in package staging directory: '+$taskRelative)}
        @{path=$taskRelative;bytes=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash}
    })
    $taskFullPackage=Join-Path $taskOut ('SnapLite-'+$taskVersion+'-x64-unsigned.msix')
    & $taskMakeAppx pack /o /d $taskFullDir /p $taskFullPackage
    if($LASTEXITCODE -ne 0){throw 'Full MSIX creation failed.'}
    $taskManifest=Join-Path $taskOut ('SnapLite-'+$taskVersion+'-x64-manifest.json')
    @{
        product='SnapLite';version=$taskVersion;architecture='x64';signing='unsigned development candidate';
        createdAt=(Get-Date -Format o);minimumWindowsVersion='10.0.19041.0';
        validatedEnvironment='Windows 11 x64 local development machine';
        prerequisites=@('Windows OCR recognizer language components for the selected languages','Trusted package signature for distribution');
        package=@{path=(Split-Path -Leaf $taskFullPackage);bytes=(Get-Item -LiteralPath $taskFullPackage).Length;sha256=(Get-FileHash -LiteralPath $taskFullPackage -Algorithm SHA256).Hash};
        installedFiles=$taskFileRecords;installedBytes=($taskFileRecords|Measure-Object -Property bytes -Sum).Sum
    } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $taskManifest -Encoding utf8
    Write-Output ('Unsigned distribution candidate: '+$taskFullPackage)
    Write-Output ('File inventory and SHA256: '+$taskManifest)
}
Write-Output ('Unsigned identity package: '+$taskIdentityPackage)
