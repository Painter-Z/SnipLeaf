@echo off
setlocal
if not exist "%~dp0build\Release\SnapLite.Review.exe" (
  echo Build the local review version first: scripts\Build.ps1 -Review
  pause
  exit /b 1
)
for %%P in (SnipLeaf.exe SnapLite.exe) do (
  tasklist /FI "IMAGENAME eq %%P" /NH 2>nul | find /I "%%P" >nul
  if not errorlevel 1 (
    echo Please save your pinned images and exit the current SnipLeaf from its tray menu.
    echo Then run this launcher again to use local development version 0.1.4.
    pause
    exit /b 1
  )
)
start "" "%~dp0build\Release\SnapLite.Review.exe"
