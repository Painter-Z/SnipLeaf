[CmdletBinding()]
param([switch]$Remove)
$ErrorActionPreference='Stop'
$taskProjectRoot=Split-Path -Parent $PSScriptRoot
$taskCertificatePath=Join-Path $taskProjectRoot 'out\SnapLite-development.cer'
$taskPackagePath=Join-Path $taskProjectRoot 'out\SnapLite-identity.msix'
$taskStatus=Join-Path $taskProjectRoot 'reports\certificate-trust.local.json'
$taskPrincipal=[Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if(-not $taskPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'Administrator confirmation is required for the machine Trusted People store.'}
try{
    $taskCert=Get-PfxCertificate -FilePath $taskCertificatePath
    $taskSignature=Get-AuthenticodeSignature -FilePath $taskPackagePath
    if($Remove -and(Test-Path -LiteralPath (Join-Path $taskProjectRoot 'reports\registration.local.json'))){
        $taskRegistration=Get-Content -LiteralPath (Join-Path $taskProjectRoot 'reports\registration.local.json') -Raw|ConvertFrom-Json
        $taskThumb=[string]$taskRegistration.certificateThumbprint
        if($taskThumb -notmatch '^[0-9A-Fa-f]{40}$'){throw 'Invalid recorded development certificate thumbprint.'}
        $taskCert=Get-Item -LiteralPath ('Cert:\LocalMachine\TrustedPeople\'+$taskThumb) -ErrorAction Stop
    }
    if($taskCert.Subject -ne 'CN=SnapLite Local Development' -or $taskCert.HasPrivateKey -or
        (-not $Remove -and $taskSignature.SignerCertificate.Thumbprint -ne $taskCert.Thumbprint)){
        throw 'The dedicated public development certificate does not match the signed SnapLite identity package.'
    }
    if($Remove){
        $taskExisting=Get-Item -LiteralPath ('Cert:\LocalMachine\TrustedPeople\'+$taskCert.Thumbprint) -ErrorAction SilentlyContinue
        if($taskExisting -and $taskExisting.Subject -eq $taskCert.Subject){Remove-Item -LiteralPath $taskExisting.PSPath}
    }else{
        Import-Certificate -FilePath $taskCertificatePath -CertStoreLocation 'Cert:\LocalMachine\TrustedPeople'|Out-Null
    }
    @{success=$true;removed=[bool]$Remove;thumbprint=$taskCert.Thumbprint;store='LocalMachine\TrustedPeople';time=(Get-Date).ToString('o')}|
        ConvertTo-Json|Set-Content -LiteralPath $taskStatus -Encoding utf8
}catch{
    @{success=$false;error=$_.Exception.Message;time=(Get-Date).ToString('o')}|ConvertTo-Json|Set-Content -LiteralPath $taskStatus -Encoding utf8
    throw
}
