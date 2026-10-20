[CmdletBinding()]
param([switch]$SkipOcr,[switch]$Quality,[string]$Executable,[string]$ReportDirectory)
$ErrorActionPreference='Stop'
if($SkipOcr -and $Quality){throw 'Quality requires OCR; do not combine it with SkipOcr.'}
$taskProjectRoot=Split-Path -Parent $PSScriptRoot
$taskExe=Join-Path $taskProjectRoot 'build\Release\SnapLite.exe'
if($Executable){$taskExe=[IO.Path]::GetFullPath($Executable)}
if(-not(Test-Path -LiteralPath $taskExe)){throw 'Build Release first.'}
$taskReports=Join-Path $taskProjectRoot 'reports'
if($ReportDirectory){$taskReports=[IO.Path]::GetFullPath($ReportDirectory)}
[IO.Directory]::CreateDirectory($taskReports)|Out-Null
$taskSuites=@(@{name='core';argument='--self-test'},@{name='ui';argument='--ui-test'},@{name='regression';argument='--regression-test'},@{name='translation';argument='--translation-test'})
if(-not $SkipOcr){$taskSuites+=@{name='ocr';argument='--ocr-test'}}
if($Quality){$taskSuites+=@{name='ocr-quality';argument='--ocr-quality-test'}}
$taskFailed=@()
foreach($taskSuite in $taskSuites){
    $taskReport=Join-Path $taskReports ($taskSuite.name+'.local.json')
    $taskStarted=[DateTime]::UtcNow
    $taskProcess=Start-Process -FilePath $taskExe -ArgumentList @($taskSuite.argument,('"'+$taskReport+'"')) -WindowStyle Hidden -PassThru
    $null=$taskProcess.Handle
    if(-not $taskProcess.WaitForExit(120000)){
        $taskProcess.Kill()
        throw ('Test timed out: '+$taskSuite.name)
    }
    $taskProcess.WaitForExit()
    if($taskProcess.ExitCode -ne 0){$taskFailed+=$taskSuite.name}
    if(-not(Test-Path -LiteralPath $taskReport) -or (Get-Item -LiteralPath $taskReport).LastWriteTimeUtc -lt $taskStarted){
        throw ('Test did not produce a fresh report: '+$taskSuite.name+'; process exit '+$taskProcess.ExitCode)
    }
    $taskResult=Get-Content -LiteralPath $taskReport -Raw -Encoding UTF8|ConvertFrom-Json
    if($taskResult.suite -ne $taskSuite.name){throw ('Unexpected report suite: '+$taskSuite.name)}
    Write-Output ($taskSuite.name+': exit '+$taskProcess.ExitCode+'; '+$taskReport)
}
if($taskFailed.Count){throw ('Failed suites: '+($taskFailed -join ', '))}
Write-Output 'All requested test suites passed.'
