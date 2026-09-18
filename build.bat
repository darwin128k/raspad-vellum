@echo off
setlocal
cd /d "%~dp0"

set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars32.bat"
call %VCVARS% >nul
if errorlevel 1 (
    echo Failed to init MSVC x86 environment
    exit /b 1
)

set STANDALONE=OFF
set METAHHOOK=OFF
set BUILD_TYPE=Release
set REVEMU2013=OFF
set "HL_DIR="

:parse_args
if "%~1"=="" goto args_done
if /I "%~1"=="debug" (
    set BUILD_TYPE=Debug
    shift
    goto parse_args
)
if /I "%~1"=="release" (
    set BUILD_TYPE=Release
    shift
    goto parse_args
)
if /I "%~1"=="standalone" (
    set STANDALONE=ON
    shift
    goto parse_args
)
if /I "%~1"=="metahook" (
    set METAHHOOK=ON
    shift
    goto parse_args
)
if /I "%~1"=="nometahook" (
    set METAHHOOK=OFF
    shift
    goto parse_args
)
if /I "%~1"=="revemu2013" (
    set REVEMU2013=ON
    shift
    goto parse_args
)
if "%STANDALONE%"=="ON" if "%HL_DIR%"=="" (
    set "HL_DIR=%~1"
    shift
    goto parse_args
)
echo Unknown argument: %~1
exit /b 1
:args_done
if "%STANDALONE%"=="ON" if "%HL_DIR%"=="" (
    if exist "%~dp0..\raspad-hl\src\launcher.cpp" (
        set "HL_DIR=%~dp0..\raspad-hl"
    ) else (
        set "HL_DIR=%~dp0..\hl"
    )
)
if "%STANDALONE%"=="ON" if "%METAHHOOK%"=="ON" (
    call "%HL_DIR%\prepare-metahook.bat"
    if errorlevel 1 exit /b 1
)

if not exist build mkdir build
cd build

if "%STANDALONE%"=="ON" (
    cmake -G "Ninja" -DCMAKE_BUILD_TYPE=%BUILD_TYPE% -DCMAKE_CXX_COMPILER=cl -DREVLOADER_STANDALONE=ON -DHL_METAHHOOK=%METAHHOOK% -DHL_DIR="%HL_DIR%" -DVELLUM_AUTH_REVEMU2013=%REVEMU2013% ..
) else (
    cmake -G "Ninja" -DCMAKE_BUILD_TYPE=%BUILD_TYPE% -DCMAKE_CXX_COMPILER=cl -DREVLOADER_STANDALONE=OFF -DVELLUM_AUTH_REVEMU2013=%REVEMU2013% ..
)
if errorlevel 1 (
    echo CMake configure failed
    exit /b 1
)

cmake --build .
if errorlevel 1 (
    echo Build failed
    exit /b 1
)

if "%STANDALONE%"=="ON" (
    echo Build OK: build\cstrike.exe build\steamclient.dll ^(standalone, hl from %HL_DIR%, HL_METAHHOOK=%METAHHOOK%, %BUILD_TYPE%, REVEMU2013=%REVEMU2013%^)
) else (
    echo Build OK: build\cstrike.exe build\steamclient.dll ^(%BUILD_TYPE%, REVEMU2013=%REVEMU2013%^)
)
endlocal
