[CmdletBinding()]
param([ValidateSet('Debug','Release')][string]$Configuration='Release',[switch]$Review,[switch]$Distribution)
$ErrorActionPreference='Stop'
$taskProjectRoot=Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'Enter-DevShell.ps1')
$taskBuild=Join-Path $taskProjectRoot ('build\'+$Configuration)
$taskOutputName='SnapLite'
if($Review){
    if($Configuration -ne 'Release'){throw 'Isolated OCR review requires Release.'}
    $taskBuild=Join-Path $taskProjectRoot 'build\UxReview'
    $taskOutputName='SnapLite.Review'
}
$taskOutputDirectory=Join-Path $taskProjectRoot ('build\'+$Configuration)
$taskIdentity=@('-DSNAPLITE_PACKAGE_NAME=SnapLite.Development','-DSNAPLITE_PUBLISHER=CN=SnapLite Local Development','-DSNAPLITE_APPLICATION_ID=SnapLite')
if($Distribution){
    if($Review -or $Configuration -ne 'Release'){throw 'Distribution requires a separate Release build.'}
    $taskProduct=Get-Content -LiteralPath (Join-Path $taskProjectRoot 'packaging\Product.json') -Raw|ConvertFrom-Json
    $taskBuild=Join-Path $taskProjectRoot 'build\Distribution'
    $taskOutputName=$taskProduct.name
    $taskOutputDirectory=Join-Path $taskProjectRoot 'out\distribution-bin'
    $taskIdentity=@("-DSNAPLITE_PACKAGE_NAME=$($taskProduct.packageName)","-DSNAPLITE_PUBLISHER=$($taskProduct.publisher)","-DSNAPLITE_APPLICATION_ID=$($taskProduct.applicationId)")
}
& cmake.exe -S $taskProjectRoot -B $taskBuild -G 'NMake Makefiles' "-DCMAKE_BUILD_TYPE=$Configuration" "-DSNAPLITE_OUTPUT_NAME=$taskOutputName" "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$taskOutputDirectory" @taskIdentity
if($LASTEXITCODE -ne 0){throw 'CMake configure failed.'}
& cmake.exe --build $taskBuild
if($LASTEXITCODE -ne 0){throw 'Compilation failed.'}
Write-Output ('Application: '+(Join-Path $taskOutputDirectory ($taskOutputName+'.exe')))
