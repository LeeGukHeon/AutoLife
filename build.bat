@echo off
setlocal

echo [INFO] AutoLife Build Script
echo [INFO] Using CMake from D:\MyApps\vcpkg...

set CMAKE_PATH="D:\MyApps\vcpkg\downloads\tools\cmake-3.31.10-windows\cmake-3.31.10-windows-x86_64\bin\cmake.exe"

if not exist %CMAKE_PATH% (
    echo [ERROR] CMake not found at %CMAKE_PATH%
    exit /b 1
)

if not exist build (
    mkdir build
)

cd build
%CMAKE_PATH% ..
if errorlevel 1 (
    echo [ERROR] CMake configuration failed.
    exit /b 1
)

%CMAKE_PATH% --build . --config Release
if errorlevel 1 (
    echo [ERROR] Build failed.
    exit /b 1
)

echo [SUCCESS] Build completed successfully.
endlocal
