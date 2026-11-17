# Running the Android build

Target: arm64 Android 10+ with Vulkan 1.1 (tested hardware: AYN Odin 3, Snapdragon 8 Elite / Adreno 830).
No game files are included in the APK — you need your own dump of Saints Row (Xbox 360).

## Build

```bat
build_apk.bat
```

This builds `libsaintsrow.so` with the NDK (`build_android.bat`), strips it into
`android/app/src/main/jniLibs/arm64-v8a/`, and runs Gradle. Output:
`android/app/build/outputs/apk/debug/app-debug.apk`.

Requirements on the PC: Android SDK + NDK 29.0.14206865 + CMake 3.31.6 (`%LOCALAPPDATA%\Android\Sdk`),
JDK 17, Vulkan SDK (shader compiler), and the Xendroid+ checkout in `tools/xendroid` with
`patches/xendroid-recomp-host-mode.patch` applied.

## Install

```bat
adb install -r android\app\build\outputs\apk\debug\app-debug.apk
```

## Copy the game files

The app reads the game from its own external files folder (no storage permission needed):

```
/sdcard/Android/data/com.saintsrow.port/files/game/
```

Push the folder that contains `default.xex` (the `layer*filler.bin` files are disc padding and can be
skipped):

```bat
adb shell mkdir -p /sdcard/Android/data/com.saintsrow.port/files/game
adb push "<Saints Row folder>\." /sdcard/Android/data/com.saintsrow.port/files/game
```

Start the app once before pushing so Android creates the `Android/data/com.saintsrow.port` folder.

## Logs

```bat
adb logcat -s SaintsRow
adb pull /sdcard/Android/data/com.saintsrow.port/files/userdata/saintsrow.log
```

Save data and caches live in `files/userdata/`.

## Custom Vulkan driver (Turnip) and GPU settings

- **Driver:** put an adrenotools driver package (unzipped: `meta.json` + the driver `.so`, e.g.
  `libvulkan_freedreno.so`) in `Android/data/com.saintsrow.port/files/driver/`. At start the app
  copies it to private storage and loads it through adrenotools; logcat tag `SaintsRowGPU` prints
  `Vulkan driver: <path>` or `system`. Empty the folder to return to the system driver.
- **Settings:** `files/userdata/gpu.toml` holds Xenia GPU cvars (`name = value` per line), applied
  before the GPU starts - useful to try settings without rebuilding.
