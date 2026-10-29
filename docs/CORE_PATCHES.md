# Core patches

Every change made inside `mds_upstream/` or `borealis/` is listed here so we can rebase
onto upstream. Prefer `#ifdef __SWITCH__` guards.

The three dependencies are git submodules pinned to upstream commits. Our changes live
in `patches/melonDS.patch` and `patches/borealis.patch`, which `scripts/apply_patches.sh`
applies (it skips a patch that is already applied; `scripts/build.sh` runs it first).
After editing a submodule, regenerate its patch:

```
git -C mds_upstream diff > patches/melonDS.patch
git -C borealis diff > patches/borealis.patch
```

To rebase: in the submodule, `git checkout -- .`, check out the new upstream commit,
re-apply the patch (fix conflicts by hand), regenerate it, and commit the new submodule
pointer together with the patch.

Upstream bases:
- melonDS-emu/melonDS @ `7d8dea42`
- XITRIX/borealis @ `8c1f1093` (built with `PLATFORM_SWITCH=ON USE_DEKO3D=ON`; two patches, below)
- RetroAchievements/rcheevos @ `a2f31ea` (v11.6.0+43, in `rcheevos/`; no patches)

| File | Change | Reason |
|---|---|---|
| `src/ARMJIT_A64/ARMJIT_Compiler.cpp` | On `__SWITCH__`: include `<switch.h>`, declare `__start__`, define `JitMemSize` (16 MB); skip `ARMJIT_Global::FreeCodeMem`/`DeInit` in `~Compiler` | The Switch ctor maps its own RX/RW code alias next to the executable and never takes an `ARMJIT_Global` slice or ref; the symbols were dropped upstream (values from the working umby24/melonDS Switch fork) |
| `src/ARMJIT_Global.cpp` | On `__SWITCH__`: no `sys/mman.h`, no static `CodeMemory` array, `AllocateCodeMem`/`FreeCodeMem` are no-ops, `Init` skips `mprotect` | newlib has no mman; the static array is 128 MB of BSS that the Switch path never uses |
| `src/ARMJIT_Memory.h` | On `__SWITCH__`: `HandleSwitchException` static member; `MemoryHeap` field | Exception handler needs the private fault types; heap pointer kept separately (below) |
| `src/ARMJIT_Memory.cpp` | Switch exception handler rewritten for the instance API (`NDS::Current`, `JIT.Memory.FastMem*Start`, `FaultHandler(desc, nds)`); returns for foreign faults instead of calling the fork's `HandleFault` | Upstream still had the pre-refactor global API and didn't compile |
| `src/ARMJIT_Memory.cpp` | `Mapping::Unmap`: `memory.UnmapFromRange` → `nds.JIT.Memory.UnmapFromRange` | Stale identifier in the Switch-only branch |
| `src/ARMJIT_Memory.cpp` | `IsFastMemSupported`: Switch branch (4 KB pages, supported); `Register/UnregisterFaultHandler`: Switch no-op | No `sysconf`/`sigaction` on newlib; the libnx handler is a link-time override |
| `src/ARMJIT_Memory.cpp` | ctor: heap buffer kept in `MemoryHeap`, `MemoryBase = MemoryBaseCodeMem`; generic `FastMem*Start = MemoryBase+...` lines skipped on Switch; dtor unmaps/frees `MemoryHeap` | `svcMapProcessCodeMemory` makes the source heap inaccessible, so RAM getters must use the alias; the generic lines overwrote the Switch ASLR reservations |
| `src/ARMJIT_Memory.cpp`, `src/ARMJIT_A64/ARMJIT_Compiler.cpp` | Switch ctors: log (`Platform::Log` Error) each failed allocation / svc call with its result code, plus the chosen addresses (Info); asserts unchanged | Release builds compile the asserts out, so setup failures were silent and crashed later |
| `src/GPU.h` | `VRAMCaptureBlockFlags[16]` value-initialized (`{}`) | Upstream bug, not Switch-specific: `GPU::GPU` → `SetRenderer` → `SyncAllVRAMCaptures` reads the flags before any `Reset` and, if garbage marks a block as an unsynced capture, calls `Rend->SyncVRAMCapture` on the still-null renderer. Desktop heaps hand out zeroed pages for the large `NDS` allocation so it never shows; on Switch it crashed `NDS` construction at random (crash report 2026-10-26: data abort in `GPU::SetRenderer`). Worth upstreaming. The host also constructs `NDS` on zeroed memory as a safety net |

### borealis

| File | Change | Reason |
|---|---|---|
| `library/include/borealis/core/application.hpp`, `library/lib/core/application.cpp` | On `__SWITCH__`: `Application::setRotation(quarterTurns)` / `getRotation()` and a `rotation` field. `frame()` translates + rotates the NanoVG transform before the UI scale; `setWindowSize` lays out a portrait content area (720 units wide, so text keeps its size) for 90°/270°; `processInput` maps raw touch points into the rotated space and rotates the D-pad / nav buttons and stick axes so "up" stays up on screen | Option `ui_rotation` (the whole interface rotated, for holding the console sideways). borealis has no rotation support of its own |
| `library/include/borealis/extern/nanovg/dk_renderer.hpp`, `library/lib/extern/nanovg/deko3d/dk_renderer.cpp` | On `__SWITCH__`: public `nvg::DkRenderer::GetTextureImage(id)`, the `dk::Image` behind a NanoVG texture | `gpu::FrameUploader` updates the game image with GPU commands on borealis' queue instead of `nvgUpdateImage`, which waits for the queue to go idle on every call; a deko3d renderer will write its output the same way (docs/GPU_RENDERER.md) |

## Build-level differences from upstream CMake

These live in `switch/CMakeLists.txt`, not in the core tree:

- Source list hand-mirrored from `src/CMakeLists.txt` (core target without OpenGL or GDB
  stub; JIT sources from the `ENABLE_JIT` + ARM64 branch when `MELONDS_JIT=ON`, the
  default) plus `src/teakra/src/CMakeLists.txt` (teakra library, no `teakra_c`).
- `version.h` generated with `configure_file` from `src/version.h.in` (version `1.1`, suffix
  `-switch`). Bump `MELONDS_VERSION` when upstream's `project(... VERSION)` changes.
- `src/frontend/ScreenLayout.cpp` (desktop melonDS's screen layout math; frontend-side but
  self-contained) is compiled into the app unchanged, and `src/frontend/mic_blow.h` (the
  mic noise sample) is included from there.
- rcheevos is built from a glob of its `src/` minus `rc_client_raintegration.c`,
  `rc_client_external.c` and `rc_libretro.c`, with `RC_CLIENT_SUPPORTS_HASH`.
- devkitPro's arch flags (`-march=... -mtp=soft -ftls-model=local-exec`) re-added after
  borealis' toolchain overwrites `CMAKE_C_FLAGS`.

Verified on hardware 2026-10-26: JIT ~85% speed, JIT + fastmem ~100% on the first test game.

## Reference

[umby24/melonDS](https://github.com/umby24/melonDS) (pre-instance-API melonDS) has a
JIT + fastmem that ran on Switch; its `src/ARMJIT_Memory.cpp` and
`src/ARMJIT_A64/ARMJIT_Compiler.cpp` `__SWITCH__` paths are what the patches above restore.
