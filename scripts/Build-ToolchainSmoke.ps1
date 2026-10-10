[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Enter-DevShell.ps1')
$taskProjectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$taskSourceRoot = Join-Path $taskProjectRoot 'tools\toolchain-smoke'
$taskBuildRoot = Join-Path $taskProjectRoot 'build\toolchain-smoke'
$taskCmake = (Get-Command cmake.exe -ErrorAction Stop).Source
& $taskCmake -S $taskSourceRoot -B $taskBuildRoot -G 'NMake Makefiles' '-DCMAKE_BUILD_TYPE=Release'
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
& $taskCmake --build $taskBuildRoot
if ($LASTEXITCODE -ne 0) { throw 'Native compilation failed.' }
$taskExecutable = Join-Path $taskBuildRoot 'snaplite-toolchain-smoke.exe'
$taskRunOutput = @(& $taskExecutable)
if ($LASTEXITCODE -ne 0) { throw 'Native smoke program failed.' }
$taskReport = [pscustomobject]@{
    CheckedAt = [DateTimeOffset]::UtcNow.ToOffset([TimeSpan]::FromHours(8)).ToString('o')
    Status = 'Passed'
    Cmake = $taskCmake
    CmakeVersion = @(& $taskCmake --version)[0]
    Compiler = (Get-Command cl.exe -ErrorAction Stop).Source
    WindowsSdkRoot = $env:WindowsSdkDir
    WindowsSdkVersion = $env:WindowsSDKVersion
    Executable = $taskExecutable
    ExecutableBytes = (Get-Item -LiteralPath $taskExecutable).Length
    Output = $taskRunOutput
    Note = 'Toolchain validation only; no screenshot app or OCR package deployment is validated.'
}
$taskReportRoot = Join-Path $taskProjectRoot 'reports'
[IO.Directory]::CreateDirectory($taskReportRoot) | Out-Null
$taskReportJson = $taskReport | ConvertTo-Json -Depth 5
[IO.File]::WriteAllText((Join-Path $taskReportRoot 'toolchain-smoke.local.json'), $taskReportJson, [Text.UTF8Encoding]::new($false))
$taskReportJson
