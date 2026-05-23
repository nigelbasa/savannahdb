@echo off
REM Add the VS-bundled CMake to PATH so cmake-js can find it, then compile.
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"
REM Point CMake at the VS-bundled vcpkg so find_package(bson) resolves libbson
REM from the manifest (vcpkg.json) without requiring a separate vcpkg checkout.
REM Use the 8.3 short path because cmake-js splits --CDvar=value on spaces.
REM Override VCPKG_ROOT to use a different vcpkg install.
if "%VCPKG_ROOT%"=="" set "VCPKG_ROOT=C:\PROGRA~2\MICROS~3\18\BUILDT~1\VC\vcpkg"
cmake-js compile --CDCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake --CDVCPKG_TARGET_TRIPLET=x64-windows-static-md %*
