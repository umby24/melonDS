# melonds-next — project guide for Claude

Nintendo Switch homebrew (libnx `.nro`) Nintendo DS emulator. The emulation engine is
upstream desktop **melonDS** (`mds_upstream/`), whose core library (`src/`, minus the Qt
frontend) is statically linked into a small purpose-built frontend. The UI is
**borealis** (`borealis/`, XITRIX fork: Switch-style NanoVG UI on deko3d). We do **not**
depend on RetroArch or libretro.

Read `docs/PLAN.md` (roadmap + status) and `docs/ARCHITECTURE.md` (module contracts) before
making changes. Update the status checkboxes in `docs/PLAN.md` when a milestone lands.

## Layout

```
mds_upstream/         upstream melonDS (git submodule, melonDS-emu/melonDS; patched from patches/)
  src/                core sources; public API is NDS.h, Args.h, Platform.h
  src/frontend/       melonDS's Qt frontend -- NOT built, reference only
borealis/             UI library (git submodule, XITRIX/borealis; patched from patches/), built via its CMake
rcheevos/             RetroAchievements client library (git submodule, RetroAchievements/rcheevos; unpatched)
patches/              our changes to mds_upstream/ and borealis/ (applied by scripts/apply_patches.sh)
switch/               the Switch app
  CMakeLists.txt      builds melonds (static lib; source list mirrors src/CMakeLists.txt,
                      re-sync it when rebasing upstream), borealis, the app, the .nro + RomFS
  resources/          our RomFS files (borealis' i18n/icons/shaders are added at configure)
  source/main.cpp     app lifecycle; borealis owns the window and main loop
  source/frontend/    melonDS host + our Platform:: implementation, options (no libnx here)
  source/platform/    libnx audio / input / system / http (curl) / keyboard (libnx only here)
  source/ui/          borealis activities: ROM list, settings, game view, in-game overlay
scripts/              build helpers (docker wrapper, patch application)
docs/                 PLAN.md, ARCHITECTURE.md, CORE_PATCHES.md
```

## Build

No local devkitPro install; builds run in the official image:

```
scripts/build.sh            # -> switch/melonds-next.nro
scripts/build.sh clean
```

`scripts/build.sh` runs CMake (`-S switch -B switch/build`) inside `devkitpro/devkita64`. Test on hardware via
nxlink (`nxlink -s switch/melonds-next.nro`) or Ryujinx.

## Rules

- **Core edits are patches.** Keep changes inside `mds_upstream/` and `borealis/` minimal, guarded by
  `#ifdef __SWITCH__` where possible, and log every one in `docs/CORE_PATCHES.md`
  (file, reason). After editing a submodule, regenerate its file in `patches/`
  (`git -C <submodule> diff > patches/<name>.patch`); the submodule's own working tree is
  never committed. This keeps rebasing on upstream feasible.
- Never `std::thread::detach()` (libnx doesn't support it; it throws). Own and join threads.
- `source/frontend/` must not include libnx headers; `source/platform/` is the only libnx
  layer we write (borealis itself uses libnx internally; `ui/` only uses borealis). Module boundaries are the headers documented in `docs/ARCHITECTURE.md` — change
  a header only together with that doc.
- melonDS's AArch64 JIT is built (`MELONDS_JIT=ON` → `JIT_ENABLED`); its `__SWITCH__` paths
  needed patches (see `docs/CORE_PATCHES.md`). Options `jit` and `fastmem` (both default
  on; fastmem verified on hardware 2026-10-26) are read when a game loads. Fastmem faults are handled by
  our `__libnx_exception_handler` in `ARMJIT_Memory.cpp`; don't define another one.
- The core's only external interface is `melonDS::Platform::` (`src/Platform.h`): every
  function declared there must be defined by us (`frontend/melon_platform.cpp` for
  stateless services, `frontend/emu_host.cpp` for callbacks that touch host state).
- C++17 (core requirement), `-O3 -fwrapv`, `-march=armv8-a+crc+crypto -mtune=cortex-a57`.
- Runtime paths on SD: `sdmc:/switch/melonds-next/{system,saves,states,cheats,config.ini}`; ROMs default
  to `sdmc:/roms/nds` (option `rom_dir`, set from Settings → ROM folder). Optional dumps in `system/`: `bios9.bin`, `bios7.bin`,
  `firmware.bin` (otherwise FreeBIOS + generated firmware, which force direct boot).
