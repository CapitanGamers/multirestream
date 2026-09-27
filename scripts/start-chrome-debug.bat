@echo off
setlocal EnableExtensions
title Chrome debug - copy existing profile
echo.
echo Chrome 136+ will NOT debug the official profile folder.
echo This copies YOUR existing Chrome profile to a side folder
echo so logins stay the same, then opens that copy with debug port.
echo.
echo Close ALL Chrome windows first (system tray too).
echo If Chrome stays open, cookie files are locked and logins will be missing.
echo.
pause

set PORT=9333
set "CHROME="
if exist "%ProgramFiles%\Google\Chrome\Application\chrome.exe" set "CHROME=%ProgramFiles%\Google\Chrome\Application\chrome.exe"
if exist "%ProgramFiles(x86)%\Google\Chrome\Application\chrome.exe" set "CHROME=%ProgramFiles(x86)%\Google\Chrome\Application\chrome.exe"
if not defined CHROME (
  echo chrome.exe not found
  pause
  exit /b 1
)

set "SRC=%LOCALAPPDATA%\Google\Chrome\User Data"
set "DST=%LOCALAPPDATA%\MultiRestream\ChromeDebug"

if not exist "%SRC%\Default" (
  echo Source profile not found:
  echo   %SRC%\Default
  pause
  exit /b 1
)

echo closing chrome.exe
taskkill /IM chrome.exe /F >nul 2>&1
timeout /t 3 /nobreak >nul

if not exist "%DST%" mkdir "%DST%"

echo copying profile (caches skipped, may take a minute)...
if exist "%SRC%\Local State" copy /y "%SRC%\Local State" "%DST%\Local State" >nul
if exist "%SRC%\First Run" copy /y "%SRC%\First Run" "%DST%\First Run" >nul

robocopy "%SRC%\Default" "%DST%\Default" /E /XO /R:1 /W:1 /NFL /NDL /NJH /NJS /NP /XD Cache "Code Cache" "GPUCache" "GrShaderCache" ShaderCache "Service Worker" "optimization_guide_hint_cache" Crashpad BrowserMetrics System Temp "File System" blob_storage

for /d %%P in ("%SRC%\Profile *") do (
  echo copying %%~nxP
  robocopy "%%P" "%DST%\%%~nxP" /E /XO /R:1 /W:1 /NFL /NDL /NJH /NJS /NP /XD Cache "Code Cache" "GPUCache" "GrShaderCache" ShaderCache "Service Worker" Crashpad Temp
)

echo.
echo starting debug Chrome on your copied profile
start "" "%CHROME%" --remote-debugging-port=%PORT% --remote-allow-origins=* --user-data-dir="%DST%" --profile-directory=Default

echo.
echo Use the window that just opened.
echo It should already be logged in. Then click Read Chrome in the client.
echo Your everyday Chrome is untouched. Next time this copy is updated with /XO.
echo.
pause
