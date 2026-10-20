[CmdletBinding()]
param([string]$Executable,[string]$ReportDirectory)
$ErrorActionPreference='Stop'
$taskProjectRoot=Split-Path -Parent $PSScriptRoot
if(-not $Executable){$Executable=Join-Path $taskProjectRoot 'build\Release\SnapLite.exe'}
$taskExecutable=[IO.Path]::GetFullPath($Executable)
if(-not(Test-Path -LiteralPath $taskExecutable -PathType Leaf)){throw 'Build the application first or specify -Executable.'}
if(-not $ReportDirectory){$ReportDirectory=Join-Path $taskProjectRoot 'reports\translation-live'}
$taskReportDirectory=[IO.Path]::GetFullPath($ReportDirectory)
[IO.Directory]::CreateDirectory($taskReportDirectory)|Out-Null
$taskReport=Join-Path $taskReportDirectory 'translation-live.local.json'
$taskStarted=[DateTime]::UtcNow
# Credentials are read inside the app from its DPAPI-protected user store.
# No credential is placed on the command line, in environment variables or logs.
$taskProcess=Start-Process -FilePath $taskExecutable -ArgumentList @('--translation-live-test',('"'+$taskReport+'"')) -WindowStyle Hidden -PassThru
$null=$taskProcess.Handle
if(-not $taskProcess.WaitForExit(45000)){throw 'Translation check did not finish. Close the test process before retrying.'}
if(-not(Test-Path -LiteralPath $taskReport) -or(Get-Item -LiteralPath $taskReport).LastWriteTimeUtc -lt $taskStarted){throw 'No fresh translation report was produced. Verify the executable supports translation.'}
$taskResult=Get-Content -LiteralPath $taskReport -Raw -Encoding UTF8|ConvertFrom-Json
if($taskProcess.ExitCode -ne 0 -or -not $taskResult.passed){throw ('Translation check failed: '+$taskResult.error)}
Write-Output ('Translation check passed; '+$taskResult.paragraphCount+' paragraphs preserved; both plain text boxes and native preview confirmed; '+[Math]::Round($taskResult.elapsedMs)+' ms. Report: '+$taskReport)
