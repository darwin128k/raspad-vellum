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
set "HL_DIR="

:parse_args
if "%~1"=="" goto args_done
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
    cmake -G "Ninja" -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=cl -DREVLOADER_STANDALONE=ON -DHL_METAHHOOK=%METAHHOOK% -DHL_DIR="%HL_DIR%" ..
) else (
    cmake -G "Ninja" -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=cl -DREVLOADER_STANDALONE=OFF ..
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
    echo Build OK: build\cstrike.exe build\steamclient.dll ^(standalone, hl from %HL_DIR%, HL_METAHHOOK=%METAHHOOK%^)
) else (
    echo Build OK: build\cstrike.exe build\steamclient.dll
)
endlocal
