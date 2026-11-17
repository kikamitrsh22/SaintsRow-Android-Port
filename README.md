# Saints Row (2006) native port

Static recompilation of Saints Row (Xbox 360, Title ID `545107D1`, XEX version 1) with
[XenonRecomp](https://github.com/hedge-dev/XenonRecomp), aiming for a native Android (ARM64/Vulkan) build.

No game files are included. You need your own dump of the game.

## Layout
- `config/SaintsRow.toml` - XenonRecomp config (register helpers, setjmp/longjmp, explicit function bounds)
- `config/SaintsRow_switch_tables.toml` - jump tables from XenonAnalyse, plus two manual fixes
- `patches/` - changes to XenonRecomp (missing instructions: dcbst, vandc, vpkswss, mulhdu, frsqrte)
- `tools/xexdump` - decrypts/decompresses `default.xex` to a flat image for Ghidra/IDA
- `tools/*.py` - analysis helpers (.pdata parser, pattern scan, switch bound fixer)

## Setup
```
git clone --recursive https://github.com/hedge-dev/XenonRecomp tools/XenonRecomp
git -C tools/XenonRecomp apply ../../patches/xenonrecomp-missing-instructions.patch
build_xenonrecomp.bat
tools\XenonRecomp\build\XenonRecomp\XenonRecomp.exe config\SaintsRow.toml tools\XenonRecomp\XenonUtils\ppc_context.h
```
Output goes to `ppc/` (git-ignored).

## Running on Windows

`run_windows.bat` starts `build\SaintsRow.exe` with the game folder (log: `work\win_run.log`).
An XInput controller works as-is. Keyboard and mouse are merged in while the game window has focus,
laid out like Saints Row 2 on PC (layout from Saints Reborn):

| On foot | | In a vehicle | |
|---|---|---|---|
| W A S D | Move | W / S | Accelerate / brake, reverse |
| Mouse | Camera (hold Alt to free the cursor) | A / D | Steer |
| Left mouse | Fire / attack | Left mouse | Drive-by attack |
| Right mouse | Secondary attack | Space | Handbrake |
| Space | Jump | Shift | Nitrous |
| Shift | Sprint | Ctrl | Hydraulics |
| E | Action / enter car | E | Exit car |
| R | Reload / pick up | Z / C / X | Look left / right / back |
| F / C | Kick / crouch | | |

| Everywhere | | Menus | |
|---|---|---|---|
| Esc or M | Pause | Arrows or W / S | Up / down |
| Tab | Map | Enter, Space or left mouse | Select |
| Q (hold) | Weapon wheel (pick with W A S D) | Esc, Backspace or right mouse | Back |
| Arrows | D-pad (recruit, radio) | Q / E | Previous / next tab |
| Middle mouse or V | Right stick click | X / Y | X / Y actions (Y = Begin Game, Resume) |

`SR_AUTOPRESS=14S,23A,50Y` presses buttons at the given seconds (A, B, X, Y, S = Start, K = Back)
for unattended tests.
