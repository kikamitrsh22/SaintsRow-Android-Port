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
