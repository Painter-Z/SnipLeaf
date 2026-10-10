[CmdletBinding()]
param([switch]$TrustMachine)
$ErrorActionPreference='Stop'
$taskProjectRoot=Split-Path -Parent $PSScriptRoot
$taskExternal=(Resolve-Path -LiteralPath (Join-Path $taskProjectRoot 'build\Release')).Path
$taskStatePath=Join-Path $taskProjectRoot 'reports\registration.local.json'
$taskExisting=Get-AppxPackage -Name 'SnapLite.Development'
if($taskExisting -and (Test-Path -LiteralPath $taskStatePath)){
    $taskState=Get-Content -LiteralPath $taskStatePath -Raw|ConvertFrom-Json
    if($taskState.externalLocation -eq $taskExternal){Write-Output 'SnapLite development identity is already registered.';return}
}
if($taskExisting){throw 'SnapLite is registered at another location. Run Unregister-DevPackage.ps1 before changing it.'}
$taskPackage=Join-Path $taskProjectRoot 'out\SnapLite-identity.msix'
$taskPublicCertificate=Join-Path $taskProjectRoot 'out\SnapLite-development.cer'
$taskCertificate=$null
$taskReady=$false
if((Test-Path -LiteralPath $taskPackage)-and(Test-Path -LiteralPath $taskPublicCertificate)){
    $taskSignature=Get-AuthenticodeSignature -FilePath $taskPackage
    $taskPublic=Get-PfxCertificate -FilePath $taskPublicCertificate
    $taskReady=$taskSignature.SignerCertificate -and $taskSignature.SignerCertificate.Thumbprint -eq $taskPublic.Thumbprint -and $taskPublic.Subject -eq 'CN=SnapLite Local Development'
}
if(-not $taskReady){
    & (Join-Path $PSScriptRoot 'Package.ps1') -IdentityOnly
    Copy-Item -LiteralPath (Join-Path $taskProjectRoot 'out\SnapLite-identity-unsigned.msix') -Destination $taskPackage
    . (Join-Path $PSScriptRoot 'Find-WindowsSdk.ps1')
    $taskSignTool=Find-TaskWindowsSdkTool -Tool 'signtool.exe'
    try{
        $taskCertParameters=@{
            Type='Custom';Subject='CN=SnapLite Local Development';FriendlyName='SnapLite local development only'
            KeyAlgorithm='RSA';KeyLength=2048;KeyUsage='DigitalSignature';KeyExportPolicy='NonExportable'
            TextExtension=@('2.5.29.37={text}1.3.6.1.5.5.7.3.3','2.5.29.19={text}CA=false')
            CertStoreLocation='Cert:\CurrentUser\My';NotAfter=(Get-Date).AddYears(1)
        }
        $taskCertificate=New-SelfSignedCertificate @taskCertParameters
        Export-Certificate -Cert $taskCertificate -FilePath $taskPublicCertificate|Out-Null
        & $taskSignTool sign /fd SHA256 /sha1 $taskCertificate.Thumbprint /s My $taskPackage
        if($LASTEXITCODE -ne 0){throw 'Development package signing failed.'}
    }finally{
        if($taskCertificate){Remove-Item -LiteralPath ('Cert:\CurrentUser\My\'+$taskCertificate.Thumbprint) -DeleteKey -ErrorAction SilentlyContinue}
    }
}
[IO.Directory]::CreateDirectory((Split-Path -Parent $taskStatePath))|Out-Null
$taskPublic=Get-PfxCertificate -FilePath $taskPublicCertificate
$taskStore='LocalMachine\TrustedPeople'
$taskMachineTrusted=Test-Path -LiteralPath ('Cert:\LocalMachine\TrustedPeople\'+$taskPublic.Thumbprint)
if(-not $taskMachineTrusted -and $TrustMachine){
    $taskHelper=Join-Path $PSScriptRoot 'Trust-DevCertificate.ps1'
    $taskProcess=Start-Process -FilePath 'powershell.exe' -Verb RunAs -WindowStyle Hidden -PassThru -Wait -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File',('"'+$taskHelper+'"'))
    if($taskProcess.ExitCode -ne 0){throw 'The administrator certificate-trust step was canceled or failed.'}
    $taskMachineTrusted=Test-Path -LiteralPath ('Cert:\LocalMachine\TrustedPeople\'+$taskPublic.Thumbprint)
}
if(-not $taskMachineTrusted){
    $taskStore='CurrentUser\TrustedPeople'
    Import-Certificate -FilePath $taskPublicCertificate -CertStoreLocation 'Cert:\CurrentUser\TrustedPeople'|Out-Null
}
try{
    Add-AppxPackage -Path $taskPackage -ExternalLocation $taskExternal -ErrorAction Stop
}catch{
    if(-not $taskMachineTrusted){Remove-Item -LiteralPath ('Cert:\CurrentUser\TrustedPeople\'+$taskPublic.Thumbprint) -ErrorAction SilentlyContinue}
    throw ('Windows rejected package registration: '+$_.Exception.Message+' Use -TrustMachine for the dedicated test-certificate administrator confirmation.')
}
$taskPackageInfo=Get-AppxPackage -Name 'SnapLite.Development'
@{
    packageName=$taskPackageInfo.Name;packageFullName=$taskPackageInfo.PackageFullName;externalLocation=$taskExternal
    certificateThumbprint=$taskPublic.Thumbprint;certificateStore=$taskStore
    registeredAt=(Get-Date).ToString('o');developmentOnly=$true
}|ConvertTo-Json|Set-Content -LiteralPath $taskStatePath -Encoding utf8
Write-Output ('Registered identity: '+$taskPackageInfo.PackageFullName)
