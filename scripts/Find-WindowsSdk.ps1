function Find-TaskWindowsSdkTool {
    [CmdletBinding()]
    param([Parameter(Mandatory)][ValidateSet('makeappx.exe','signtool.exe')][string]$Tool)

    $taskSdkRoots = @()
    if ($env:WindowsSdkDir) { $taskSdkRoots += $env:WindowsSdkDir }
    foreach ($taskSdkKey in @(
        'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots',
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots'
    )) {
        if (Test-Path -LiteralPath $taskSdkKey) {
            $taskSdkRoot = Get-ItemPropertyValue -LiteralPath $taskSdkKey -Name KitsRoot10 -ErrorAction SilentlyContinue
            if ($taskSdkRoot) { $taskSdkRoots += $taskSdkRoot }
        }
    }
    $taskSdkRoots += Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Windows Kits\10'
    foreach ($taskSdkRoot in @($taskSdkRoots | Select-Object -Unique)) {
        $taskSdkBin = Join-Path $taskSdkRoot 'bin'
        if (-not (Test-Path -LiteralPath $taskSdkBin)) { continue }
        $taskSdkVersions = @(Get-ChildItem -LiteralPath $taskSdkBin -Directory |
            Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' } |
            Sort-Object { [version]$_.Name } -Descending)
        foreach ($taskSdkVersion in $taskSdkVersions) {
            $taskCandidate = Join-Path $taskSdkVersion.FullName ('x64\' + $Tool)
            if (Test-Path -LiteralPath $taskCandidate -PathType Leaf) { return $taskCandidate }
        }
    }
    throw ('Windows SDK x64 tool was not found: ' + $Tool + '. Install a Windows SDK with desktop C++ tools.')
}
