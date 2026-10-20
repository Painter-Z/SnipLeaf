[CmdletBinding()]
param([string]$ReportPath)
$ErrorActionPreference='Stop'
$taskProjectRoot=Split-Path -Parent $PSScriptRoot
$taskSourcePath=Join-Path $taskProjectRoot 'packaging\installer\Deploy-App.ps1'
$taskTokens=$null
$taskParseErrors=$null
$taskAst=[System.Management.Automation.Language.Parser]::ParseFile($taskSourcePath,[ref]$taskTokens,[ref]$taskParseErrors)
if($taskParseErrors.Count){throw ($taskParseErrors|Out-String)}

# Load only the pure/delegating helpers. Never execute the installer,
# import a certificate, register a package, or launch an elevated process.
foreach($taskFunctionName in @('Get-IdentitySupport','Get-SystemIdentityVolume','Add-IdentityPackage','Write-DeploymentDiagnostics')){
    $taskDefinition=$taskAst.Find({param($node) $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $taskFunctionName},$true)
    if(-not $taskDefinition){throw ('Missing installer helper: '+$taskFunctionName)}
    . ([scriptblock]::Create($taskDefinition.Extent.Text))
}
$taskCalls=[System.Collections.Generic.List[object]]::new()
$taskLog=[System.Collections.Generic.List[string]]::new()
$taskEventCalls=[System.Collections.Generic.List[object]]::new()
$taskSystem=[pscustomobject]@{Name='system-volume';PackageStorePath='C:\Program Files\WindowsApps';IsSystemVolume=$true;IsOffline=$false}
$taskSecondary=[pscustomobject]@{Name='secondary-volume';PackageStorePath='E:\WindowsApps';IsSystemVolume=$false;IsOffline=$false}
$taskOffline=[pscustomobject]@{Name='offline-system';PackageStorePath='C:\Program Files\WindowsApps';IsSystemVolume=$true;IsOffline=$true}
$taskTestVolumes=@()
$taskIdentityVolume=$null
$taskDirectory='E:\Tool with spaces\SnipLeaf'
$taskDefaultAppxVolume=$taskSecondary
$taskEventFailure=$false
$taskAddFailure=$false
$taskFixtureRoot=Join-Path $taskProjectRoot ('out\validation\installer-staging\'+[Guid]::NewGuid().ToString('N'))
$taskIdentityStagingRoot=Join-Path $taskFixtureRoot 'standard cache'
[IO.Directory]::CreateDirectory($taskFixtureRoot)|Out-Null
$taskOriginalPackage=Join-Path $taskFixtureRoot 'original identity.msix'
[IO.File]::WriteAllText($taskOriginalPackage,'Mock identity bytes, never registered with Windows')
$taskOriginalHash=(Get-FileHash -LiteralPath $taskOriginalPackage).Hash

function Get-AppxVolume { [CmdletBinding()]param() $taskTestVolumes }
function Add-AppxPackage {
    [CmdletBinding()]param([string]$Path,[string]$ExternalLocation,$Volume)
    $taskCalls.Add([pscustomobject]@{Path=$Path;ExternalLocation=$ExternalLocation;Volume=$Volume;Hash=(Get-FileHash -LiteralPath $Path).Hash})
    if($taskAddFailure){throw '0x80070005: simulated service access denial'}
}
function Write-InstallLog([string]$Message){$taskLog.Add($Message)}
function Get-WinEvent {
    [CmdletBinding()]param([string]$LogName,[string]$FilterXPath,[switch]$Oldest)
    $taskEventCalls.Add([pscustomobject]@{LogName=$LogName;XPath=$FilterXPath;Oldest=$Oldest.IsPresent})
    if($taskEventFailure){throw 'Event service unavailable'}
    [pscustomobject]@{Id=5240;TimeCreated=[datetime]'2026-10-10T11:27:56';Message='First failure: settings apphive copy, 0x800701C0'}
    [pscustomobject]@{Id=5407;TimeCreated=[datetime]'2026-10-10T11:27:57';Message='Cleanup: AppContainer, 0x800701C0'}
}
function Assert-Task([bool]$Condition,[string]$Message){if(-not $Condition){throw $Message}}
function Assert-TaskThrows([scriptblock]$Operation){
    $taskThrew=$false
    try{& $Operation|Out-Null}catch{$taskThrew=$true}
    Assert-Task $taskThrew 'Expected a failure before any deployment.'
}
$taskResults=[System.Collections.Generic.List[object]]::new()
function Check-Task([string]$Name,[scriptblock]$Body){
    try{& $Body;$taskResults.Add([pscustomobject]@{name=$Name;passed=$true})}
    catch{$taskResults.Add([pscustomobject]@{name=$Name;passed=$false;error=$_.Exception.Message})}
}

Check-Task 'Secondary default and E: executable still select system identity volume' {
    $taskTestVolumes=@($taskSecondary,$taskSystem)
    $taskIdentityVolume=Get-SystemIdentityVolume
    Assert-Task ([object]::ReferenceEquals($taskIdentityVolume,$taskSystem)) 'Selected the secondary volume.'
    Add-IdentityPackage $taskOriginalPackage
    Assert-Task ([object]::ReferenceEquals($taskCalls[0].Volume,$taskSystem)) 'Registration lost the explicit system volume.'
    Assert-Task ($taskCalls[0].ExternalLocation -eq $taskDirectory) 'Changed the external executable directory.'
    Assert-Task ([object]::ReferenceEquals($taskDefaultAppxVolume,$taskSecondary)) 'Changed the global default.'
}
Check-Task 'Service reads a verified local staging copy and keeps the original' {
    $call=$taskCalls[0]
    Assert-Task ($call.Path -ne $taskOriginalPackage -and $call.Path.StartsWith($taskIdentityStagingRoot+'\')) 'Service still reads the custom installation path.'
    Assert-Task ($call.Hash -eq $taskOriginalHash) 'Service received different package bytes.'
    Assert-Task (-not(Test-Path -LiteralPath $call.Path) -and -not(Test-Path -LiteralPath (Split-Path -Parent $call.Path))) 'Staged copy was not cleaned up.'
    Assert-Task ((Get-FileHash -LiteralPath $taskOriginalPackage).Hash -eq $taskOriginalHash) 'Modified the original identity package.'
}
Check-Task 'Deployment denial cleans only its own staging files' {
    $taskIdentityVolume=$taskSystem
    $taskAddFailure=$true
    try{Assert-TaskThrows {Add-IdentityPackage $taskOriginalPackage}}finally{$taskAddFailure=$false}
    $call=$taskCalls[$taskCalls.Count-1]
    Assert-Task (-not(Test-Path -LiteralPath $call.Path) -and (Test-Path -LiteralPath $taskOriginalPackage)) 'Failed deployment leaked its staging file or removed the original.'
}
Check-Task 'Windows 10 2004 and Windows 11 support the identity API' {
    Assert-Task (-not(Get-IdentitySupport ([version]'10.0.19041.0'))) 'Windows 10 2004 was rejected.'
    Assert-Task (-not(Get-IdentitySupport ([version]'10.0.26100.0'))) 'Windows 11 was rejected.'
}
Check-Task 'Older Windows explains the missing external identity support' {
    Assert-Task ((Get-IdentitySupport ([version]'10.0.18363.0')) -match '19041') 'Older Windows was incorrectly accepted.'
}
Check-Task 'System-only machine remains supported' {
    $taskTestVolumes=@($taskSystem)
    Assert-Task ([object]::ReferenceEquals((Get-SystemIdentityVolume),$taskSystem)) 'System-only selection failed.'
}
Check-Task 'Offline candidate is not used' {
    $taskTestVolumes=@($taskOffline,$taskSystem,$taskSecondary)
    Assert-Task ([object]::ReferenceEquals((Get-SystemIdentityVolume),$taskSystem)) 'Selected an offline candidate.'
}
Check-Task 'Missing system volume fails before registration' {
    $taskTestVolumes=@($taskSecondary)
    $taskCount=$taskCalls.Count
    Assert-TaskThrows {Get-SystemIdentityVolume}
    Assert-Task ($taskCalls.Count -eq $taskCount) 'Attempted registration without a system volume.'
}
Check-Task 'Offline system volume fails before registration' {
    $taskTestVolumes=@($taskOffline,$taskSecondary)
    Assert-TaskThrows {Get-SystemIdentityVolume}
}
Check-Task 'Ambiguous system volumes fail closed' {
    $taskTestVolumes=@($taskSystem,$taskSystem)
    Assert-TaskThrows {Get-SystemIdentityVolume}
}
Check-Task 'Unselected volume cannot silently inherit the Windows default' {
    $taskIdentityVolume=$null
    $taskCount=$taskCalls.Count
    Assert-TaskThrows {Add-IdentityPackage 'identity.msix'}
    Assert-Task ($taskCalls.Count -eq $taskCount) 'Called deployment with an implicit volume.'
}
Check-Task 'Initial registration and certificate retry share staging without changing global settings' {
    $taskCommands=@($taskAst.FindAll({param($node) $node -is [System.Management.Automation.Language.CommandAst]},$true))
    $taskRawAdds=@($taskCommands|Where-Object {$_.GetCommandName() -eq 'Add-AppxPackage'})
    Assert-Task ($taskRawAdds.Count -eq 1) 'Found a deployment call outside the shared wrapper.'
    $taskOwner=$taskRawAdds[0].Parent
    while($taskOwner -and $taskOwner -isnot [System.Management.Automation.Language.FunctionDefinitionAst]){$taskOwner=$taskOwner.Parent}
    Assert-Task ($taskOwner.Name -eq 'Add-IdentityPackage') 'Direct deployment bypasses the shared wrapper.'
    Assert-Task (@($taskCommands|Where-Object {$_.GetCommandName() -eq 'Add-IdentityPackage'}).Count -eq 2) 'A registration/retry path is missing.'
    Assert-Task (@($taskCommands|Where-Object {$_.GetCommandName() -eq 'Set-AppxDefaultVolume'}).Count -eq 0) 'Installer changes global application storage.'
    Assert-Task (@($taskCommands|Where-Object {$_.GetCommandName() -in @('Set-Acl','icacls','takeown','Set-ExecutionPolicy')}).Count -eq 0) 'Installer widens permissions or changes global policy.'
}
Check-Task 'Diagnostics retain the first error before cleanup and filter by exact activity' {
    $taskFailure=[System.Management.Automation.ErrorRecord]::new([Exception]::new('Failed [ActivityId] f39798ce-54ee-0000-9706-c2f3ee54dd01'),'DeploymentError',[System.Management.Automation.ErrorCategory]::NotSpecified,$null)
    Write-DeploymentDiagnostics $taskFailure
    $first=@($taskLog|Where-Object {$_ -match '5240.*First failure'})
    $cleanup=@($taskLog|Where-Object {$_ -match '5407.*Cleanup'})
    Assert-Task ($first.Count -eq 2 -and $cleanup.Count -eq 2) 'Lost the original error or later cleanup error.'
    Assert-Task ($taskLog.IndexOf($first[0]) -lt $taskLog.IndexOf($cleanup[0])) 'Cleanup obscured the first failure.'
    Assert-Task ($taskEventCalls[0].XPath -eq "*[System[Correlation[@ActivityID='{f39798ce-54ee-0000-9706-c2f3ee54dd01}']]]") 'Incorrect activity filter.'
    Assert-Task $taskEventCalls[0].Oldest 'Events are not in chronological order.'
}
Check-Task 'Errors without an activity do not collect unrelated logs' {
    $taskCount=$taskEventCalls.Count
    $taskFailure=[System.Management.Automation.ErrorRecord]::new([Exception]::new('Payload verification failed'),'PayloadError',[System.Management.Automation.ErrorCategory]::InvalidData,$null)
    Write-DeploymentDiagnostics $taskFailure
    Assert-Task ($taskEventCalls.Count -eq $taskCount) 'Read unrelated deployment events.'
}
Check-Task 'Unavailable event log does not replace the installation failure' {
    $taskEventFailure=$true
    $taskFailure=[System.Management.Automation.ErrorRecord]::new([Exception]::new('Original failure [ActivityId] f39798ce-54ee-0000-9706-c2f3ee54dd01'),'DeploymentError',[System.Management.Automation.ErrorCategory]::NotSpecified,$null)
    Write-DeploymentDiagnostics $taskFailure
    Assert-Task ($taskFailure.Exception.Message -like 'Original failure*') 'Replaced the original error.'
    Assert-Task ($taskLog[$taskLog.Count-1] -match 'Event service unavailable') 'Did not explain missing diagnostics.'
}
Check-Task 'Installer uses a native x64 host' {
    $taskInstaller=Get-Content -LiteralPath (Join-Path $taskProjectRoot 'packaging\installer\SnipLeaf.iss') -Raw
    Assert-Task ($taskInstaller -match '(?m)^SetupArchitecture=x64\s*$') 'Installer can redirect PowerShell to SysWOW64.'
}

$taskPassed=@($taskResults|Where-Object {$_.passed}).Count
$taskFailed=$taskResults.Count-$taskPassed
if(-not $ReportPath){
    $taskProduct=Get-Content -LiteralPath (Join-Path $taskProjectRoot 'packaging\Product.json') -Raw -Encoding UTF8|ConvertFrom-Json
    $ReportPath=Join-Path $taskProjectRoot ('reports\installer-deployment-'+$taskProduct.version+'.local.json')
}
$taskReportFullPath=[IO.Path]::GetFullPath($ReportPath)
[IO.Directory]::CreateDirectory((Split-Path -Parent $taskReportFullPath))|Out-Null
@{suite='installer-deployment';mockedWindowsMutations=$true;passed=$taskPassed;failed=$taskFailed;cases=@($taskResults.ToArray());createdAt=(Get-Date -Format o)}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath $taskReportFullPath -Encoding UTF8
Write-Output ('Installer deployment: '+$taskPassed+' passed, '+$taskFailed+' failed. '+$taskReportFullPath)
if($taskFailed){$taskResults|Where-Object {-not $_.passed}|Format-List;exit 1}
