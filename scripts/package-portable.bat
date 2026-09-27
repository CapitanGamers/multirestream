@echo off
setlocal EnableExtensions
cd /d "%~dp0\.."
title MultiRestream package
echo.

if not exist build\Release\mr_server.exe (
  echo mr_server.exe not found
  echo run START-HERE.bat first
  echo.
  pause
  exit /b 1
)
if not exist build\Release\mr_client.exe (
  echo mr_client.exe not found
  echo run START-HERE.bat first
  echo.
  pause
  exit /b 1
)

set "DIST=dist"
echo cleaning dist
if exist "%DIST%" rmdir /s /q "%DIST%"
mkdir "%DIST%\VPS\ffmpeg\bin"
mkdir "%DIST%\VPS\logs"
mkdir "%DIST%\PC"

copy /y build\Release\mr_server.exe "%DIST%\VPS\mr_server.exe"
copy /y config.example.json          "%DIST%\VPS\config.json"
copy /y scripts\vps-start.bat        "%DIST%\VPS\start.bat"
copy /y scripts\vps-start-ui.bat     "%DIST%\VPS\start-ui.bat"
copy /y scripts\vps-firewall.bat     "%DIST%\VPS\open-firewall.bat"
copy /y scripts\vps-autostart.bat    "%DIST%\VPS\install-autostart.bat"
copy /y SETUP.md                     "%DIST%\VPS\SETUP.md"
echo put ffmpeg.exe in this folder> "%DIST%\VPS\ffmpeg\bin\PUT_ffmpeg.exe_HERE.txt"

copy /y build\Release\mr_client.exe    "%DIST%\PC\mr_client.exe"
copy /y scripts\start-chrome-debug.bat "%DIST%\PC\start-chrome-debug.bat"

echo.
echo OK
echo   %cd%\dist\VPS
echo   %cd%\dist\PC
echo.
echo Next: put ffmpeg.exe here
echo   %cd%\dist\VPS\ffmpeg\bin\ffmpeg.exe
echo Download: https://www.gyan.dev/ffmpeg/builds/
echo.
pause
exit /b 0
