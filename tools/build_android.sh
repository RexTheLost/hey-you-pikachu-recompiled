#!/bin/sh
# Build the Android port: native code (NDK, arm64) into build/android, then the APK
# (tools/package_android.sh). Run from the repo root in Git Bash.
#   tools/build_android.sh [Release|RelWithDebInfo]
#   HYP_VR=1 tools/build_android.sh   -> the Meta Quest VR build in build/android-vr (OpenXR, docs/vr-plan.txt)
# Needs: the Windows build once (its host file_to_c.exe), Android SDK + NDK, JDK, cmake + ninja.
set -e
CONFIG=${1:-Release}
SDK=${ANDROID_SDK:-D:/AI/tools/android-sdk}
NDK=${ANDROID_NDK:-$SDK/ndk/28.1.13356709}
HOST_FILE_TO_C=${HOST_FILE_TO_C:-$PWD/build/win/file_to_c.exe}
BUILD_DIR=build/android
EXTRA=""
if [ "$HYP_VR" = "1" ]; then
    BUILD_DIR=build/android-vr
    OPENXR_LOADER=${OPENXR_LOADER:-D:/AI/tools/openxr-android/arm64-v8a/libopenxr_loader.so}
    EXTRA="-DHYP_ENABLE_VR=ON -DOPENXR_ANDROID_LOADER=$OPENXR_LOADER"
fi

if [ ! -f "$HOST_FILE_TO_C" ]; then
    echo "missing $HOST_FILE_TO_C: build the Windows version first (tools\\build.bat)"; exit 1
fi
if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    cmake -S . -B "$BUILD_DIR" -G Ninja \
        -DCMAKE_BUILD_TYPE="$CONFIG" \
        -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DANDROID_STL=c++_shared \
        -DRT64_HOST_FILE_TO_C="$HOST_FILE_TO_C" $EXTRA
fi
cmake --build "$BUILD_DIR" --target HeyYouPikachuRecompiled
echo NATIVE OK
