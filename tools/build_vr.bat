@echo off
rem PC VR test build (OpenXR, Vulkan) into build\win-vr. Run from the repo root.
rem Test it with the Meta XR Simulator: set XR_RUNTIME_JSON to its meta_openxr_simulator.json for that run only.
setlocal
set BUILD_DIR=build\win-vr

set VSROOT=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%

if not exist %BUILD_DIR%\CMakeCache.txt (
    cmake -S . -B %BUILD_DIR% -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
        -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl ^
        -DHYP_ENABLE_VR=ON ^
        -DFETCHCONTENT_BASE_DIR=%CD%\build\_deps || exit /b 1
)
cmake --build %BUILD_DIR% --target HeyYouPikachuRecompiled || exit /b 1
echo BUILD OK
