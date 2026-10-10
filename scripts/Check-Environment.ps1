[CmdletBinding()]
param([switch]$Json, [switch]$Strict, [switch]$SkipOcrProbe)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT') { throw 'Windows is required.' }

$taskVswhere = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft Visual Studio\Installer\vswhere.exe'
$taskVsInstances = @()
if (Test-Path -LiteralPath $taskVswhere) {
    $taskVsInstances = @(& $taskVswhere -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json | ConvertFrom-Json)
}
$taskCompilers = @(
    foreach ($taskInstance in $taskVsInstances) {
        $taskVersionFile = Join-Path $taskInstance.installationPath 'VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt'
        if (Test-Path -LiteralPath $taskVersionFile) {
            $taskToolset = ([IO.File]::ReadAllText($taskVersionFile)).Trim()
            $taskCompilerPath = Join-Path $taskInstance.installationPath ('VC\Tools\MSVC\' + $taskToolset + '\bin\Hostx64\x64\cl.exe')
            if (Test-Path -LiteralPath $taskCompilerPath) {
                [pscustomobject]@{ Toolset = $taskToolset; Path = $taskCompilerPath; VisualStudioVersion = $taskInstance.installationVersion }
            }
        }
    }
)
$taskCmakeCommand = Get-Command cmake.exe -ErrorAction SilentlyContinue
$taskCmakePaths = @()
if ($taskCmakeCommand) { $taskCmakePaths += $taskCmakeCommand.Source }
$taskCmakeRoot = Join-Path $env:LOCALAPPDATA 'Programs\CMake'
$taskCmakePaths += @(Get-ChildItem -LiteralPath $taskCmakeRoot -Directory -ErrorAction SilentlyContinue |
    ForEach-Object { Join-Path $_.FullName 'bin\cmake.exe' } |
    Where-Object { Test-Path -LiteralPath $_ })
$taskCmakePaths = @($taskCmakePaths | Select-Object -Unique)

$taskSdkRoots = @((Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Windows Kits\10'))
foreach ($taskSdkKey in @(
    'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots',
    'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots'
)) {
    if (Test-Path -LiteralPath $taskSdkKey) {
        $taskSdkRoot = Get-ItemPropertyValue -LiteralPath $taskSdkKey -Name KitsRoot10 -ErrorAction SilentlyContinue
        if ($taskSdkRoot) { $taskSdkRoots += $taskSdkRoot }
    }
}
$taskSdks = @(
    foreach ($taskSdkRoot in @($taskSdkRoots | Select-Object -Unique)) {
        $taskSdkIncludes = Join-Path $taskSdkRoot 'Include'
        if (-not (Test-Path -LiteralPath $taskSdkIncludes)) { continue }
        foreach ($taskSdkVersionDir in @(Get-ChildItem -LiteralPath $taskSdkIncludes -Directory)) {
            $taskSdkVersion = $taskSdkVersionDir.Name
            [pscustomobject]@{
                Version = $taskSdkVersion
                Root = $taskSdkRoot
                DesktopHeaders = Test-Path -LiteralPath (Join-Path $taskSdkVersionDir.FullName 'um\Windows.h')
                CppWinRtHeaders = Test-Path -LiteralPath (Join-Path $taskSdkVersionDir.FullName 'cppwinrt\winrt\Windows.Media.Ocr.h')
                X64Libraries = Test-Path -LiteralPath (Join-Path $taskSdkRoot ('Lib\' + $taskSdkVersion + '\um\x64\user32.lib'))
                MakeAppx = Test-Path -LiteralPath (Join-Path $taskSdkRoot ('bin\' + $taskSdkVersion + '\x64\makeappx.exe'))
                SignTool = Test-Path -LiteralPath (Join-Path $taskSdkRoot ('bin\' + $taskSdkVersion + '\x64\signtool.exe'))
            }
        }
    }
)
$taskOcr = [pscustomobject]@{ Status = 'Skipped'; Languages = @(); DeploymentVerified = $false }
if (-not $SkipOcrProbe) {
    $taskOcrCommand = @'
$ErrorActionPreference = 'Stop'
try {
    $taskEngineType = [Windows.Media.Ocr.OcrEngine,Windows.Foundation,ContentType=WindowsRuntime]
    [pscustomobject]@{
        Status = 'AvailableInPowerShellHost'
        Languages = @($taskEngineType::AvailableRecognizerLanguages | ForEach-Object { $_.LanguageTag })
        MaxImageDimension = $taskEngineType::MaxImageDimension
        DeploymentVerified = $false
    } | ConvertTo-Json -Compress
} catch {
    [pscustomobject]@{
        Status = 'Failed'
        Languages = @()
        DeploymentVerified = $false
        Error = $_.Exception.Message
    } | ConvertTo-Json -Compress
}
'@
    $taskLegacyPowerShell = Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $taskOcrOutput = & $taskLegacyPowerShell -NoProfile -NonInteractive -Command $taskOcrCommand
    if ($LASTEXITCODE -eq 0 -and $taskOcrOutput) {
        $taskOcr = ($taskOcrOutput -join [Environment]::NewLine) | ConvertFrom-Json
    }
}
$taskSdkReady = @($taskSdks | Where-Object { $_.DesktopHeaders -and $_.CppWinRtHeaders -and $_.X64Libraries }).Count -gt 0
$taskOs = Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
$taskReport = [pscustomobject]@{
    CheckedAt = [DateTimeOffset]::UtcNow.ToOffset([TimeSpan]::FromHours(8)).ToString('o')
    ProjectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
    Windows = [pscustomobject]@{ Build = $taskOs.CurrentBuild; Revision = $taskOs.UBR; DisplayVersion = $taskOs.DisplayVersion }
    Compilers = $taskCompilers
    CmakePaths = $taskCmakePaths
    CmakeVersion = if ($taskCmakePaths.Count -gt 0) { @(& $taskCmakePaths[0] --version)[0] } else { $null }
    WindowsSdks = $taskSdks
    Ocr = $taskOcr
    ReadyForNativeConfigure = ($taskCompilers.Count -gt 0 -and $taskCmakePaths.Count -gt 0 -and $taskSdkReady)
    Notes = @(
        'Read-only check; no screen or clipboard access.',
        'MSVC is normally activated through a developer shell, not the global PATH.',
        'PowerShell OCR enumeration does not establish supported native application deployment.',
        'A successful actual compile and run are required in addition to path checks.'
    )
}
if ($Json) {
    $taskReport | ConvertTo-Json -Depth 8
} else {
    Write-Output ('Native configure prerequisites found: ' + $taskReport.ReadyForNativeConfigure)
    $taskReport.Compilers | Format-Table -AutoSize
    Write-Output ('CMake: ' + $taskReport.CmakeVersion)
    $taskReport.WindowsSdks | Format-Table -AutoSize
    Write-Output ('OCR languages: ' + ($taskReport.Ocr.Languages -join ', '))
}
if ($Strict -and -not $taskReport.ReadyForNativeConfigure) { exit 2 }
