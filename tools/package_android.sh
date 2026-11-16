#!/bin/sh
# Pack build/android/libmain.so + SDL + the Java side (android/) into a signed APK without
# Gradle: aapt2 (resources + manifest), javac + d8 (classes.dex), zipalign, apksigner.
#   tools/package_android.sh        -> build/android/HeyYouPikachu.apk
# The voice model (models/ggml-base.en.bin) goes in as an uncompressed asset; the app unpacks
# it once. The ROM is never packed: the player picks their own copy on the phone.
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

rm -rf "$OUT"
mkdir -p "$OUT/res" "$OUT/gen" "$OUT/classes" "$OUT/assets" "$OUT/lib/arm64-v8a"

# Resources + manifest
"$BT/aapt2.exe" compile --dir android/res -o "$OUT/res/res.zip"
if [ -f "$MODEL" ]; then cp "$MODEL" "$OUT/assets/ggml-base.en.bin"; else echo "warning: no $MODEL (voice off)"; fi
"$BT/aapt2.exe" link -o "$OUT/base.apk" -I "$JAR" --manifest android/AndroidManifest.xml \
    --java "$OUT/gen" -A "$OUT/assets" -0 bin --min-sdk-version 26 --target-sdk-version 35 \
    "$OUT/res/res.zip"

# Java: SDL's activity classes + ours
SDLJAVA=android/third_party/SDL2/android-project/app/src/main/java
find "$SDLJAVA" android/src "$OUT/gen" -name "*.java" | while read -r f; do cygpath -m "$f"; done > "$OUT/sources.txt"
"$JDK/bin/javac.exe" --release 11 -nowarn -classpath "$JAR" -d "$OUT/classes" @"$OUT/sources.txt"
CLASSES=$(cd "$OUT/classes" && find . -name "*.class" | sed 's|^\./||')
(cd "$OUT/classes" && cmd //c "$(cygpath -w "$BT/d8.bat")" --release --min-api 26 --lib "$(cygpath -w "$JAR")" --output .. $CLASSES)

# Native libraries (stripped of debug info)
for so in build/android/libmain.so build/android/libSDL2.so "$CXXSHARED"; do
    "$STRIP" --strip-unneeded -o "$OUT/lib/arm64-v8a/$(basename "$so")" "$so"
done

# Assemble, align, sign
python -I tools/apk_add.py "$OUT/base.apk" "$OUT/unsigned.apk" "$OUT/classes.dex" "$OUT/lib"
"$BT/zipalign.exe" -f -p 4 "$OUT/unsigned.apk" "$OUT/aligned.apk"
cmd //c "$(cygpath -w "$BT/apksigner.bat")" sign --ks "$(cygpath -w "$KEYSTORE")" --ks-pass pass:android \
    --ks-key-alias androiddebugkey --key-pass pass:android \
    --out "$(cygpath -w build/android/HeyYouPikachu.apk)" "$(cygpath -w "$OUT/aligned.apk")"
ls -la build/android/HeyYouPikachu.apk
echo APK OK
