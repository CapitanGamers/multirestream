@echo off
setlocal EnableExtensions
cd /d "%~dp0\.."

set "LOG=%~dp0last-run.log"
echo ===== BUILD %DATE% %TIME% ===== > "%LOG%"

echo [1/3] tools
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"

set "CMAKE="
where cmake >nul 2>&1
if not errorlevel 1 (
  for /f "delims=" %%I in ('where cmake') do (
    if not defined CMAKE set "CMAKE=%%I"
  )
)
if exist "%VSWHERE%" if not defined CMAKE (
  for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -find Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`) do set "CMAKE=%%I"
)

set "MSBUILD="
if exist "%VSWHERE%" (
  for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do set "MSBUILD=%%I"
)

if defined CMAKE echo     CMake   %CMAKE%
if defined MSBUILD echo     MSBuild %MSBUILD%
echo CMAKE=%CMAKE%>> "%LOG%"
echo MSBUILD=%MSBUILD%>> "%LOG%"

if not exist build mkdir build
if not exist build\Release mkdir build\Release

if defined CMAKE goto CMAKE_TRY
goto MSBUILD_PATH

:CMAKE_TRY
echo [2/3] cmake generate
cd build
if exist CMakeCache.txt del /q CMakeCache.txt
if exist CMakeFiles rmdir /s /q CMakeFiles

"%CMAKE%" -G "Visual Studio 18 2026" -A x64 .. >> "%LOG%" 2>&1
if errorlevel 1 (
  echo     not VS 2026, trying VS 2022
  if exist CMakeCache.txt del /q CMakeCache.txt
  "%CMAKE%" -G "Visual Studio 17 2022" -A x64 .. >> "%LOG%" 2>&1
)
if errorlevel 1 (
  echo     not VS 2022, trying VS 2019
  if exist CMakeCache.txt del /q CMakeCache.txt
  "%CMAKE%" -G "Visual Studio 16 2019" -A x64 .. >> "%LOG%" 2>&1
)
if errorlevel 1 (
  echo cmake generate failed, falling back to MSBuild
  cd ..
  goto MSBUILD_PATH
)

echo [3/3] cmake build
"%CMAKE%" --build . --config Release >> "%LOG%" 2>&1
cd ..
if errorlevel 1 (
  echo cmake build failed
  echo see %LOG%
  goto MSBUILD_PATH
)
goto CHECK

:MSBUILD_PATH
if not defined MSBUILD (
  echo.
  echo CMake generator failed and MSBuild was not found.
  echo Open multi-restream.sln in Visual Studio and Build Solution (Release x64).
  echo see %LOG%
  exit /b 1
)

echo [2/3] MSBuild multi-restream.sln
"%MSBUILD%" "%cd%\multi-restream.sln" /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v145 /m /v:minimal >> "%LOG%" 2>&1
if errorlevel 1 (
  echo     retry toolset v143
  "%MSBUILD%" "%cd%\multi-restream.sln" /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143 /m /v:minimal >> "%LOG%" 2>&1
)
if errorlevel 1 (
  echo     retry toolset v142
  "%MSBUILD%" "%cd%\multi-restream.sln" /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v142 /m /v:minimal >> "%LOG%" 2>&1
)
if errorlevel 1 (
  echo MSBuild failed
  echo see %LOG%
  exit /b 1
)

:CHECK
if not exist build\Release\mr_server.exe (
  echo mr_server.exe was not created
  echo see %LOG%
  exit /b 1
)
if not exist build\Release\mr_client.exe (
  echo mr_client.exe was not created
  echo see %LOG%
  exit /b 1
)

echo.
echo OK
echo   build\Release\mr_server.exe
echo   build\Release\mr_client.exe
exit /b 0
