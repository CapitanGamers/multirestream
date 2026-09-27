@echo off
setlocal
cd /d "%~dp0"
if not exist "%~dp0mr_server.exe" (
  echo mr_server.exe is not in this folder
  pause
  exit /b 1
)
start "" "%~dp0mr_server.exe"
echo UI started
pause
