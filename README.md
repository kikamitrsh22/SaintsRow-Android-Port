# Saints Row (2006) for Android — native port

A native Android (ARM64 / Vulkan) port of **Saints Row** for Xbox 360, made by **static
recompilation**: the game's PowerPC code is translated to C++ with
[XenonRecomp](https://github.com/hedge-dev/XenonRecomp) and compiled for ARM64. The console's GPU,
audio decoder and memory are emulated with parts of
[Xendroid+](https://github.com/phforner0/Xendroid-Plus) (an Android fork of
[Xenia](https://github.com/xenia-project/xenia)); the Xbox 360 kernel is reimplemented in
`runtime/`.

> **No game files are included.** You need your own dump of Saints Row (Xbox 360, Title ID
> `545107D1`, the original release — not the TU1 update). Nothing from the game is in this repo:
> the recompiled code (`ppc/`) is generated on your PC from your own `default.xex`.

Tested on an **AYN Odin 3** (Snapdragon 8 Elite, Adreno 830, Android 15).

## Status

| | |
|---|---|
| Boot, menus, New Game, player creation | Working |
| Free roam in the city, driving | Working (full campaign not tested yet) |
| Audio (music, voices, effects — XMA decoding) | Working |
| Saving and loading | Working |
| Frame rate | 60 FPS in the city on the Odin 3 (30 / 40 / 60 / 120 selectable) |
| Controllers | Android gamepads (Xbox layout), including the Odin 3's built-in controls |

Known issues:

- **GPU hangs with some Turnip driver builds.** With the "Turnip Gen8 V39 patched" driver the game
  sometimes closes with `Graphics device lost` (logged in `xenia.log`). FXAA is turned off
  automatically while a custom driver is loaded, because it reliably hung the GPU when getting into
  a car.
- **The stock Adreno driver draws building shadows wrong** (blocky light patches in shaded areas).
  A Turnip driver fixes that — hence the trade-off above.
- Heavy scenes are limited by the CPU thread that translates the game's draw calls to Vulkan; the
  Odin 3's *Normal* performance mode caps that core. A higher performance mode helps the most.

## Install and play

1. **Build the APK** (see [Building](#building)) or get it from whoever built it for you, and
   install it:
   ```bat
   adb install -r app-debug.apk
   ```
2. **Start the app once** so Android creates its folder, then copy your game — the folder that
   contains `default.xex` — to:
   ```
   /sdcard/Android/data/com.saintsrow.port/files/game/
   ```
   For example from a PC:
   ```bat
   adb push "<your Saints Row folder>\." /sdcard/Android/data/com.saintsrow.port/files/game
   ```
   (The `layer*filler.bin` files are disc padding and can be left out.)
3. **Optional — custom Vulkan driver (Turnip):** unzip an adrenotools driver package (`meta.json`
   plus the driver `.so`) into `/sdcard/Android/data/com.saintsrow.port/files/driver/`. Empty that
   folder to go back to the phone's own driver.
4. Start the app and play with a controller.

## In-game menu

Tap the **MENU** button in the top right corner, or press **L3 + R3** on the controller.

| Setting | Options | Applies |
|---|---|---|
| Frame rate limit | 30, 40, 60, 120 FPS (40 and 120 switch the screen to 120 Hz) | At once |
| Resolution | 1280×720 (native), 2560×1440, 3840×2160 | Next start |
| Anti-aliasing | Off, FXAA, FXAA Extreme (off with a custom driver) | At once |
| Frame generation | Off, ZeroFG (generated in-between frames) | Next start |
| Performance overlay | Off, Simple, Detailed (FPS, 1% low, CPU, GPU, temperatures, RAM) | At once |

Settings are remembered between runs.

## Files on the phone

Everything lives in `/sdcard/Android/data/com.saintsrow.port/files/`:

| Path | Contents |
|---|---|
| `game/` | Your game files |
| `driver/` | Optional custom Vulkan driver |
| `userdata/save/` | Save games (one folder per save) |
| `userdata/cache/` | Shader / pipeline cache (speeds up later sessions; safe to delete) |
| `userdata/saintsrow.log` | Port log (frame rate, frame spikes, file access) |
| `userdata/xenia.log` | GPU / audio emulation log (Xenia) |
| `userdata/gpu.toml` | Optional Xenia GPU settings (`name = value`), read at start |

Switch files in `userdata/` for testing (an empty file with this name turns the behaviour on):
`no_pin_fast_cores` (don't keep the game's main and GPU threads on the fastest cores) and
`spin_gpu_wait` (busy-wait for the GPU instead of sleeping).

## Building

On a Windows PC with: Visual Studio 2022 Build Tools, LLVM (clang-cl), CMake + Ninja, Python 3,
the Android SDK with NDK **29.0.14206865** and CMake 3.31.6 (in `%LOCALAPPDATA%\Android\Sdk`),
JDK 17, and the Vulkan SDK (for the shader compiler).

1. **XenonRecomp** (with this project's patch) and the recompiled game code:
   ```bat
   git clone --recursive https://github.com/hedge-dev/XenonRecomp tools/XenonRecomp
   git -C tools/XenonRecomp apply ../../patches/xenonrecomp-missing-instructions.patch
   build_xenonrecomp.bat
   ```
   Put your game next to this repository so that `config/SaintsRow.toml` finds
   `../../Saints_Row_1_Modded.8/Saints Row 1 Modded/default.xex` (or edit `file_path` there), then:
   ```bat
   tools\XenonRecomp\build\XenonRecomp\XenonRecomp.exe config\SaintsRow.toml tools\XenonRecomp\XenonUtils\ppc_context.h
   ```
   This writes the recompiled game to `ppc/` (git-ignored, never commit it).
2. **Xendroid+** (GPU, audio decoder and Vulkan driver loading), with this project's patch:
   ```bat
   git clone https://github.com/phforner0/Xendroid-Plus tools/xendroid
   git -C tools/xendroid checkout e4003ba66
   git -C tools/xendroid submodule update --init --recursive
   git -C tools/xendroid apply ../../patches/xendroid-recomp-host-mode.patch
   ```
3. **APK:**
   ```bat
   build_apk.bat
   ```
   Output: `android/app/build/outputs/apk/debug/app-debug.apk`.

The build targets the Snapdragon 8 Elite (`-mcpu=oryon-1`). For other ARMv8.2+ phones, configure
the native build with `-DSR_GENERIC_ARM64=ON`.

More technical notes — audio, performance work, the game-speed fix, occlusion queries — are in
[docs/ANDROID.md](docs/ANDROID.md); fixes ported from the Saints Reborn project are in
[docs/SAINTSREBORN_FINDINGS.md](docs/SAINTSREBORN_FINDINGS.md).

## Repository layout

| Path | |
|---|---|
| `config/` | XenonRecomp configuration: function bounds, jump tables, mid-asm hooks |
| `runtime/` | Xbox 360 kernel / XAM reimplementation, memory, file system, audio, game fixes (`src/game/overrides.cpp`), the bridge to Xenia (`src/xglue/`) |
| `android/` | Android app (Java) and the native build (`android/native/CMakeLists.txt`) |
| `patches/` | Changes to XenonRecomp and Xendroid+ |
| `tools/` | Analysis helpers (switch tables, function splits, imports) |
| `docs/` | Technical notes |

## Credits

- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) — the PowerPC → C++ recompiler
- [Xenia](https://github.com/xenia-project/xenia) and [Xendroid+](https://github.com/phforner0/Xendroid-Plus) — GPU, XMA audio and Android Vulkan driver support
- [Saints Reborn](https://github.com/whompay/SaintsReborn) — its documented fixes for the same game guided several of ours
- Game patches ("Unlock FPS", Havok step fix) from the Xenia game-patches community

Saints Row is a trademark of its owners. This project is not affiliated with them and contains no
game assets; you must own the game.
