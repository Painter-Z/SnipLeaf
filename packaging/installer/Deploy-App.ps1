[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('Install','Uninstall')][string]$Action,
    [Parameter(Mandatory)][string]$Directory,
    [switch]$TrustPreview,
    # Retained for older callers. System features are now prepared separately.
    [switch]$EnsureOcrLanguages,
    [string]$BackupIdentity
)
$ErrorActionPreference='Stop'
# Inno can inherit PSModulePath from a developer's PowerShell 7 session.
# Use only Windows PowerShell's own system modules in this child process.
if($PSVersionTable.PSEdition -eq 'Desktop'){$env:PSModulePath=Join-Path $PSHOME 'Modules'}
$taskDirectory=[IO.Path]::GetFullPath($Directory)
$taskStatePath=Join-Path $taskDirectory 'installation-state.json'
$taskLogPath=Join-Path $env:TEMP 'SnipLeaf-install-last.log'
$taskAddedUser=$false
$taskAddedMachine=$false
$taskRegistered=$false
$taskRemovedPrevious=$false
$taskIdentityVolume=$null
function Write-InstallLog([string]$Message){Add-Content -LiteralPath $taskLogPath -Value ((Get-Date -Format o)+' '+$Message) -Encoding UTF8}
function Get-SystemIdentityVolume {
    # The tiny identity package and its AppData must not inherit a secondary
    # default AppX volume. The native executable keeps its ExternalLocation.
    $taskVolumes=@(Get-AppxVolume -ErrorAction Stop|Where-Object {$_.IsSystemVolume -and -not $_.IsOffline})
    if($taskVolumes.Count -ne 1){throw 'A unique online system AppX volume could not be found. No package or certificate was changed.'}
    return $taskVolumes[0]
}
function Add-IdentityPackage([string]$Path) {
    if(-not $taskIdentityVolume){throw 'The system identity volume has not been selected.'}
    Add-AppxPackage -Path $Path -ExternalLocation $taskDirectory -Volume $taskIdentityVolume -ErrorAction Stop
}
function Write-DeploymentDiagnostics([System.Management.Automation.ErrorRecord]$Failure) {
    $taskActivityMatch=[regex]::Match($Failure.Exception.Message,'(?i)\[ActivityId\]\s*([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})')
    if(-not $taskActivityMatch.Success){return}
    $taskActivityId=$taskActivityMatch.Groups[1].Value
    Write-InstallLog ('Deployment ActivityId: '+$taskActivityId)
    # Filter in the event service instead of reading unrelated deployments.
    $taskXPath="*[System[Correlation[@ActivityID='{${taskActivityId}}']]]"
    foreach($taskChannel in @('Microsoft-Windows-AppXDeploymentServer/Operational','Microsoft-Windows-AppxPackaging/Operational')){
        try{
            Get-WinEvent -LogName $taskChannel -FilterXPath $taskXPath -Oldest -ErrorAction Stop|ForEach-Object {
                Write-InstallLog ('Deployment event '+$_.Id+' ['+$_.TimeCreated.ToString('o')+'] '+$_.Message)
            }
        }catch{Write-InstallLog ('Deployment diagnostics unavailable for '+$taskChannel+': '+$_.Exception.Message)}
    }
}
function Write-OcrSetupStatus($Check,[string]$Destination) {
    # Missing Windows features must not roll back a working application.
    # Query only the recognizers reported by the installed executable; no DISM,
    # network request or elevation belongs in this installation transaction.
    $taskLanguages=@($Check.languages|Where-Object {$_ -is [string] -and $_})
    $taskMissing=@()
    if(-not @($taskLanguages|Where-Object {$_ -match '^zh-(Hans(-CN)?|CN)$'}).Count){$taskMissing+='zh-CN'}
    if('en-US' -notin $taskLanguages){$taskMissing+='en-US'}
    $taskOcrReady=([bool]$Check.ocrReady -and $taskLanguages.Count -gt 0 -and -not $Check.languageCheckError)
    $taskOcrStatus=if($Check.languageCheckError){'check-failed'}elseif(-not $taskOcrReady){'unavailable'}elseif($taskMissing.Count){'partial'}else{'ready'}
    foreach($taskMarker in @('ocr-setup-needed.flag','ocr-unavailable.flag')){
        $taskMarkerPath=Join-Path $Destination $taskMarker
        $taskMarkerNeeded=if($taskMarker -eq 'ocr-setup-needed.flag'){$taskMissing.Count -gt 0 -or -not $taskOcrReady}else{-not $taskOcrReady}
        if($taskMarkerNeeded){Set-Content -LiteralPath $taskMarkerPath -Value $taskOcrStatus -Encoding UTF8}
        elseif(Test-Path -LiteralPath $taskMarkerPath){Remove-Item -LiteralPath $taskMarkerPath}
    }
    return [pscustomobject]@{status=$taskOcrStatus;ready=$taskOcrReady;languages=$taskLanguages;missingLanguages=$taskMissing;languageCheckError=$Check.languageCheckError}
}
function Invoke-MachineTrust([string]$Certificate,[string]$Thumbprint,[bool]$Remove){
    $taskHelper=Join-Path $PSScriptRoot 'Trust-PreviewCertificate.ps1'
    $taskArgs=@('-NoLogo','-NoProfile','-ExecutionPolicy','Bypass','-File',('"'+$taskHelper+'"'),'-CertificatePath',('"'+$Certificate+'"'),'-ExpectedThumbprint',$Thumbprint)
    if($Remove){$taskArgs+='-Remove'}
    $taskChild=Start-Process -FilePath (Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe') -Verb RunAs -WindowStyle Hidden -ArgumentList $taskArgs -PassThru
    $null=$taskChild.Handle
    if(-not $taskChild.WaitForExit(60000)){throw 'Certificate confirmation did not finish within 60 seconds. Retry installation after responding to Windows.'}
    if($taskChild.ExitCode -ne 0){throw 'The dedicated preview certificate confirmation was canceled or failed.'}
}
function Get-OwnedCertificates($State){
    if(-not $State){return @()}
    if($State.trustedCertificates){return @($State.trustedCertificates)}
    # Compatibility with the first local installer candidate.
    if($State.certificateThumbprint -eq $taskCertificate.Thumbprint){
        return @([pscustomobject]@{thumbprint=$taskCertificate.Thumbprint;userAdded=[bool]$State.userCertificateAdded;machineAdded=[bool]$State.machineCertificateAdded;publicCertificateBase64=[Convert]::ToBase64String($taskCertificate.RawData)})
    }
    throw 'Previous installation certificate ownership cannot be verified.'
}
function Remove-OwnedCertificate($Record){
    if($Record.thumbprint -notmatch '^[A-Fa-f0-9]{40}$'){throw 'Invalid certificate ownership record.'}
    $taskOwnedCert=[Security.Cryptography.X509Certificates.X509Certificate2]::new([Convert]::FromBase64String($Record.publicCertificateBase64))
    if($taskOwnedCert.HasPrivateKey -or $taskOwnedCert.Thumbprint -ne $Record.thumbprint -or $taskOwnedCert.Subject -ne 'CN=SnipLeaf Preview'){throw 'Certificate ownership validation failed.'}
    $taskOwnedMachine='Cert:\LocalMachine\TrustedPeople\'+$Record.thumbprint
    if($Record.machineAdded -and(Test-Path -LiteralPath $taskOwnedMachine)){
        $taskTemporary=Join-Path $env:TEMP ('SnipLeaf-uninstall-'+[Guid]::NewGuid().ToString('N')+'.cer')
        try{
            [IO.File]::WriteAllBytes($taskTemporary,$taskOwnedCert.RawData)
            Invoke-MachineTrust $taskTemporary $Record.thumbprint $true
        }finally{Remove-Item -LiteralPath $taskTemporary -ErrorAction SilentlyContinue}
    }
    $taskOwnedUser='Cert:\CurrentUser\TrustedPeople\'+$Record.thumbprint
    if($Record.userAdded -and(Test-Path -LiteralPath $taskOwnedUser)){Remove-Item -LiteralPath $taskOwnedUser}
}
try {
    Write-InstallLog ('Begin '+$Action+' '+$taskDirectory)
    Write-InstallLog ('PowerShell '+$PSVersionTable.PSVersion+'; 64-bit process: '+[Environment]::Is64BitProcess)
    $taskSecurityPath=Join-Path $taskDirectory 'installer-security.json'
    $taskSecurity=Get-Content -LiteralPath $taskSecurityPath -Raw -Encoding UTF8|ConvertFrom-Json
    if($taskSecurity.packageName -ne 'SnipLeaf.Preview' -or $taskSecurity.publisher -ne 'CN=SnipLeaf Preview'){throw 'Unexpected application identity.'}
    $taskIdentity=Join-Path $taskDirectory 'SnipLeaf.identity.msix'
    $taskCertificatePath=Join-Path $taskDirectory 'SnipLeaf-preview.cer'
    $taskCertificate=Get-PfxCertificate -FilePath $taskCertificatePath
    if($taskCertificate.HasPrivateKey -or $taskCertificate.Subject -ne $taskSecurity.publisher -or $taskCertificate.Thumbprint -ne $taskSecurity.certificateThumbprint){throw 'Preview certificate validation failed.'}
    $taskUserCertPath='Cert:\CurrentUser\TrustedPeople\'+$taskCertificate.Thumbprint
    $taskMachineCertPath='Cert:\LocalMachine\TrustedPeople\'+$taskCertificate.Thumbprint
    if($Action -eq 'Uninstall'){
        if(-not(Test-Path -LiteralPath $taskStatePath)){throw 'Installation ownership record is missing; refusing to unregister another installation.'}
        $taskState=Get-Content -LiteralPath $taskStatePath -Raw -Encoding UTF8|ConvertFrom-Json
        if($taskState.directory -ne $taskDirectory -or $taskState.certificateThumbprint -ne $taskCertificate.Thumbprint){throw 'Installation ownership record does not match.'}
        foreach($taskOwnedRecord in @(Get-OwnedCertificates $taskState)){Remove-OwnedCertificate $taskOwnedRecord}
        $taskInstalled=Get-AppxPackage -Name $taskSecurity.packageName
        if($taskInstalled -and $taskInstalled.PackageFullName -eq $taskState.packageFullName){Remove-AppxPackage -Package $taskInstalled.PackageFullName -ErrorAction Stop}
        Remove-Item -LiteralPath $taskStatePath
        Write-InstallLog 'Unregistered application identity and removed only installer-owned certificate trust. Settings retained.'
        exit 0
    }
    foreach($taskFile in $taskSecurity.files){
        if($taskFile.name -notin @('SnipLeaf.exe','SnipLeaf.identity.msix','SnipLeaf-preview.cer')){throw 'Unexpected payload path.'}
        $taskPayload=Join-Path $taskDirectory $taskFile.name
        if((Get-FileHash -LiteralPath $taskPayload -Algorithm SHA256).Hash -ne $taskFile.sha256){throw ('Payload verification failed: '+$taskFile.name)}
    }
    $taskSignature=Get-AuthenticodeSignature -LiteralPath $taskIdentity
    if(-not $taskSignature.SignerCertificate -or $taskSignature.SignerCertificate.Thumbprint -ne $taskCertificate.Thumbprint -or $taskSignature.Status -eq 'HashMismatch'){throw 'Identity package signature does not match the dedicated certificate.'}
    $taskOldState=$null
    if(Test-Path -LiteralPath $taskStatePath){$taskOldState=Get-Content -LiteralPath $taskStatePath -Raw -Encoding UTF8|ConvertFrom-Json}
    if($taskOldState -and $taskOldState.directory -ne $taskDirectory){throw 'Previous installation ownership record does not match this directory.'}
    $taskOwnedCertificates=@(Get-OwnedCertificates $taskOldState)
    $taskExisting=Get-AppxPackage -Name $taskSecurity.packageName
    if($taskExisting -and(-not $taskOldState -or $taskOldState.directory -ne $taskDirectory)){throw 'SnipLeaf is registered at another location. Uninstall that copy before choosing a different directory.'}
    # Select before removing an old identity or adding any certificate trust.
    $taskIdentityVolume=Get-SystemIdentityVolume
    Write-InstallLog ('Identity package volume: '+$taskIdentityVolume.PackageStorePath+'; external executable directory: '+$taskDirectory)
    try{Write-InstallLog ('Windows default AppX volume (unchanged): '+(Get-AppxDefaultVolume -ErrorAction Stop).PackageStorePath)}catch{Write-InstallLog ('Default AppX volume could not be queried: '+$_.Exception.Message)}
    if(-not(Test-Path -LiteralPath $taskMachineCertPath) -and -not(Test-Path -LiteralPath $taskUserCertPath)){
        if(-not $TrustPreview){throw 'This preview requires explicit consent to trust its dedicated public certificate.'}
        Import-Certificate -FilePath $taskCertificatePath -CertStoreLocation 'Cert:\CurrentUser\TrustedPeople'|Out-Null
        $taskAddedUser=$true
    }
    if($taskExisting){Remove-AppxPackage -Package $taskExisting.PackageFullName -ErrorAction Stop;$taskRemovedPrevious=$true}
    try{
        Add-IdentityPackage $taskIdentity
    }catch{
        if($_.Exception.Message -notmatch '0x800B0109' -or(Test-Path -LiteralPath $taskMachineCertPath) -or -not $TrustPreview){throw}
        Write-InstallLog 'Current-user trust was insufficient; requesting dedicated LocalMachine TrustedPeople confirmation.'
        Invoke-MachineTrust $taskCertificatePath $taskCertificate.Thumbprint $false
        $taskAddedMachine=$true
        Add-IdentityPackage $taskIdentity
    }
    $taskRegistered=$true
    $taskInstalled=Get-AppxPackage -Name $taskSecurity.packageName
    $taskCheckPath=Join-Path $taskDirectory 'installation-check.json'
    $taskCheck=Start-Process -FilePath (Join-Path $taskDirectory 'SnipLeaf.exe') -ArgumentList @('--install-check',('"'+$taskCheckPath+'"')) -WindowStyle Hidden -PassThru
    $null=$taskCheck.Handle
    if(-not $taskCheck.WaitForExit(15000) -or $taskCheck.ExitCode -ne 0){throw 'Installed application did not acquire its OCR identity.'}
    $taskResult=Get-Content -LiteralPath $taskCheckPath -Raw -Encoding UTF8|ConvertFrom-Json
    if(-not $taskResult.identityReady -or $taskResult.identity -ne $taskInstalled.PackageFullName){throw 'Installed application identity does not match the registered package.'}
    $taskOcrStatus=Write-OcrSetupStatus $taskResult $taskDirectory
    Write-InstallLog ('Application identity verified: '+$taskInstalled.PackageFullName)
    Write-InstallLog ('OCR status: '+$taskOcrStatus.status+'; available recognizers: '+($taskOcrStatus.languages -join ', ')+'; missing target languages: '+($taskOcrStatus.missingLanguages -join ', '))
    if($taskOcrStatus.languageCheckError){Write-InstallLog ('OCR recognizer query failed (installation retained): '+$taskOcrStatus.languageCheckError)}
    if($EnsureOcrLanguages){Write-InstallLog 'Legacy EnsureOcrLanguages request deferred. Use the independent, visible OCR language setup after installation.'}
    $taskCurrentRecord=@($taskOwnedCertificates|Where-Object {$_.thumbprint -eq $taskCertificate.Thumbprint})
    if($taskCurrentRecord.Count){
        $taskCurrentRecord[0].userAdded=([bool]$taskCurrentRecord[0].userAdded -or $taskAddedUser)
        $taskCurrentRecord[0].machineAdded=([bool]$taskCurrentRecord[0].machineAdded -or $taskAddedMachine)
    }else{
        $taskOwnedCertificates+=@([pscustomobject]@{thumbprint=$taskCertificate.Thumbprint;userAdded=$taskAddedUser;machineAdded=$taskAddedMachine;publicCertificateBase64=[Convert]::ToBase64String($taskCertificate.RawData)})
    }
    @{
        directory=$taskDirectory;packageFullName=$taskInstalled.PackageFullName;certificateThumbprint=$taskCertificate.Thumbprint;
        identityPackageStorePath=$taskIdentityVolume.PackageStorePath;
        trustedCertificates=$taskOwnedCertificates;
        installedAt=(Get-Date -Format o);previewOnly=$true;ocrLanguages=$taskOcrStatus.languages;ocrReady=$taskOcrStatus.ready;
        ocrStatus=$taskOcrStatus.status;missingOcrLanguages=$taskOcrStatus.missingLanguages;ocrLanguageCheckError=$taskOcrStatus.languageCheckError
    }|ConvertTo-Json -Depth 5|Set-Content -LiteralPath $taskStatePath -Encoding UTF8
    if(-not $taskOcrStatus.ready){Write-InstallLog 'OCR is currently unavailable. Screenshot, annotations and pins are installed; prepare Windows OCR languages separately.'}
    Write-InstallLog ('Installation verified: '+$taskInstalled.PackageFullName)
    exit 0
}catch{
    $taskFailure=$_
    Write-InstallLog ('ERROR: '+$taskFailure.Exception.Message)
    Write-DeploymentDiagnostics $taskFailure
    if($Action -eq 'Install'){
        if($taskRegistered){Get-AppxPackage -Name 'SnipLeaf.Preview'|Remove-AppxPackage -ErrorAction SilentlyContinue}
        if($taskRemovedPrevious -and $BackupIdentity -and(Test-Path -LiteralPath $BackupIdentity)){
            try{Add-IdentityPackage $BackupIdentity;Write-InstallLog 'Previous identity restored.'}catch{Write-InstallLog ('Previous identity restoration failed: '+$_.Exception.Message)}
        }
        if($taskAddedUser){Remove-Item -LiteralPath $taskUserCertPath -ErrorAction SilentlyContinue}
        if($taskAddedMachine){try{Invoke-MachineTrust $taskCertificatePath $taskCertificate.Thumbprint $true}catch{Write-InstallLog 'Dedicated machine certificate cleanup still requires confirmation.'}}
    }
    Write-Error -ErrorRecord $taskFailure -ErrorAction Continue
    exit 1
}
