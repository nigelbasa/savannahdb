@echo off
REM Add the VS-bundled CMake to PATH so cmake-js can find it, then compile.
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"
REM Point CMake at vcpkg so find_package(bson) resolves the static libbson we installed.
set "VCPKG_ROOT=C:\vcpkg"
cmake-js compile --CDCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake --CDVCPKG_TARGET_TRIPLET=x64-windows-static-md %*
