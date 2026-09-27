@echo off
setlocal
cd /d "%~dp0"
title MultiRestream Server
echo.
echo folder: %cd%
echo.

if not exist "%~dp0mr_server.exe" (
  echo mr_server.exe is not in this folder
  echo This script must sit next to mr_server.exe
  echo.
  pause
  exit /b 1
)
if not exist "%~dp0ffmpeg\bin\ffmpeg.exe" (
  echo ffmpeg.exe missing:
  echo   %cd%\ffmpeg\bin\ffmpeg.exe
  echo.
  pause
  exit /b 1
)

echo starting mr_server.exe --console
echo do not close this window
echo logs: %cd%\logs
echo.
"%~dp0mr_server.exe" --console
echo.
echo server exited. if this was instant, open the logs folder.
pause
