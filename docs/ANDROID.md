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

## Audio

The game mixes in software and needs the console's XMA hardware decoder. `runtime/src/audio.cpp`
runs one host thread per XAudio render client that calls the game's mixer whenever the output has
room for a frame (6 x 256 big-endian floats); `runtime/src/xglue/xglue_audio.cpp` holds Xenia's XMA
decoder and the output device. On Android the decoder is compiled in host mode
(`XE_APU_HOST_MODE`, part of `patches/xendroid-recomp-host-mode.patch`: std::thread worker, no
guest CPU) with FFmpeg, and the output is Xendroid's AAudio driver. Windows is still silent
(`xglue_audio_none.cpp`): its Xenia build has no APU / FFmpeg yet.

## Performance

The game holds itself to 30 fps; the "Unlock FPS" patch (mid-asm hooks at `0x82201830` /
`0x82370D40`) lifts that, and the Havok physics step follows the real frame time
(`runtime/src/game/overrides.cpp`). Measured in the city on the Odin 3 (Normal mode, cool start):
~22 fps before, a locked 60 after. What got it there:

- Android Performance Hint session over the game's threads (`runtime/src/perf_hint.cpp`); the main
  thread and the GPU command thread are pinned to the fastest cores (`no_pin_fast_cores` in
  `userdata` turns that off).
- Recompiled code built with `-mcpu=oryon-1` (Snapdragon 8 Elite; `SR_GENERIC_ARM64=ON` for other
  phones), no stack protector.
- Kernel waits wake only the threads waiting on the signalled object (was: every waiting thread).
- Native memset/memcpy for the guest CRT; the D3D GPU-fence wait sleeps after 300 us instead of
  spinning (`spin_gpu_wait` in `userdata` restores the spin).

The Odin 3's performance mode and temperature cap the CPU (Normal mode: 1.15 / 1.96 GHz); when it
throttles, the city drops toward 45 fps.
