[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$taskVswhere = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft Visual Studio\Installer\vswhere.exe'
$taskVsRoot = $null
if (Test-Path -LiteralPath $taskVswhere) {
    $taskDetectedVsRoot = & $taskVswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($taskDetectedVsRoot) { $taskVsRoot = [string]$taskDetectedVsRoot }
}
if (-not $taskVsRoot -and $env:VSINSTALLDIR) { $taskVsRoot = $env:VSINSTALLDIR }
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    if (-not $taskVsRoot) { throw 'Install Visual Studio C++ Build Tools, or start from an x64 MSVC developer shell.' }
    $taskDevShellScript = Join-Path $taskVsRoot 'Common7\Tools\Launch-VsDevShell.ps1'
    if (-not (Test-Path -LiteralPath $taskDevShellScript)) {
        throw 'The Visual Studio developer shell is unavailable.'
    }
    & $taskDevShellScript -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
}
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    throw 'The developer shell did not expose cl.exe.'
}
$taskCmakeCommand = Get-Command cmake.exe -ErrorAction SilentlyContinue
if (-not $taskCmakeCommand) {
    $taskCmakeRoot = Join-Path $env:LOCALAPPDATA 'Programs\CMake'
    $taskCmakeBin = @(Get-ChildItem -LiteralPath $taskCmakeRoot -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like 'cmake-*-windows-x86_64' } |
        Sort-Object Name -Descending |
        ForEach-Object { Join-Path $_.FullName 'bin' } |
        Where-Object { Test-Path -LiteralPath (Join-Path $_ 'cmake.exe') }) | Select-Object -First 1
    if ($taskCmakeBin) { $env:PATH = $taskCmakeBin + ';' + $env:PATH }
}
if (-not (Get-Command cmake.exe -ErrorAction SilentlyContinue)) { throw 'Install CMake 3.28 or later and add cmake.exe to PATH.' }
Write-Output ('Developer environment: ' + $(if ($taskVsRoot) { $taskVsRoot } else { 'existing MSVC developer shell' }))
