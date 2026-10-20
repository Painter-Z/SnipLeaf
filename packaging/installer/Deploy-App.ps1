[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('Install','Repair','Uninstall')][string]$Action,
    [Parameter(Mandatory)][string]$Directory,
    [switch]$TrustPreview,
    # Retained for older callers. System features are now prepared separately.
    [switch]$EnsureOcrLanguages,
    [string]$BackupIdentity,
    [switch]$Interactive
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
$taskIdentityVolume=$null
$taskIdentityStagingRoot=Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'SnipLeaf\Deployment'
function Write-InstallLog([string]$Message){Add-Content -LiteralPath $taskLogPath -Value ((Get-Date -Format o)+' '+$Message) -Encoding UTF8}
function Get-IdentitySupport([version]$WindowsVersion=[Environment]::OSVersion.Version) {
    $taskVersion=$WindowsVersion
    if($taskVersion.Major -lt 10 -or $taskVersion.Build -lt 19041){return 'Windows OCR identity requires Windows 10 version 2004 (build 19041) or newer.'}
    $taskAddCommand=Get-Command Add-AppxPackage -ErrorAction SilentlyContinue
    if(-not $taskAddCommand -or -not $taskAddCommand.Parameters.ContainsKey('ExternalLocation')){return 'The Windows AppX module does not support external-location identity packages.'}
    return $null
}
function Get-SystemIdentityVolume {
    # The tiny identity package and its AppData must not inherit a secondary
    # default AppX volume. The native executable keeps its ExternalLocation.
    $taskVolumes=@(Get-AppxVolume -ErrorAction Stop|Where-Object {$_.IsSystemVolume -and -not $_.IsOffline})
    if($taskVolumes.Count -ne 1){throw 'A unique online system AppX volume could not be found. No package or certificate was changed.'}
    return $taskVolumes[0]
}
function Add-IdentityPackage([string]$Path) {
    if(-not $taskIdentityVolume){throw 'The system identity volume has not been selected.'}
    # AppXSvc must open the package itself. A custom install drive can be
    # readable to the interactive user but not to that service on Windows 10.
    # Stage only the tiny identity, inheriting the standard per-user directory
    # permissions. Never widen ACLs on the application's original directory.
    $taskStageDirectory=Join-Path $taskIdentityStagingRoot ([Guid]::NewGuid().ToString('N'))
    $taskStagePackage=Join-Path $taskStageDirectory 'SnipLeaf.identity.msix'
    [IO.Directory]::CreateDirectory($taskStageDirectory)|Out-Null
    try{
        Copy-Item -LiteralPath $Path -Destination $taskStagePackage -ErrorAction Stop
        if((Get-FileHash -LiteralPath $Path).Hash -ne (Get-FileHash -LiteralPath $taskStagePackage).Hash){throw 'Identity staging checksum mismatch.'}
        Write-InstallLog ('Identity source staged at '+$taskStagePackage+'; external executable directory unchanged: '+$taskDirectory)
        Add-AppxPackage -Path $taskStagePackage -ExternalLocation $taskDirectory -Volume $taskIdentityVolume -ErrorAction Stop
    }finally{
        # Remove only the exact file and empty GUID directory created above.
        if(Test-Path -LiteralPath $taskStagePackage){Remove-Item -LiteralPath $taskStagePackage -ErrorAction SilentlyContinue}
        if(Test-Path -LiteralPath $taskStageDirectory){Remove-Item -LiteralPath $taskStageDirectory -ErrorAction SilentlyContinue}
    }
}
function Get-ApplicationCheck {
    $taskCheckPath=Join-Path $taskDirectory 'installation-check.json'
    if(Test-Path -LiteralPath $taskCheckPath){Remove-Item -LiteralPath $taskCheckPath -ErrorAction Stop}
    $taskCheck=Start-Process -FilePath (Join-Path $taskDirectory 'SnipLeaf.exe') -ArgumentList @('--install-check',('"'+$taskCheckPath+'"')) -WindowStyle Hidden -PassThru
    $null=$taskCheck.Handle
    if(-not $taskCheck.WaitForExit(15000)){
        try{$taskCheck.Kill()}catch{}
        throw 'Application verification did not finish within 15 seconds.'
    }
    $taskCheck.WaitForExit()
    if($taskCheck.ExitCode -notin @(0,3)){throw ('Application verification failed with exit code '+$taskCheck.ExitCode+'.')}
    $taskResult=Get-Content -LiteralPath $taskCheckPath -Raw -Encoding UTF8|ConvertFrom-Json
    if(-not $taskResult.basicReady){throw 'Application did not confirm basic screenshot/graphics initialization.'}
    return $taskResult
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
    Write-InstallLog ('Windows '+[Environment]::OSVersion.Version+'; PowerShell '+$PSVersionTable.PSVersion+'; 64-bit process: '+[Environment]::Is64BitProcess)
    $taskSecurityPath=Join-Path $taskDirectory 'installer-security.json'
    $taskSecurity=Get-Content -LiteralPath $taskSecurityPath -Raw -Encoding UTF8|ConvertFrom-Json
    if($taskSecurity.packageName -ne 'SnipLeaf.Preview' -or $taskSecurity.publisher -ne 'CN=SnipLeaf Preview'){throw 'Unexpected application identity.'}
    $taskIdentity=Join-Path $taskDirectory 'SnipLeaf.identity.msix'
    $taskCertificatePath=Join-Path $taskDirectory 'SnipLeaf-preview.cer'
    $taskCertificate=[Security.Cryptography.X509Certificates.X509Certificate2]::new([IO.File]::ReadAllBytes($taskCertificatePath))
    if($taskCertificate.HasPrivateKey -or $taskCertificate.Subject -ne $taskSecurity.publisher -or $taskCertificate.Thumbprint -ne $taskSecurity.certificateThumbprint){throw 'Preview certificate validation failed.'}
    $taskUserCertPath='Cert:\CurrentUser\TrustedPeople\'+$taskCertificate.Thumbprint
    $taskMachineCertPath='Cert:\LocalMachine\TrustedPeople\'+$taskCertificate.Thumbprint
    if($Action -eq 'Uninstall'){
        if(-not(Test-Path -LiteralPath $taskStatePath)){throw 'Installation ownership record is missing; refusing to unregister another installation.'}
        $taskState=Get-Content -LiteralPath $taskStatePath -Raw -Encoding UTF8|ConvertFrom-Json
        if($taskState.directory -ne $taskDirectory -or $taskState.certificateThumbprint -ne $taskCertificate.Thumbprint){throw 'Installation ownership record does not match.'}
        if($taskState.packageFullName){
            $taskInstalled=Get-AppxPackage -Name $taskSecurity.packageName
            if($taskInstalled -and $taskInstalled.PackageFullName -eq $taskState.packageFullName){Remove-AppxPackage -Package $taskInstalled.PackageFullName -ErrorAction Stop}
        }
        foreach($taskOwnedRecord in @(Get-OwnedCertificates $taskState)){Remove-OwnedCertificate $taskOwnedRecord}
        Remove-Item -LiteralPath $taskStatePath
        Write-InstallLog 'Unregistered application identity and removed only installer-owned certificate trust. Settings retained.'
        exit 0
    }
    foreach($taskRequired in @('SnipLeaf.exe','SnipLeaf.identity.msix','SnipLeaf-preview.cer')){
        if(@($taskSecurity.files|Where-Object {$_.name -eq $taskRequired}).Count -ne 1){throw 'Payload manifest is incomplete or has duplicate entries.'}
    }
    foreach($taskFile in $taskSecurity.files){
        if($taskFile.name -notin @('SnipLeaf.exe','SnipLeaf.identity.msix','SnipLeaf-preview.cer')){throw 'Unexpected payload path.'}
        $taskPayload=Join-Path $taskDirectory $taskFile.name
        if((Get-FileHash -LiteralPath $taskPayload -Algorithm SHA256).Hash -ne $taskFile.sha256){throw ('Payload verification failed: '+$taskFile.name)}
    }
    $taskOldState=$null
    if(Test-Path -LiteralPath $taskStatePath){$taskOldState=Get-Content -LiteralPath $taskStatePath -Raw -Encoding UTF8|ConvertFrom-Json}
    if($taskOldState -and $taskOldState.directory -ne $taskDirectory){throw 'Previous installation ownership record does not match this directory.'}
    $taskOwnedCertificates=@(Get-OwnedCertificates $taskOldState)
    $taskInstalled=$null
    $taskIdentityStatus='deferred'
    $taskIdentityError=$null
    if($TrustPreview){
        try{
            $taskSupportError=Get-IdentitySupport
            if($taskSupportError){throw $taskSupportError}
            $taskSignature=Get-AuthenticodeSignature -LiteralPath $taskIdentity
            if(-not $taskSignature.SignerCertificate -or $taskSignature.SignerCertificate.Thumbprint -ne $taskCertificate.Thumbprint -or $taskSignature.Status -in @('HashMismatch','NotSigned')){throw 'Identity package signature does not match the dedicated certificate.'}
            $taskExisting=Get-AppxPackage -Name $taskSecurity.packageName
            if($taskExisting -and(-not $taskOldState -or $taskExisting.PackageFullName -ne $taskOldState.packageFullName)){throw 'SnipLeaf identity belongs to another installation. Uninstall that copy before repairing this directory.'}
            $taskIdentityVolume=Get-SystemIdentityVolume
            Write-InstallLog ('Identity package volume: '+$taskIdentityVolume.PackageStorePath+'; external executable directory: '+$taskDirectory)
            try{Write-InstallLog ('Windows default AppX volume (unchanged): '+(Get-AppxDefaultVolume -ErrorAction Stop).PackageStorePath)}catch{Write-InstallLog ('Default AppX volume could not be queried: '+$_.Exception.Message)}
            if(-not(Test-Path -LiteralPath $taskMachineCertPath) -and -not(Test-Path -LiteralPath $taskUserCertPath)){
                Import-Certificate -FilePath $taskCertificatePath -CertStoreLocation 'Cert:\CurrentUser\TrustedPeople'|Out-Null
                $taskAddedUser=$true
            }
            if(-not $taskExisting){
                try{Add-IdentityPackage $taskIdentity}
                catch{
                    if($_.Exception.Message -notmatch '0x800B0109' -or(Test-Path -LiteralPath $taskMachineCertPath)){throw}
                    Write-InstallLog 'Current-user trust was insufficient; requesting dedicated LocalMachine TrustedPeople confirmation.'
                    Invoke-MachineTrust $taskCertificatePath $taskCertificate.Thumbprint $false
                    $taskAddedMachine=$true
                    Add-IdentityPackage $taskIdentity
                }
                $taskRegistered=$true
            }
            $taskInstalled=Get-AppxPackage -Name $taskSecurity.packageName
            if(-not $taskInstalled){throw 'The identity package was not reported by Windows after registration.'}
            $taskIdentityStatus='registered'
        }catch{
            $taskIdentityStatus='failed'
            $taskIdentityError=$_.Exception.Message
            Write-InstallLog ('OCR identity preparation failed; retaining application files: '+$taskIdentityError)
            Write-DeploymentDiagnostics $_
        }
    }else{Write-InstallLog 'OCR identity registration deferred; no certificate or AppX changes requested.'}
    $taskBasicReady=$false
    $taskApplicationError=$null
    $taskResult=[pscustomobject]@{languages=@();ocrReady=$false;languageCheckError=$null;identityReady=$false}
    try{
        $taskResult=Get-ApplicationCheck
        $taskBasicReady=$true
        if($taskInstalled -and $taskResult.identityReady -and $taskResult.identity -eq $taskInstalled.PackageFullName){$taskIdentityStatus='ready'}
        elseif($taskIdentityStatus -eq 'registered'){
            $taskIdentityStatus='verification-failed'
            $taskIdentityError='Application identity does not match the registered package. Exit SnipLeaf and repair the OCR identity.'
        }
    }catch{
        $taskApplicationError=$_.Exception.Message
        Write-InstallLog ('Application execution check failed; installation retained with an explicit warning: '+$taskApplicationError)
    }
    if($taskIdentityStatus -ne 'ready'){
        $taskResult=[pscustomobject]@{languages=@();ocrReady=$false;languageCheckError=$taskIdentityError;identityReady=$false}
        Set-Content -LiteralPath (Join-Path $taskDirectory 'ocr-identity-needed.flag') -Value $taskIdentityStatus -Encoding UTF8
    }elseif(Test-Path -LiteralPath (Join-Path $taskDirectory 'ocr-identity-needed.flag')){Remove-Item -LiteralPath (Join-Path $taskDirectory 'ocr-identity-needed.flag')}
    if(-not $taskBasicReady){Set-Content -LiteralPath (Join-Path $taskDirectory 'application-check-needed.flag') -Value $taskApplicationError -Encoding UTF8}
    elseif(Test-Path -LiteralPath (Join-Path $taskDirectory 'application-check-needed.flag')){Remove-Item -LiteralPath (Join-Path $taskDirectory 'application-check-needed.flag')}
    $taskOcrStatus=Write-OcrSetupStatus $taskResult $taskDirectory
    Write-InstallLog ('Application identity status: '+$taskIdentityStatus+'; basic initialization verified: '+$taskBasicReady)
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
        identityPackageStorePath=$(if($taskIdentityVolume){$taskIdentityVolume.PackageStorePath}else{$null});
        identityStatus=$taskIdentityStatus;identityError=$taskIdentityError;basicReady=$taskBasicReady;applicationCheckError=$taskApplicationError;
        trustedCertificates=$taskOwnedCertificates;
        installedAt=(Get-Date -Format o);previewOnly=$true;ocrLanguages=$taskOcrStatus.languages;ocrReady=$taskOcrStatus.ready;
        ocrStatus=$taskOcrStatus.status;missingOcrLanguages=$taskOcrStatus.missingLanguages;ocrLanguageCheckError=$taskOcrStatus.languageCheckError
    }|ConvertTo-Json -Depth 5|Set-Content -LiteralPath $taskStatePath -Encoding UTF8
    if($taskIdentityStatus -ne 'ready'){Write-InstallLog 'OCR identity is not ready. Repair the identity from SnipLeaf settings before preparing missing OCR languages.'}
    elseif(-not $taskOcrStatus.ready){Write-InstallLog 'OCR identity is ready but no usable recognizer was reported. Prepare Windows OCR languages separately.'}
    Write-InstallLog ('Application files installed; identity status '+$taskIdentityStatus+'.')
    if(Test-Path -LiteralPath (Join-Path $taskDirectory 'deployment-helper-needed.flag')){Remove-Item -LiteralPath (Join-Path $taskDirectory 'deployment-helper-needed.flag')}
    if($Interactive){
        if(-not $taskBasicReady){Write-Host ('程序启动检查未通过，暂不能确认截图或 OCR 可用：'+$taskApplicationError+'。请查看 '+$taskLogPath) -ForegroundColor Yellow}
        elseif($taskIdentityStatus -eq 'ready'){Write-Host 'OCR 应用身份已就绪。请退出并重新启动 SnipLeaf；缺少语言时再配置 OCR 语言。' -ForegroundColor Green}
        else{Write-Host ('OCR 身份尚未就绪：'+$taskIdentityError+'。基础安装已保留，请查看 '+$taskLogPath) -ForegroundColor Yellow}
        Read-Host '按 Enter 关闭窗口'|Out-Null
    }
    exit 0
}catch{
    $taskFailure=$_
    Write-InstallLog ('ERROR: '+$taskFailure.Exception.Message)
    Write-DeploymentDiagnostics $taskFailure
    if($Action -in @('Install','Repair')){
        if($taskRegistered -and $taskInstalled){Remove-AppxPackage -Package $taskInstalled.PackageFullName -ErrorAction SilentlyContinue}
        if($taskAddedUser){Remove-Item -LiteralPath $taskUserCertPath -ErrorAction SilentlyContinue}
        if($taskAddedMachine){try{Invoke-MachineTrust $taskCertificatePath $taskCertificate.Thumbprint $true}catch{Write-InstallLog 'Dedicated machine certificate cleanup still requires confirmation.'}}
    }
    Write-Error -ErrorRecord $taskFailure -ErrorAction Continue
    if($Interactive){Read-Host ('修复未完成，日志位于 '+$taskLogPath+'。按 Enter 关闭窗口')|Out-Null}
    exit 10
}
