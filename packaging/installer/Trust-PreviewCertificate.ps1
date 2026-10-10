[CmdletBinding()]
param([Parameter(Mandatory)][string]$CertificatePath,[Parameter(Mandatory)][string]$ExpectedThumbprint,[switch]$Remove)
$ErrorActionPreference='Stop'
if($PSVersionTable.PSEdition -eq 'Desktop'){$env:PSModulePath=Join-Path $PSHOME 'Modules'}
try {
    if($ExpectedThumbprint -notmatch '^[A-Fa-f0-9]{40}$'){throw 'Invalid certificate thumbprint.'}
    $taskCertificate=Get-PfxCertificate -FilePath $CertificatePath
    if($taskCertificate.HasPrivateKey -or $taskCertificate.Subject -ne 'CN=SnipLeaf Preview' -or $taskCertificate.Thumbprint -ne $ExpectedThumbprint){throw 'The public preview certificate does not match this installer.'}
    $taskCa=@($taskCertificate.Extensions|Where-Object {$_.Oid.Value -eq '2.5.29.19'})
    $taskUsage=@($taskCertificate.Extensions|Where-Object {$_.Oid.Value -eq '2.5.29.37'})
    if($taskCa.Count -ne 1 -or $taskCa[0].CertificateAuthority -or $taskUsage.Count -ne 1 -or @($taskUsage[0].EnhancedKeyUsages|Where-Object Value -eq '1.3.6.1.5.5.7.3.3').Count -ne 1){throw 'A non-CA code signing certificate is required.'}
    $taskStorePath='Cert:\LocalMachine\TrustedPeople\'+$ExpectedThumbprint
    if($Remove){
        if(Test-Path -LiteralPath $taskStorePath){Remove-Item -LiteralPath $taskStorePath}
    }else{
        Import-Certificate -FilePath $CertificatePath -CertStoreLocation 'Cert:\LocalMachine\TrustedPeople'|Out-Null
    }
    exit 0
}catch{Write-Error $_;exit 1}
