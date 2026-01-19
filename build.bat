@echo off
setlocal EnableDelayedExpansion

echo ========================================
echo   Enfusion Unpacker Build Script
echo ========================================
echo.

:: Check for vcpkg
set VCPKG_ROOT=C:\vcpkg
if not exist "%VCPKG_ROOT%\vcpkg.exe" (
    echo ERROR: vcpkg not found at %VCPKG_ROOT%
    echo Please install vcpkg or update VCPKG_ROOT path
    pause
    exit /b 1
)

:: Create build directory
if not exist "build" mkdir build
cd build

:: Configure with CMake
echo Configuring CMake...
cmake .. -G "Visual Studio 17 2022" -A x64 ^
    -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" ^
    -DVCPKG_TARGET_TRIPLET=x64-windows

if errorlevel 1 (
    echo.
    echo CMake configuration failed!
    echo.
    echo If you don't have Visual Studio 2022, try:
    echo   cmake .. -G "Visual Studio 16 2019" -A x64 ...
    echo.
    pause
    exit /b 1
)

:: Build
echo.
echo Building Release configuration...
cmake --build . --config Release

if errorlevel 1 (
    echo.
    echo Build failed!
    pause
    exit /b 1
)

echo.
echo ========================================
echo   Build Successful!
echo ========================================
echo.
echo Executable: build\Release\EnfusionUnpacker.exe
echo.

:: Copy executable to root
if exist "Release\EnfusionUnpacker.exe" (
    copy /Y "Release\EnfusionUnpacker.exe" "..\EnfusionUnpacker.exe"
    echo Copied to: EnfusionUnpacker.exe
)

pause
