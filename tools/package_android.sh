#!/bin/sh
# Pack build/android/libmain.so + SDL + the Java side (android/) into a signed APK without
# Gradle: aapt2 (resources + manifest), javac + d8 (classes.dex), zipalign, apksigner.
#   tools/package_android.sh        -> build/android/HeyYouPikachu.apk
#   HYP_VR=1 tools/package_android.sh -> build/android-vr/HeyYouPikachuVR.apk (Meta Quest, sideload;
#                                       package com.hyprecomp.pikachu.vr, android/AndroidManifest-vr.xml)
#   HYP_VR=1 HYP_VR_ROM=<rom.z64> ...  -> build/android-vr/HeyYouPikachuVR-personal.apk with that ROM packed in
#                                       (assets/baserom.us.z64). NEVER publish it: one-time link only (tools/vr_release.py).
# The voice model is downloaded by the app on first start (HYP_APK_MODEL=1 packs it in as an
# uncompressed asset instead). The ROM is never packed: the player picks their own copy.
set -e
SDK=${ANDROID_SDK:-D:/AI/tools/android-sdk}
NDK=${ANDROID_NDK:-$SDK/ndk/28.1.13356709}
BT=$SDK/build-tools/36.1.0
JAR=$SDK/platforms/android-35/android.jar
JDK=${JAVA_HOME:-"C:/Program Files/Eclipse Adoptium/jdk-21.0.12.101-hotspot"}
KEYSTORE=${HYP_KEYSTORE:-"$APPDATA/Godot/keystores/debug.keystore"}   # Android debug key
STRIP=$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip.exe
CXXSHARED=$NDK/toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so
OUT=build/android/apk
MODEL=models/ggml-base.en.bin
NATIVE=build/android
MANIFEST=android/AndroidManifest.xml
APK=build/android/HeyYouPikachu.apk
RENAME=""
EXTRA_SO=""
if [ "$HYP_VR" = "1" ]; then
    NATIVE=build/android-vr
    OUT=build/android-vr/apk
    MANIFEST=android/AndroidManifest-vr.xml
    APK=build/android-vr/HeyYouPikachuVR.apk
    RENAME="--rename-manifest-package com.hyprecomp.pikachu.vr"
    EXTRA_SO=${OPENXR_LOADER:-D:/AI/tools/openxr-android/arm64-v8a/libopenxr_loader.so}
    if [ -n "$HYP_VR_ROM" ]; then APK=build/android-vr/HeyYouPikachuVR-personal.apk; fi
fi

rm -rf "$OUT"
mkdir -p "$OUT/res" "$OUT/gen" "$OUT/classes" "$OUT/assets" "$OUT/lib/arm64-v8a"

# Resources + manifest
"$BT/aapt2.exe" compile --dir android/res -o "$OUT/res/res.zip"
# HYP_APK_MODEL=1 packs the voice model in (a 150 MB APK); by default the app downloads it once.
if [ "$HYP_APK_MODEL" = "1" ]; then cp "$MODEL" "$OUT/assets/ggml-base.en.bin"; fi
# HYP_VR_ROM: a personal build with the player's own ROM (LauncherActivity copies it out on first start).
if [ -n "$HYP_VR_ROM" ]; then cp "$HYP_VR_ROM" "$OUT/assets/baserom.us.z64"; fi
"$BT/aapt2.exe" link -o "$OUT/base.apk" -I "$JAR" --manifest "$MANIFEST" $RENAME \
    --java "$OUT/gen" -A "$OUT/assets" -0 bin --min-sdk-version 26 --target-sdk-version 35 \
    "$OUT/res/res.zip"

# Java: SDL's activity classes + ours
SDLJAVA=android/third_party/SDL2/android-project/app/src/main/java
find "$SDLJAVA" android/src "$OUT/gen" -name "*.java" | while read -r f; do cygpath -m "$f"; done > "$OUT/sources.txt"
"$JDK/bin/javac.exe" --release 11 -nowarn -classpath "$JAR" -d "$OUT/classes" @"$OUT/sources.txt"
CLASSES=$(cd "$OUT/classes" && find . -name "*.class" | sed 's|^\./||')
(cd "$OUT/classes" && cmd //c "$(cygpath -w "$BT/d8.bat")" --release --min-api 26 --lib "$(cygpath -w "$JAR")" --output .. $CLASSES)

# Native libraries (stripped of debug info)
for so in "$NATIVE/libmain.so" "$NATIVE/libSDL2.so" "$CXXSHARED" $EXTRA_SO; do
    "$STRIP" --strip-unneeded -o "$OUT/lib/arm64-v8a/$(basename "$so")" "$so"
done

# Assemble, align, sign
python -I tools/apk_add.py "$OUT/base.apk" "$OUT/unsigned.apk" "$OUT/classes.dex" "$OUT/lib"
"$BT/zipalign.exe" -f -p 4 "$OUT/unsigned.apk" "$OUT/aligned.apk"
cmd //c "$(cygpath -w "$BT/apksigner.bat")" sign --ks "$(cygpath -w "$KEYSTORE")" --ks-pass pass:android \
    --ks-key-alias androiddebugkey --key-pass pass:android \
    --out "$(cygpath -w "$APK")" "$(cygpath -w "$OUT/aligned.apk")"
ls -la "$APK"
echo APK OK
