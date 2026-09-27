@echo off
setlocal
net session >nul 2>&1
if errorlevel 1 (
  echo Right-click this file and choose Run as administrator
  pause
  exit /b 1
)
cd /d "%~dp0"
if not exist "%cd%\mr_server.exe" (
  echo mr_server.exe not found in %cd%
  pause
  exit /b 1
)
schtasks /Create /TN "MultiRestream" /TR "\"%cd%\mr_server.exe\" --console" /SC ONSTART /RU SYSTEM /RL HIGHEST /F
echo.
echo Task created: MultiRestream
echo It will start after Windows reboot, even without RDP
echo Stop:  schtasks /End /TN MultiRestream
echo.
pause
