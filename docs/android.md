# Android port

Same recompiled game, RT64 (Vulkan) and whisper.cpp as the PC build, packed as an APK for
arm64 phones with Vulkan 1.1 (built for the user's Galaxy S24 FE).

## Build (Windows host, Git Bash)

1. Build the Windows version once (`tools\build.bat`): the Android build runs its
   `build/win/file_to_c.exe` on the host to embed RT64's shaders.
2. `tools/apply_patches.sh` (RT64 patch 0003 makes RT64 cross-compile for Android).
3. `sh tools/build_android.sh` → `build/android/libmain.so` (NDK 28, arm64-v8a, API 26).
4. `sh tools/package_android.sh` → `build/android/HeyYouPikachu.apk` (no Gradle: aapt2,
   javac, d8, zipalign, apksigner with the Android debug key).

Needs the Android SDK (`D:/AI/tools/android-sdk`: build-tools 36.1.0, platform 35, NDK
28.1.13356709), JDK 21, cmake + ninja, and SDL2 2.30.3 source in `android/third_party/SDL2`
(not in git; the official release zip, unpacked). Two local edits to that SDL copy:
`src/sensor/android/SDL_androidsensor.c` uses `ALooper_pollOnce` (NDK 28 removed `pollAll`). `android/sdl2-aaudio-blocking-write.patch` (apply in `android/third_party/SDL2`) makes the AAudio output write block instead of dropping what does not fit in 1 ms (crackling), gives the stream about 20 ms of buffer, and logs under-runs to log.txt.

## How it fits together

- `android/src/com/hyprecomp/pikachu/`
  - `LauncherActivity`: first start only: pick the ROM (Android file picker, copied to
    `files/baserom.us.z64`; the game validates it, keeps `files/data/hyp.n64.us.1.0.z64` and
    deletes the copy, a wrong file is deleted too so the next start asks again), unpack the
    voice model to `files/data/models/` (downloaded once from huggingface.co, SHA-1 checked, or
    unpacked from the APK when built with `HYP_APK_MODEL=1`; Skip = use the word list), ask
    for the microphone.
  - `HypActivity`: SDL's activity (loads `c++_shared`, `SDL2`, `main`) + the controls overlay.
  - `ControlsView`: on-screen N64 controller in the side bars of the 4:3 picture, multi-touch:
    stick, TALK (= Z, hold and speak), A, B, C buttons, L, R, Start, Words, Hide. Shows what
    the mic heard. `HypNative` = the JNI calls (`src/android/android_glue.cpp`).
  - Words: a list of the words the game listens for right now (grouped); tapping one says it.
- Native: `program_dir()` is the app's files dir; graphics are set to 2x N64 resolution, 4:3,
  no MSAA, high-precision framebuffer off (menu.cpp's non-Windows `attach`). whisper uses 4
  threads (phones mix fast and slow cores) and armv8.2-a dotprod/fp16.
- Back gesture is trapped (it would close the game); leave with Home.
- Background: SDL's APP_WILLENTERBACKGROUND pauses the runtime (patch N64ModernRuntime/0003:
  no VI/AI events, game clock shifted by the paused time) and the audio; on return RT64
  rebuilds its Vulkan surface from SDL's new ANativeWindow (patch rt64/0003). If Android
  kills the process in the background, the game still starts over (no save states).
- Full screen: the game activity lays out under the system bars and the camera cutout, so
  the 4:3 picture is the full screen height. Clock + battery are drawn top-left.

## Not verified yet

Nothing has run on a phone: speed of RT64 on mobile GPUs, Vulkan feature support, the
microphone path, and whether controllers work (SDL supports Bluetooth/USB pads; untested).

## Automatic reports

After a crash, or when Android ends the game abnormally (memory, ANR, signal...), the launcher
shows the report on the next start and sends it once to the report inbox: the Cloudflare Worker in
`reports-worker/` (https://hyp-reports.pinball-leaderboard.workers.dev, deploy with
`npx wrangler deploy` there; admin key `HYP_REPORT_KEY` in `D:\AI\.env`). `python tools/fetch_reports.py`
saves waiting reports to `reports/` (gitignored) and marks them fetched; `--list` only shows them.
