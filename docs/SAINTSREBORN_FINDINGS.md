# Lessons from Saints Reborn

[Saints Reborn](https://github.com/whompay/SaintsReborn) is a finished, fully playable static
recompilation of the same game (same `default.xex`: its ABI helper addresses match ours exactly),
built on the ReXGlue SDK. Its `config/saintsrow_manifest.toml` and `project/src/stubs.cpp` document
every game-specific fix. Below: what applies to our port, ranked by how likely it is to crash or
hang us. Addresses are guest addresses in our build.

## Already covered

| Theirs | Ours |
|---|---|
| ABI save/restore helpers | Same addresses in `config/SaintsRow.toml` |
| 21 vtable-only functions (`[entrypoint.functions]`) | All present in our function table (data-pointer scan + extra bounds) |
| Readable null page | `protect_zero=false` (but see P1-1: only 64 KB) |
| User 0 signed in locally | `XamUserGetSigninState` |
| `u32` jump tables, guest CPU numbers, achievement enumerators | Fixed here independently |

## P1 — likely crashes / hangs

**Status: all seven applied** (`runtime/src/game/overrides.cpp`, `ppc_indirect.h`, `imports_ke.cpp`,
`imports_xam.cpp`, `xglue_memory.cpp`, mid-asm hooks in `config/SaintsRow.toml`). With them the
Odin 3 gets from New Game through player creation into the city.

1. **Near-null memory is only 64 KB.** They commit guest `0x00000000–0x00FFFFFF` (16 MB) as zeroed
   RW memory and also skip faulting near-null reads. Xenia's `protect_zero=false` maps 64 KB, so a
   read of `null + 0x10000` or more still kills us. Fix: `AllocFixed(0, 0x1000000)` in the low heap.
2. **Indirect calls to unknown addresses exit the process.** Their SDK returns 0 instead (logged).
   Our `PPCIndirectCallFailed` does `_Exit(4)` — that is the "game closes by itself" we saw on New
   Game. Fix: log once per (target, lr), set `r3 = 0`, return.
3. **Semaphore re-initialisation orphans waiting threads.** The game re-runs `KeInitializeSemaphore`
   on its IO semaphores in place. We register a *new* host object, so IO threads already waiting on
   the old one never wake (our monitor shows t5–t13 parked for minutes at `lr=8271C320`). Fix: on
   re-init keep the existing object and set its count (they use `count == 0 ? 2 : count`).
4. **Loader waits forever for sounds that never finish.** `sub_82105B18` ("is this object done?",
   used by the post-*Begin Game* loader) polls sounds; with no audio they never complete. Fix: after
   6 s busy, report done. Directly relevant: our audio is silent.
5. **Streaming list filter hangs on sentinel arguments.** `sub_8265F4F0` is called with small
   sentinel values (`0xF`, `0x04000000`) instead of pointers and spins. Fix: return 0 for
   `0 < r3 < 0x10000000`, plus the three byte-clamp mid-asm hooks at `0x8265F50C` (r10),
   `0x8265F554` (r7), `0x8265F660` (r8).
6. **`XamLoaderTerminateTitle` exits the app.** They park the calling thread instead. Ours calls
   `std::exit(0)`.
7. **Render-entry function-pointer gate.** `sub_8262FFE0` (per-frame render) and `sub_82185498`
   (main loop) make an indirect call through `[0x8370F248]` gated by bytes `0x8370E9DF` /
   `0x8370E9F6`; they clear the gates while the pointer is not valid code. With P1-2 this becomes a
   silent no-op instead of a crash, but the gate fix is cleaner.

## P2 — corruption / spin / correctness

- `sub_82106580` resource-cache find-or-insert with key 0 rewrites the tree's shared sentinel and
  corrupts the cache for everyone → return 0 for key 0.
- `sub_82656CA8` inflate decoder spins forever with null tables → validate `state+28`, `inner+68/72`.
- Null-string guards: `sub_826FE350` (strcmp), `sub_826FE160` (strncpy), `sub_826FFD30` (strchr),
  `sub_82643170` (append).
- Mid-asm hook `0x821061BC` (r28): skip null voice dispatch, jump to `0x821061D8` (needed once audio
  runs).
- `KeInitializeEvent` re-init: same idea as semaphores (the game initialises events inline; we create
  host objects lazily, so check whether re-init changes their type/signal state).
- `XamContentGetLicenseMask` → all bits granted (not imported by our XEX build, check if it shows up).

## P3 — once audio / performance work starts

- XAudio engine creation `sub_8260CC50` is called repeatedly and spawns threads each time; after 200
  re-creations they reuse thread handles (otherwise `STATUS_NO_MEMORY`).
- Native `memset` `sub_82702DD0` / `memmove` `sub_82702900` (hot).
- Havok physics step follows frame time above 30 fps (Juiced Patch port) — only if we unlock 60 fps.
- Their GPU fixes (nested IB ring wrap, WAIT_REG_MEM deadlock, white unbound textures, occlusion
  queries) target ReXGlue's fork; Xenia/Xendroid's command processor already handles nested IBs and
  waits, but occlusion queries ("lights through walls") may need the `sub_825D4748` override later.

## Found while testing (not from Saints Reborn)

- **Game pauses by itself.** Two functions reached only by tail calls (`0x8216DC18`, `0x8216DF58`)
  were merged into their neighbours by our explicit function bounds, so the recompiler dropped the
  tail call from the "is action pressed" helpers (`// ERROR` in the generated code) and they
  returned garbage - the game saw Start as pressed. Fixed in `config/SaintsRow.toml`;
  `tools/check_split.py` checks whether a function can be split at an address. The remaining
  `// ERROR` targets (`0x8270F...`-`0x8271B...`) are exception funclets in the system library.
- **The game reads the pad twice per frame** and uses only the second read, so a tap must be held
  for a minimum time rather than for one read (`runtime/src/input.cpp`).
- **Occlusion queries are fine.** The device's "direct" bit is 0 in our port, so the original
  `sub_825D4748` already reads real results; no override needed.
- **Blocky / leaking building shadows on Android** are a bug of the stock Adreno 830 driver, not of
  the port: correct on Windows (Vulkan) and on the Odin 3 with a Turnip driver (see
  `docs/ANDROID.md`).
