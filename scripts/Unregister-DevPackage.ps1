[CmdletBinding()]
param([switch]$KeepCertificate)
$ErrorActionPreference='Stop'
$taskProjectRoot=Split-Path -Parent $PSScriptRoot
$taskStatePath=Join-Path $taskProjectRoot 'reports\registration.local.json'
$taskExisting=Get-AppxPackage -Name 'SnapLite.Development'
if($taskExisting){Remove-AppxPackage -Package $taskExisting.PackageFullName -ErrorAction Stop}
if(Test-Path -LiteralPath $taskStatePath){
    $taskState=Get-Content -LiteralPath $taskStatePath -Raw|ConvertFrom-Json
    $taskThumb=[string]$taskState.certificateThumbprint
    if(-not $KeepCertificate -and $taskThumb -match '^[0-9A-Fa-f]{40}$'){
        if($taskState.certificateStore -eq 'LocalMachine\TrustedPeople'){
            $taskCert=Get-Item -LiteralPath ('Cert:\LocalMachine\TrustedPeople\'+$taskThumb) -ErrorAction SilentlyContinue
            if($taskCert -and $taskCert.Subject -eq 'CN=SnapLite Local Development'){
                $taskHelper=Join-Path $PSScriptRoot 'Trust-DevCertificate.ps1'
                $taskProcess=Start-Process -FilePath 'powershell.exe' -Verb RunAs -WindowStyle Hidden -PassThru -Wait -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File',('"'+$taskHelper+'"'),'-Remove')
                if($taskProcess.ExitCode -ne 0){throw 'Certificate removal was canceled. The package is unregistered; rerun this script to finish certificate cleanup.'}
            }
        }
        $taskUserPath='Cert:\CurrentUser\TrustedPeople\'+$taskThumb
        $taskUserCert=Get-Item -LiteralPath $taskUserPath -ErrorAction SilentlyContinue
        if($taskUserCert -and $taskUserCert.Subject -eq 'CN=SnapLite Local Development'){Remove-Item -LiteralPath $taskUserPath}
    }
    if(-not $KeepCertificate){Remove-Item -LiteralPath $taskStatePath}
}
Write-Output 'SnapLite development identity was removed. Project files, user settings and Windows OCR languages were retained.'
