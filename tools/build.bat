@echo off
rem Configure + build the PC port with clang-cl (VS 2022 Build Tools) into build\win.
rem Usage: tools\build.bat [Release|RelWithDebInfo|Debug]   (run from the repo root)
setlocal
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=RelWithDebInfo

set VSROOT=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%

if not exist build\win\CMakeCache.txt (
    cmake -S . -B build\win -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% ^
        -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl ^
        -DFETCHCONTENT_BASE_DIR=%CD%\build\_deps || exit /b 1
)
cmake --build build\win --target HeyYouPikachuRecompiled || exit /b 1
echo BUILD OK
