@echo off
setlocal EnableExtensions
cd /d "%~dp0"
title MultiRestream build
echo.
echo ========================================
echo  MultiRestream - build on this PC
echo  Do not close this window
echo ========================================
echo.

call "%~dp0scripts\build-msvc.bat"
if errorlevel 1 (
  echo.
  echo BUILD FAILED
  echo Open scripts\last-run.log
  echo.
  pause
  exit /b 1
)

echo.
echo Packaging portable folders...
echo.
call "%~dp0scripts\package-portable.bat"
echo.
echo If no error above:
echo   dist\VPS  = copy this folder to the server
echo   dist\PC   = keep this folder on your PC
echo.
pause
endlocal
