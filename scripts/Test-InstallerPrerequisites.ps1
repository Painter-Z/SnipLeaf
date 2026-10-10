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
$env:TEMP=Join-Path $Fixture 'Temp'
[IO.Directory]::CreateDirectory($env:TEMP)|Out-Null
function Test-Path {
    [CmdletBinding()]param([string]$LiteralPath,[string]$Path,[string]$PathType='Any')
    $p=if($LiteralPath){$LiteralPath}else{$Path}
    if($p -like 'Cert:*'){return $true}
    Microsoft.PowerShell.Management\Test-Path -LiteralPath $p -PathType $PathType
}
function Import-Certificate {throw 'TEST GUARD: certificate import was attempted.'}
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
    $global:snipLeafTestRegistered=$true
}
function Remove-AppxPackage {
    [CmdletBinding()]param([string]$Package,[Parameter(ValueFromPipeline=$true)]$InputObject)
    process{$global:snipLeafTestCalls.Add([pscustomobject]@{action='remove'});$global:snipLeafTestRegistered=$false}
}
function Get-WindowsCapability {throw 'TEST GUARD: installer queried servicing instead of application recognizers.'}
function Add-WindowsCapability {throw 'TEST GUARD: installer tried downloading a Windows feature.'}
function Start-Process {
    [CmdletBinding()]param([string]$FilePath,[string[]]$ArgumentList,[string]$WindowStyle,[switch]$PassThru,[string]$Verb)
    if($Verb -or [IO.Path]::GetFileName($FilePath) -ne 'SnipLeaf.exe' -or $ArgumentList[0] -ne '--install-check'){
        throw 'TEST GUARD: a real or elevated child process was attempted.'
    }
    $global:snipLeafTestCalls.Add([pscustomobject]@{action='check'})
    $scenario=$global:snipLeafTestScenario
    @{identityReady=$scenario.identityReady;identity='SnipLeaf.Preview_TEST_neutral__example';ocrReady=$scenario.ocrReady;languages=@($scenario.languages);languageCheckError=$scenario.languageCheckError}|
        ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $global:snipLeafTestFixture 'installation-check.json') -Encoding UTF8
    $child=[pscustomobject]@{ExitCode=$(if($scenario.identityReady){0}else{3})}
    $child|Add-Member -MemberType ScriptMethod -Name WaitForExit -Value {param($Milliseconds) -not $global:snipLeafTestScenario.timeout}
    return $child
}
& $Source -Action Install -Directory $Fixture -TrustPreview -EnsureOcrLanguages
$result=$LASTEXITCODE
@{exitCode=$result;registered=$global:snipLeafTestRegistered;calls=@($global:snipLeafTestCalls.ToArray())}|
    ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $Fixture 'trace.json') -Encoding UTF8
exit $result
'@
$taskMockPath=Join-Path $taskFixtureRoot 'mock-host.ps1'
[IO.File]::WriteAllText($taskMockPath,$taskMockHost,[Text.UTF8Encoding]::new($true))
$taskInstallScenarios=@(
    @{name='Both OCR targets already present';languages=@('zh-Hans-CN','en-US');ocrReady=$true;identityReady=$true;expectedStatus='ready';needs=$false;unavailable=$false;exitCode=0},
    @{name='Chinese alone installs successfully without waiting for English';languages=@('zh-Hans-CN');ocrReady=$true;identityReady=$true;expectedStatus='partial';needs=$true;unavailable=$false;exitCode=0},
    @{name='No OCR recognizers retain screenshot application';languages=@();ocrReady=$false;identityReady=$true;expectedStatus='unavailable';needs=$true;unavailable=$true;exitCode=0},
    @{name='Recognizer enumeration error is recorded without rollback';languages=@();ocrReady=$false;identityReady=$true;languageCheckError='Simulated optional-feature API failure';expectedStatus='check-failed';needs=$true;unavailable=$true;exitCode=0},
    @{name='Traditional Chinese does not masquerade as Simplified Chinese';languages=@('zh-Hant-TW','en-US');ocrReady=$true;identityReady=$true;expectedStatus='partial';needs=$true;unavailable=$false;exitCode=0},
    @{name='Missing application identity remains a fatal error';languages=@('zh-Hans-CN','en-US');ocrReady=$true;identityReady=$false;exitCode=1},
    @{name='Application verification timeout remains a fatal error';languages=@('zh-Hans-CN');ocrReady=$true;identityReady=$true;timeout=$true;exitCode=1}
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
        $taskScenario|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $taskFixture 'scenario.json') -Encoding UTF8
        $taskArguments=@('-NoLogo','-NoProfile','-ExecutionPolicy','Bypass','-File',('"'+$taskMockPath+'"'),'-Source',('"'+$taskSource+'"'),'-Fixture',('"'+$taskFixture+'"'))
        $taskChild=Start-Process -FilePath $taskWindowsPowerShell -ArgumentList $taskArguments -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $taskFixture 'stdout.txt') -RedirectStandardError (Join-Path $taskFixture 'stderr.txt')
        $null=$taskChild.Handle
        if(-not $taskChild.WaitForExit(15000)){$taskChild.Kill();throw 'Mock installer exceeded 15 seconds.'}
        $taskChild.WaitForExit()
        $taskTrace=Get-Content -LiteralPath (Join-Path $taskFixture 'trace.json') -Raw -Encoding UTF8|ConvertFrom-Json
        Assert-Task ($taskChild.ExitCode -eq $taskScenario.exitCode) ('Unexpected exit code: '+$taskChild.ExitCode+'; '+(Get-Content -LiteralPath (Join-Path $taskFixture 'stderr.txt') -Raw))
        Assert-Task (@($taskTrace.calls|Where-Object {$_.action -eq 'check'}).Count -eq 1) 'Installation did not use a single bounded application check.'
        Assert-Task ($taskTrace.calls[0].volume -eq 'C:\Program Files\WindowsApps') 'System identity volume protection regressed.'
        if($taskScenario.exitCode -eq 0){
            $taskState=Get-Content -LiteralPath (Join-Path $taskFixture 'installation-state.json') -Raw -Encoding UTF8|ConvertFrom-Json
            Assert-Task ($taskState.ocrStatus -eq $taskScenario.expectedStatus) 'Incorrect OCR readiness status.'
            Assert-Task ($taskState.ocrReady -eq $taskScenario.ocrReady) 'Incorrect OCR availability.'
            Assert-Task $taskTrace.registered 'Missing language removed the working identity.'
            Assert-Task (@($taskTrace.calls|Where-Object {$_.action -eq 'remove'}).Count -eq 0) 'Optional feature caused rollback.'
            Assert-Task ((Test-Path -LiteralPath (Join-Path $taskFixture 'ocr-setup-needed.flag')) -eq $taskScenario.needs) 'Incorrect missing-language notice.'
            Assert-Task ((Test-Path -LiteralPath (Join-Path $taskFixture 'ocr-unavailable.flag')) -eq $taskScenario.unavailable) 'Incorrect OCR unavailable notice.'
        }else{
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
    Assert-Task (-not $r.success -and $r.errors[0].message -match 'query failure' -and $r.capabilities.Count -eq 3) 'Query failure obscured readiness or skipped the other language.'
}
Check-Task 'Installer offers language setup only after commit and leaves it unchecked' {
    $iss=Get-Content -LiteralPath (Join-Path $taskProjectRoot 'packaging\installer\SnipLeaf.iss') -Raw -Encoding UTF8
    $line=@($iss -split '\r?\n'|Where-Object {$_ -match '^Filename:.*Initialize-OcrLanguages\.ps1'})
    Assert-Task ($line.Count -eq 1 -and $line[0] -match 'postinstall.*unchecked' -and $line[0] -notmatch 'waituntilterminated|runhidden') 'Language setup can still block installation or start automatically.'
    Assert-Task ($iss -notmatch '-EnsureOcrLanguages' -and $iss -notmatch 'Name: "ocrlanguages"') 'Legacy automatic download task survived.'
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
