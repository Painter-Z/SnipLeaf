[CmdletBinding()]
param([string]$ReportPath)
$ErrorActionPreference='Stop'
$taskProjectRoot=Split-Path -Parent $PSScriptRoot
$taskProduct=Get-Content -LiteralPath (Join-Path $taskProjectRoot 'packaging\Product.json') -Raw -Encoding UTF8|ConvertFrom-Json
$taskFixtureRoot=Join-Path $taskProjectRoot ('out\validation\installer-prerequisites\'+$taskProduct.version+'\'+[Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($taskFixtureRoot)|Out-Null
$taskSource=Join-Path $taskProjectRoot 'packaging\installer\Deploy-App.ps1'
$taskPreparationSource=Join-Path $taskProjectRoot 'packaging\installer\Initialize-OcrLanguages.ps1'
$taskCertificate=Get-ChildItem -LiteralPath (Join-Path $taskProjectRoot 'out\installer-stage') -Filter 'SnipLeaf-preview.cer' -Recurse -File|Select-Object -First 1
if(-not $taskCertificate){throw 'A previously built public preview certificate is required for the read-only payload fixture.'}
$taskThumbprint=(Get-PfxCertificate -FilePath $taskCertificate.FullName).Thumbprint
$taskWindowsPowerShell=Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
$taskCases=[Collections.Generic.List[object]]::new()
function Assert-Task([bool]$Condition,[string]$Message){if(-not $Condition){throw $Message}}
function Check-Task([string]$Name,[scriptblock]$Body){
    try{& $Body;$taskCases.Add([pscustomobject]@{name=$Name;passed=$true})}
    catch{$taskCases.Add([pscustomobject]@{name=$Name;passed=$false;error=$_.Exception.Message})}
}

# A separate Windows PowerShell 5.1 process runs the actual deployment script.
# Every OS boundary is mocked and fails closed: no elevation, certificates,
# AppX registration, system features, real executable or user's logs are used.
$taskMockHost=@'
param([string]$Source,[string]$Fixture)
$ErrorActionPreference='Stop'
$global:snipLeafTestFixture=$Fixture
$global:snipLeafTestScenario=Get-Content -LiteralPath (Join-Path $Fixture 'scenario.json') -Raw -Encoding UTF8|ConvertFrom-Json
$global:snipLeafTestRegistered=$false
$global:snipLeafTestCalls=[Collections.Generic.List[object]]::new()
if($global:snipLeafTestScenario.existingOwned -or $global:snipLeafTestScenario.existingForeign){$global:snipLeafTestRegistered=$true}
$env:TEMP=Join-Path $Fixture 'Temp'
[IO.Directory]::CreateDirectory($env:TEMP)|Out-Null
function Test-Path {
    [CmdletBinding()]param([string]$LiteralPath,[string]$Path,[string]$PathType='Any')
    $p=if($LiteralPath){$LiteralPath}else{$Path}
    if($p -like 'Cert:*'){return (-not $global:snipLeafTestScenario.certificateDenied)}
    Microsoft.PowerShell.Management\Test-Path -LiteralPath $p -PathType $PathType
}
function Import-Certificate {
    [CmdletBinding()]param([string]$FilePath,[string]$CertStoreLocation)
    if($global:snipLeafTestScenario.certificateDenied){throw 'Simulated certificate access denied'}
    throw 'TEST GUARD: certificate import was attempted.'
}
function Get-AuthenticodeSignature {
    [CmdletBinding()]param([string]$LiteralPath)
    [pscustomobject]@{Status='Valid';SignerCertificate=(Get-PfxCertificate -FilePath (Join-Path $global:snipLeafTestFixture 'SnipLeaf-preview.cer'))}
}
function Get-AppxVolume {
    [CmdletBinding()]param()
    [pscustomobject]@{PackageStorePath='C:\Program Files\WindowsApps';IsSystemVolume=$true;IsOffline=$false}
}
function Get-AppxDefaultVolume {
    [CmdletBinding()]param()
    [pscustomobject]@{PackageStorePath='E:\WindowsApps';IsSystemVolume=$false;IsOffline=$false}
}
function Set-AppxDefaultVolume {throw 'TEST GUARD: default app volume mutation was attempted.'}
function Get-AppxPackage {
    [CmdletBinding()]param([string]$Name)
    if($global:snipLeafTestRegistered){[pscustomobject]@{PackageFullName='SnipLeaf.Preview_TEST_neutral__example'}}
}
function Add-AppxPackage {
    [CmdletBinding()]param([string]$Path,[string]$ExternalLocation,$Volume)
    $global:snipLeafTestCalls.Add([pscustomobject]@{action='register';volume=$Volume.PackageStorePath;externalLocation=$ExternalLocation})
    if($Path -eq (Join-Path $ExternalLocation 'SnipLeaf.identity.msix') -or -not(Test-Path -LiteralPath $Path)){throw 'TEST GUARD: service was not given a readable staging copy.'}
    if((Get-FileHash -LiteralPath $Path).Hash -ne (Get-FileHash -LiteralPath (Join-Path $ExternalLocation 'SnipLeaf.identity.msix')).Hash){throw 'TEST GUARD: staged bytes differ.'}
    if($global:snipLeafTestScenario.accessDenied){throw '0x80070005: simulated service cannot open identity [ActivityId] ab6fbdcc-5897-0006-153e-70ab9758dd01'}
    $global:snipLeafTestRegistered=$true
}
function Remove-AppxPackage {
    [CmdletBinding()]param([string]$Package,[Parameter(ValueFromPipeline=$true)]$InputObject)
    process{$global:snipLeafTestCalls.Add([pscustomobject]@{action='remove'});$global:snipLeafTestRegistered=$false}
}
function Get-WindowsCapability {throw 'TEST GUARD: installer queried servicing instead of application recognizers.'}
function Add-WindowsCapability {throw 'TEST GUARD: installer tried downloading a Windows feature.'}
function Get-WinEvent {[CmdletBinding()]param([string]$LogName,[string]$FilterXPath,[switch]$Oldest) throw 'Mock event channel unavailable'}
function Start-Process {
    [CmdletBinding()]param([string]$FilePath,[string[]]$ArgumentList,[string]$WindowStyle,[switch]$PassThru,[string]$Verb)
    if($Verb -or [IO.Path]::GetFileName($FilePath) -ne 'SnipLeaf.exe' -or $ArgumentList[0] -ne '--install-check'){
        throw 'TEST GUARD: a real or elevated child process was attempted.'
    }
    $global:snipLeafTestCalls.Add([pscustomobject]@{action='check'})
    $scenario=$global:snipLeafTestScenario
    if($scenario.applicationBlocked){throw 'Simulated application control policy blocked executable'}
    $identityReady=$scenario.identityReady -and $global:snipLeafTestRegistered -and -not $scenario.existingForeign
    @{basicReady=$true;identityReady=$identityReady;identity=$(if($identityReady){'SnipLeaf.Preview_TEST_neutral__example'}else{''});ocrReady=$scenario.ocrReady;languages=@($scenario.languages);languageCheckError=$scenario.languageCheckError}|
        ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $global:snipLeafTestFixture 'installation-check.json') -Encoding UTF8
    $child=[pscustomobject]@{Handle=1;ExitCode=$(if($identityReady){0}else{3})}
    $child|Add-Member -MemberType ScriptMethod -Name WaitForExit -Value {param($Milliseconds) if($Milliseconds){return (-not $global:snipLeafTestScenario.timeout)}}
    $child|Add-Member -MemberType ScriptMethod -Name Kill -Value {$global:snipLeafTestCalls.Add([pscustomobject]@{action='kill-check'})}
    return $child
}
if($global:snipLeafTestScenario.unsupportedApi){
    function Add-AppxPackage {[CmdletBinding()]param([string]$Path) throw 'TEST GUARD: unsupported module was used.'}
}
# Redirect only the per-user staging directory to the test fixture. The actual
# deployment body and all helpers execute unchanged under Windows PowerShell.
$text=[IO.File]::ReadAllText($Source)
$old='$taskIdentityStagingRoot=Join-Path ([Environment]::GetFolderPath(''LocalApplicationData'')) ''SnipLeaf\Deployment'''
if(($text.Split(@($old),[StringSplitOptions]::None)).Count -ne 2){throw 'TEST GUARD: staging substitution no longer matches exactly once.'}
$text=$text.Replace($old,'$taskIdentityStagingRoot=Join-Path $taskDirectory ''staging cache''')
$copy=Join-Path $Fixture 'Deploy-under-test.ps1'
[IO.File]::WriteAllText($copy,$text,[Text.UTF8Encoding]::new($true))
$options=@{Action='Install';Directory=$Fixture;EnsureOcrLanguages=$true}
if(-not $global:snipLeafTestScenario.noTrust){$options.TrustPreview=$true}
if($global:snipLeafTestScenario.repair){$options.Action='Repair'}
& $copy @options
$result=$LASTEXITCODE
@{exitCode=$result;registered=$global:snipLeafTestRegistered;calls=@($global:snipLeafTestCalls.ToArray())}|
    ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $Fixture 'trace.json') -Encoding UTF8
exit $result
'@
$taskMockPath=Join-Path $taskFixtureRoot 'mock-host.ps1'
[IO.File]::WriteAllText($taskMockPath,$taskMockHost,[Text.UTF8Encoding]::new($true))
$taskInstallScenarios=@(
    @{name='Both OCR targets already present';languages=@('zh-Hans-CN','en-US');ocrReady=$true;identityReady=$true;expectedStatus='ready';identityStatus='ready';basicReady=$true;registered=$true;needs=$false;unavailable=$false;exitCode=0},
    @{name='Chinese alone installs successfully without waiting for English';languages=@('zh-Hans-CN');ocrReady=$true;identityReady=$true;expectedStatus='partial';identityStatus='ready';basicReady=$true;registered=$true;needs=$true;unavailable=$false;exitCode=0},
    @{name='No OCR recognizers retain screenshot application';languages=@();ocrReady=$false;identityReady=$true;expectedStatus='unavailable';identityStatus='ready';basicReady=$true;registered=$true;needs=$true;unavailable=$true;exitCode=0},
    @{name='Recognizer enumeration error is recorded without rollback';languages=@();ocrReady=$false;identityReady=$true;languageCheckError='Simulated optional-feature API failure';expectedStatus='check-failed';identityStatus='ready';basicReady=$true;registered=$true;needs=$true;unavailable=$true;exitCode=0},
    @{name='Traditional Chinese does not masquerade as Simplified Chinese';languages=@('zh-Hant-TW','en-US');ocrReady=$true;identityReady=$true;expectedStatus='partial';identityStatus='ready';basicReady=$true;registered=$true;needs=$true;unavailable=$false;exitCode=0},
    @{name='Missing application identity retains basic installation with repair notice';languages=@();ocrReady=$false;identityReady=$false;expectedStatus='check-failed';identityStatus='verification-failed';basicReady=$true;registered=$true;needs=$true;unavailable=$true;exitCode=0},
    @{name='Verification timeout retains files and does not claim basic readiness';languages=@();ocrReady=$false;identityReady=$true;timeout=$true;expectedStatus='unavailable';identityStatus='registered';basicReady=$false;registered=$true;needs=$true;unavailable=$true;exitCode=0},
    @{name='Windows 10 package source access denial does not roll back native application';languages=@();ocrReady=$false;identityReady=$false;accessDenied=$true;expectedStatus='check-failed';identityStatus='failed';basicReady=$true;registered=$false;needs=$true;unavailable=$true;exitCode=0},
    @{name='Deferring OCR performs no registration or certificate import';languages=@();ocrReady=$false;identityReady=$false;noTrust=$true;expectedStatus='unavailable';identityStatus='deferred';basicReady=$true;registered=$false;needs=$true;unavailable=$true;exitCode=0},
    @{name='Unsupported AppX API retains the application and explains OCR failure';languages=@();ocrReady=$false;identityReady=$false;unsupportedApi=$true;expectedStatus='check-failed';identityStatus='failed';basicReady=$true;registered=$false;needs=$true;unavailable=$true;exitCode=0},
    @{name='Denied certificate trust retains basic application';languages=@();ocrReady=$false;identityReady=$false;certificateDenied=$true;expectedStatus='check-failed';identityStatus='failed';basicReady=$true;registered=$false;needs=$true;unavailable=$true;exitCode=0},
    @{name='Foreign installation identity is never removed or replaced';languages=@();ocrReady=$false;identityReady=$false;existingForeign=$true;expectedStatus='check-failed';identityStatus='failed';basicReady=$true;registered=$true;needs=$true;unavailable=$true;exitCode=0},
    @{name='Repair reuses its owned identity without unregistering it';languages=@('zh-Hans-CN','en-US');ocrReady=$true;identityReady=$true;existingOwned=$true;repair=$true;expectedStatus='ready';identityStatus='ready';basicReady=$true;registered=$true;needs=$false;unavailable=$false;exitCode=0},
    @{name='Application policy block is explicitly reported instead of claimed usable';languages=@();ocrReady=$false;identityReady=$true;applicationBlocked=$true;expectedStatus='unavailable';identityStatus='registered';basicReady=$false;registered=$true;needs=$true;unavailable=$true;exitCode=0},
    @{name='Payload tampering remains fatal before certificate or AppX operations';tamper=$true;exitCode=10}
)
$taskIndex=0
foreach($taskScenario in $taskInstallScenarios){
    $taskIndex++
    Check-Task $taskScenario.name {
        $taskFixture=Join-Path $taskFixtureRoot ('install '+$taskIndex)
        [IO.Directory]::CreateDirectory($taskFixture)|Out-Null
        Copy-Item -LiteralPath $taskCertificate.FullName -Destination (Join-Path $taskFixture 'SnipLeaf-preview.cer')
        [IO.File]::WriteAllText((Join-Path $taskFixture 'SnipLeaf.exe'),'Unexecutable mocked payload')
        [IO.File]::WriteAllText((Join-Path $taskFixture 'SnipLeaf.identity.msix'),'Mocked identity; never deployed')
        $taskRecords=@('SnipLeaf.exe','SnipLeaf.identity.msix','SnipLeaf-preview.cer')|ForEach-Object {@{name=$_;sha256=(Get-FileHash -LiteralPath (Join-Path $taskFixture $_)).Hash}}
        @{packageName='SnipLeaf.Preview';publisher='CN=SnipLeaf Preview';certificateThumbprint=$taskThumbprint;files=@($taskRecords)}|
            ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $taskFixture 'installer-security.json') -Encoding UTF8
        if($taskScenario.tamper){Add-Content -LiteralPath (Join-Path $taskFixture 'SnipLeaf.exe') -Value 'Tampered payload'}
        if($taskScenario.existingOwned){
            @{directory=$taskFixture;certificateThumbprint=$taskThumbprint;packageFullName='SnipLeaf.Preview_TEST_neutral__example';trustedCertificates=@(@{thumbprint=$taskThumbprint;userAdded=$false;machineAdded=$false;publicCertificateBase64=[Convert]::ToBase64String([IO.File]::ReadAllBytes($taskCertificate.FullName))})}|
                ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $taskFixture 'installation-state.json') -Encoding UTF8
        }
        $taskScenario|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $taskFixture 'scenario.json') -Encoding UTF8
        $taskArguments=@('-NoLogo','-NoProfile','-ExecutionPolicy','Bypass','-File',('"'+$taskMockPath+'"'),'-Source',('"'+$taskSource+'"'),'-Fixture',('"'+$taskFixture+'"'))
        $taskChild=Start-Process -FilePath $taskWindowsPowerShell -ArgumentList $taskArguments -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $taskFixture 'stdout.txt') -RedirectStandardError (Join-Path $taskFixture 'stderr.txt')
        $null=$taskChild.Handle
        if(-not $taskChild.WaitForExit(15000)){$taskChild.Kill();throw 'Mock installer exceeded 15 seconds.'}
        $taskChild.WaitForExit()
        $taskTrace=Get-Content -LiteralPath (Join-Path $taskFixture 'trace.json') -Raw -Encoding UTF8|ConvertFrom-Json
        Assert-Task ($taskChild.ExitCode -eq $taskScenario.exitCode) ('Unexpected exit code: '+$taskChild.ExitCode+'; '+(Get-Content -LiteralPath (Join-Path $taskFixture 'stderr.txt') -Raw))
        $taskAdds=@($taskTrace.calls|Where-Object {$_.action -eq 'register'})
        foreach($call in $taskAdds){Assert-Task ($call.volume -eq 'C:\Program Files\WindowsApps' -and $call.externalLocation -eq $taskFixture) 'System identity volume or external directory protection regressed.'}
        if($taskScenario.exitCode -eq 0){
            Assert-Task (@($taskTrace.calls|Where-Object {$_.action -eq 'check'}).Count -eq 1) 'Installation did not use a single bounded application check.'
            $taskState=Get-Content -LiteralPath (Join-Path $taskFixture 'installation-state.json') -Raw -Encoding UTF8|ConvertFrom-Json
            Assert-Task ($taskState.ocrStatus -eq $taskScenario.expectedStatus) 'Incorrect OCR readiness status.'
            Assert-Task ($taskState.ocrReady -eq $taskScenario.ocrReady) 'Incorrect OCR availability.'
            Assert-Task ($taskState.identityStatus -eq $taskScenario.identityStatus) ('Incorrect identity status: '+$taskState.identityStatus)
            Assert-Task ($taskState.basicReady -eq $taskScenario.basicReady) 'Startup block was mislabeled as working.'
            Assert-Task ($taskTrace.registered -eq $taskScenario.registered) 'Unexpected identity mutation.'
            Assert-Task (@($taskTrace.calls|Where-Object {$_.action -eq 'remove'}).Count -eq 0) 'Optional feature caused rollback.'
            if($taskScenario.noTrust -or $taskScenario.existingOwned -or $taskScenario.existingForeign -or $taskScenario.unsupportedApi -or $taskScenario.certificateDenied){Assert-Task ($taskAdds.Count -eq 0) 'Unexpected registration attempt.'}
            if($taskScenario.accessDenied){Assert-Task ($taskState.identityError -match '0x80070005') 'Original access-denied diagnostic was lost.'}
            Assert-Task ((Test-Path -LiteralPath (Join-Path $taskFixture 'ocr-identity-needed.flag')) -eq ($taskScenario.identityStatus -ne 'ready')) 'Incorrect identity repair notice.'
            Assert-Task ((Test-Path -LiteralPath (Join-Path $taskFixture 'application-check-needed.flag')) -eq (-not $taskScenario.basicReady)) 'Incorrect native startup warning.'
            Assert-Task ((Test-Path -LiteralPath (Join-Path $taskFixture 'ocr-setup-needed.flag')) -eq $taskScenario.needs) 'Incorrect missing-language notice.'
            Assert-Task ((Test-Path -LiteralPath (Join-Path $taskFixture 'ocr-unavailable.flag')) -eq $taskScenario.unavailable) 'Incorrect OCR unavailable notice.'
        }else{
            Assert-Task ($taskTrace.calls.Count -eq 0) 'Tampered payload caused external operations.'
            Assert-Task (-not $taskTrace.registered) 'Invalid identity was not rolled back.'
            Assert-Task (-not(Test-Path -LiteralPath (Join-Path $taskFixture 'installation-state.json'))) 'Fatal verification failure committed ownership.'
        }
    }
}

# Execute the actual preparation core with only DISM calls replaced. Reports
# are real files, allowing inspection while a simulated download is running.
$taskTokens=$null;$taskErrors=$null
$taskPreparationAst=[Management.Automation.Language.Parser]::ParseFile($taskPreparationSource,[ref]$taskTokens,[ref]$taskErrors)
if($taskErrors.Count){throw ($taskErrors|Out-String)}
foreach($taskName in @('Write-OcrPreparationReport','Invoke-OcrLanguagePreparation')){
    $taskDefinition=$taskPreparationAst.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $taskName},$true)
    if(-not $taskDefinition){throw ('Missing preparation helper: '+$taskName)}
    . ([scriptblock]::Create($taskDefinition.Extent.Text))
}
$taskCapabilityStates=@{}
$taskCapabilityCalls=[Collections.Generic.List[string]]::new()
$taskCapabilityFailure='';$taskQueryFailure='';$taskSilentFailure='';$taskPending='';$taskRestartAt='';$taskCurrentReport=''
function Get-WindowsCapability {
    [CmdletBinding()]param([switch]$Online,[string]$Name)
    if(-not $taskCapabilityStates.ContainsKey($Name)){throw 'TEST GUARD: unexpected Windows feature.'}
    if($Name -eq $taskQueryFailure){throw 'Simulated capability query failure'}
    Assert-Task (Test-Path -LiteralPath $taskCurrentReport) 'Initial progress report was not saved before querying Windows.'
    [pscustomobject]@{State=$taskCapabilityStates[$Name]}
}
function Add-WindowsCapability {
    [CmdletBinding()]param([switch]$Online,[string]$Name,[string]$LogPath)
    if(-not $taskCapabilityStates.ContainsKey($Name)){throw 'TEST GUARD: unexpected Windows feature.'}
    $taskCapabilityCalls.Add($Name)
    $progress=Get-Content -LiteralPath $taskCurrentReport -Raw -Encoding UTF8|ConvertFrom-Json
    Assert-Task ($progress.phase -eq 'installing' -and $progress.activeCapability -eq $Name) 'No readable progress report during servicing.'
    Assert-Task ($progress.capabilities.Count -eq 4 -and @($progress.capabilities|Where-Object {$_.state -eq 'Checking'}).Count -eq 0) 'Started downloading before all four components were inspected.'
    Assert-Task ($progress.dismLogPath -eq $LogPath) 'Detailed DISM log destination was lost.'
    if($Name -eq $taskCapabilityFailure){throw 'Simulated Windows Update download failure'}
    if($Name -ne $taskSilentFailure){$taskCapabilityStates[$Name]=if($Name -eq $taskPending){'InstallPending'}else{'Installed'}}
    [pscustomobject]@{RestartNeeded=($Name -eq $taskRestartAt)}
}
function Reset-TaskCapabilities([string]$State){
    $script:taskCapabilityStates=@{}
    foreach($language in @('zh-CN','en-US')){foreach($feature in @('Basic','OCR')){$script:taskCapabilityStates['Language.'+$feature+'~~~'+$language+'~0.0.1.0']=$State}}
    $script:taskCapabilityCalls.Clear()
    $script:taskCapabilityFailure='';$script:taskQueryFailure='';$script:taskSilentFailure='';$script:taskPending='';$script:taskRestartAt=''
    $script:taskCurrentReport=Join-Path $taskFixtureRoot ('progress-'+[Guid]::NewGuid().ToString('N')+'.json')
}
function Run-TaskPreparation {
    Invoke-OcrLanguagePreparation $taskCurrentReport (Join-Path $taskFixtureRoot 'mock-dism.log') 6>$null
}
Check-Task 'Installed capabilities do not trigger downloads' {
    Reset-TaskCapabilities 'Installed'
    $r=Run-TaskPreparation
    Assert-Task ($r.success -and $taskCapabilityCalls.Count -eq 0) 'Reinstalled existing system languages.'
}
Check-Task 'Missing capabilities respect Basic before OCR and save in-progress reports' {
    Reset-TaskCapabilities 'NotPresent'
    $r=Run-TaskPreparation
    Assert-Task $r.success 'Component preparation failed.'
    Assert-Task (($taskCapabilityCalls -join ',') -eq 'Language.Basic~~~zh-CN~0.0.1.0,Language.OCR~~~zh-CN~0.0.1.0,Language.Basic~~~en-US~0.0.1.0,Language.OCR~~~en-US~0.0.1.0') 'Missing dependency ordering or unrelated feature installation.'
}
Check-Task 'Only the missing English OCR component is installed' {
    Reset-TaskCapabilities 'Installed'
    $taskCapabilityStates['Language.OCR~~~en-US~0.0.1.0']='NotPresent'
    $r=Run-TaskPreparation
    Assert-Task ($r.success -and $taskCapabilityCalls.Count -eq 1 -and $taskCapabilityCalls[0] -eq 'Language.OCR~~~en-US~0.0.1.0') 'Touched an existing language.'
}
Check-Task 'Download failure retains detailed report and permits the other language' {
    Reset-TaskCapabilities 'NotPresent'
    $taskCapabilityFailure='Language.OCR~~~zh-CN~0.0.1.0'
    $r=Run-TaskPreparation
    $saved=Get-Content -LiteralPath $taskCurrentReport -Raw -Encoding UTF8|ConvertFrom-Json
    Assert-Task (-not $r.success -and $saved.errors.Count -eq 1 -and $saved.errors[0].capability -eq $taskCapabilityFailure) 'Lost the actual failing component.'
    Assert-Task ($taskCapabilityStates['Language.OCR~~~en-US~0.0.1.0'] -eq 'Installed') 'Discarded the other language after a failure.'
}
Check-Task 'Basic dependency failure prevents installing its OCR component' {
    Reset-TaskCapabilities 'NotPresent'
    $taskCapabilityFailure='Language.Basic~~~zh-CN~0.0.1.0'
    $r=Run-TaskPreparation
    Assert-Task (-not $r.success -and 'Language.OCR~~~zh-CN~0.0.1.0' -notin $taskCapabilityCalls) 'Installed OCR without its basic dependency.'
}
Check-Task 'Silent servicing failure cannot be reported as success' {
    Reset-TaskCapabilities 'Installed'
    $taskSilentFailure='Language.OCR~~~en-US~0.0.1.0'
    $taskCapabilityStates[$taskSilentFailure]='NotPresent'
    $r=Run-TaskPreparation
    Assert-Task (-not $r.success -and $r.errors.Count -eq 1) 'Trusted DISM return without checking installed state.'
}
Check-Task 'An earlier reboot requirement survives later successful components' {
    Reset-TaskCapabilities 'NotPresent'
    $taskRestartAt='Language.Basic~~~zh-CN~0.0.1.0'
    $r=Run-TaskPreparation
    Assert-Task ($r.success -and $r.restartNeeded) 'Lost the initial RestartNeeded flag.'
}
Check-Task 'Pending basic feature asks for reboot without reinstalling or installing dependent OCR' {
    Reset-TaskCapabilities 'Installed'
    $taskCapabilityStates['Language.Basic~~~zh-CN~0.0.1.0']='InstallPending'
    $r=Run-TaskPreparation
    Assert-Task (-not $r.success -and $r.restartNeeded -and $r.phase -eq 'restart-required' -and $taskCapabilityCalls.Count -eq 0) 'Reinstalled a feature pending reboot.'
}
Check-Task 'A newly pending OCR feature is not falsely reported ready' {
    Reset-TaskCapabilities 'Installed'
    $taskPending='Language.OCR~~~en-US~0.0.1.0'
    $taskCapabilityStates[$taskPending]='NotPresent'
    $r=Run-TaskPreparation
    Assert-Task (-not $r.success -and $r.restartNeeded -and $r.phase -eq 'restart-required') 'Pending OCR was reported ready.'
}
Check-Task 'Capability query error is recorded and leaves the other language usable' {
    Reset-TaskCapabilities 'Installed'
    $taskQueryFailure='Language.Basic~~~zh-CN~0.0.1.0'
    $r=Run-TaskPreparation
    Assert-Task (-not $r.success -and $r.errors[0].message -match 'query failure' -and $r.capabilities.Count -eq 4 -and $r.capabilities[0].state -eq 'QueryFailed') 'Query failure obscured readiness or skipped the other language.'
}
Check-Task 'Installer launches first-run preparation through the app after commit' {
    $iss=Get-Content -LiteralPath (Join-Path $taskProjectRoot 'packaging\installer\SnipLeaf.iss') -Raw -Encoding UTF8
    $line=@($iss -split '\r?\n'|Where-Object {$_ -match '^Filename:.*\{app\}\\SnipLeaf\.exe'})
    Assert-Task ($line.Count -eq 1 -and $line[0] -match 'nowait postinstall skipifsilent runasoriginaluser' -and $line[0] -notmatch 'unchecked') 'First-run app is not selected or blocks installation.'
    Assert-Task ($iss -notmatch '^Filename:.*Initialize-OcrLanguages\.ps1') 'Duplicate installer language preparation survived.'
    Assert-Task ($iss -notmatch '-EnsureOcrLanguages' -and $iss -notmatch 'Name: "ocrlanguages"') 'Legacy automatic download task survived.'
}
Check-Task 'All four rows exist and existing language states are known before any download' {
    Reset-TaskCapabilities 'Installed'
    $taskCapabilityStates['Language.Basic~~~zh-CN~0.0.1.0']='NotPresent'
    $taskCapabilityStates['Language.OCR~~~zh-CN~0.0.1.0']='NotPresent'
    $r=Run-TaskPreparation
    Assert-Task ($r.capabilities.Count -eq 4 -and $r.success -and $taskCapabilityCalls.Count -eq 2) 'Missing component rows or reinstalled English.'
}

$taskPassed=@($taskCases|Where-Object {$_.passed}).Count
$taskFailed=$taskCases.Count-$taskPassed
if(-not $ReportPath){$ReportPath=Join-Path $taskProjectRoot ('reports\installer-prerequisites-'+$taskProduct.version+'.local.json')}
$taskReportFullPath=[IO.Path]::GetFullPath($ReportPath)
[IO.Directory]::CreateDirectory((Split-Path -Parent $taskReportFullPath))|Out-Null
@{suite='installer-prerequisites';hostPowerShell=$PSVersionTable.PSVersion.ToString();deploymentHost='Windows PowerShell 5.1';mockedWindowsMutations=$true;passed=$taskPassed;failed=$taskFailed;fixtures=$taskFixtureRoot;cases=@($taskCases.ToArray());createdAt=(Get-Date -Format o)}|
    ConvertTo-Json -Depth 6|Set-Content -LiteralPath $taskReportFullPath -Encoding UTF8
Write-Output ('Installer prerequisites: '+$taskPassed+' passed, '+$taskFailed+' failed. '+$taskReportFullPath)
if($taskFailed){$taskCases|Where-Object {-not $_.passed}|Format-List;exit 1}
