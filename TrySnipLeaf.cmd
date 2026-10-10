@echo off
if exist "%LOCALAPPDATA%\Programs\SnipLeaf\SnipLeaf.exe" (
  start "" "%LOCALAPPDATA%\Programs\SnipLeaf\SnipLeaf.exe" --demo
  exit /b 0
)
if not exist "%~dp0build\Release\SnapLite.exe" (
  echo Build SnapLite first: powershell -NoProfile -File scripts\Build.ps1
  pause
  exit /b 1
)
start "" "%~dp0build\Release\SnapLite.exe" --demo
