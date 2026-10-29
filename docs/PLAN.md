# melonds-next — Plan

Goal: a standalone Switch homebrew NDS emulator (`melonds-next.nro`) built on the current
desktop melonDS core, with a native frontend tuned for the Switch (docked + handheld,
touchscreen as the DS touch screen, Joy-Con mappings).

## Why melonDS, and why a custom frontend

_2026-10-26: switched the engine from the libretro desmume2015 core to upstream melonDS
(`mds_upstream/`). The Phase 1 frontend code was ported; the libretro host was replaced._

melonDS is actively maintained, more accurate than the 2015 DeSmuME snapshot, and ships an
AArch64 JIT (with historical Switch support) — the only realistic route to full speed on
the Switch's A57s. Its core is a plain C++ library: construct `melonDS::NDS` from
`NDSArgs`, call `RunFrame()`, read framebuffers / SPU output, and implement the
`melonDS::Platform::` functions it calls back into. That seam is as clean as libretro's
and lets us control the Switch UX completely (screen layouts, touch mapping, menus). Cost:
we own everything RetroArch would have given us (menus, save states UI, shaders).

## Known constraints and risks

| Risk | Impact | Mitigation |
|---|---|---|
| melonDS's Switch JIT paths had bit-rotted | JIT crashes on device | Patched (see `docs/CORE_PATCHES.md`); `jit`/`fastmem` options allow falling back to the interpreter |
| Whole ROM is loaded into RAM | Large ROMs (256-512 MB) fail in applet mode | Recommend title-override launch; later: stream ROM reads from SD |
| FreeBIOS + generated firmware | Rare compatibility issues; no firmware menu boot | Real dumps from `system/` are used when present |
| Upstream moves fast (APIs in `NDS.h`/`Args.h`/`GPU.h` change) | Rebase breaks the host | Submodules pinned to a base commit; our changes in `patches/` (see `docs/CORE_PATCHES.md`); `switch/CMakeLists.txt` source list re-synced per rebase |

## Phases

### Phase 0 — Scaffolding
- [x] Project docs: `CLAUDE.md`, `docs/PLAN.md`, `docs/ARCHITECTURE.md`
- [x] Module contracts (headers) for frontend / platform / ui
- [x] Docker build wrapper (`scripts/build.sh`) using `devkitpro/devkita64`
- [x] `switch/CMakeLists.txt`: melonDS core, borealis, rcheevos and the app, packaged as a `.nro` with NACP + RomFS
- [x] Public repo layout: dependencies as pinned submodules, our changes as `patches/` applied by the build (2026-10-27)

### Phase 1 — First boot (MVP)
_Code for every item below exists and builds against melonDS (2026-10-26); tick items only once verified on a Switch or Ryujinx._
- [x] melonDS host: NDS lifecycle, `Platform::` implementation, options store, system/save dirs, logging
- [x] Video: two ARGB8888 screens → composed layout → NanoVG image in a borealis view, aspect-correct scaling, centered
- [x] Audio: melonDS resampled to 48 kHz → ring buffer → audren (2026-10-26, sounds right; jitter not stress-tested)
- [x] Input: Joy-Con/Pro Controller → DS buttons; handheld touchscreen → DS touch (verified with the console-UI build; re-check under borealis)
- [x] borealis UI: Games tab (ROM list for `sdmc:/roms/nds`), Settings tab (2026-10-26)
- [x] A commercial ROM boots to gameplay on hardware (2026-10-26, console-UI build)

### Phase 2 — Usable
- [ ] Battery saves (`.sav`) next to the ROM or in `saves/` (option `save_location`; existing files found in either), written 1 s after the last write + on exit (code exists; next-to-ROM lookup untested)
- [x] Save states: 4 slots in the overlay (2026-10-26)
- [x] In-game overlay (Plus + Minus): opens, options apply, save/load state, quit returns to a working menu (2026-10-26)
- [ ] Cheats in the overlay: per-game `.mch` files (desktop melonDS format), usrcheat.dat import, category-aware toggles. Works for small lists (SM64DS, 2026-10-26); recycler-based list for 2000+ codes (Pokémon) untested
- [ ] Screen layouts: stacked, side-by-side, hybrid + swap; sizes equal / emphasize top / emphasize bottom / auto / one screen only, via upstream's `ScreenLayout` (code exists, untested); emphasis hotkey (default R-stick click)
- [ ] Config persistence (`config.ini`) for host options
- [ ] Docked mode: touch cursor on the right stick (mouse or point-from-centre mode, hold/toggle touch button, default ZR; code exists, untested). Gyro cursor not done
- [ ] Load external BIOS/firmware from `system/` (code exists; falls back to FreeBIOS + generated firmware)
- [ ] Persist firmware settings changes (`Platform::WriteFirmware` is ignored for now)
- [ ] Set the generated firmware's language/nickname from the Switch system settings (currently melonDS defaults)
- [ ] DS clock synced to the Switch's local time at boot, on resume and every minute (code exists, untested)
- [ ] Sleep/lid: close the DS lid on hotkey or when the Switch goes to sleep

### Phase 3 — Polish
- [x] GPU-side frame upload: `gpu::FrameUploader` queues the copy into the game image on borealis' deko3d queue instead of `nvgUpdateImage` (which waits for the GPU to go idle every frame) (2026-10-29)
- [ ] deko3d renderer, staged (see `docs/GPU_RENDERER.md`): R1 compute 3D on the GPU with the software 2D engines (option `renderer = gpu`; works on hardware 2026-10-29 in SM64DS and Platinum, as fast as software with `gpu_3d_delay` on; GPU frame cost ~4-9 ms still to profile); R2 2D + compositing on the GPU; R3 upscaling
- [ ] ROM folder setting with a folder picker (main menu Settings; code exists, untested)
- [ ] Downloader tab: cover art (box or front cover) from GameTDB by game code into `thumbnails/<type>/<CODE>.png`, shown on the cards (option `card_image`) (code exists, untested)
- [ ] Graphical ROM browser: card grid with icon + title + publisher from each ROM's banner, read in the background and cached (code exists, untested)
- [ ] Remappable controls: DS buttons and hotkeys (menu, fast forward, emphasis, mic, touch) bound by pressing buttons, Controls tab + overlay (code exists, untested)
- [ ] Microphone blow via hotkey (desktop melonDS's breath sample; default L-stick click; code exists, untested)
- [ ] DSi mode, option `console_type` (DSi BIOS dumps or FreeBIOS, optional `nand.bin`, DSP HLE; code exists, untested; `Platform::AAC_*` still stubs)
- [ ] Interface rotation (option `ui_rotation`, borealis patch; menus, game and touch rotate; code exists, untested)
- [ ] Fast forward (hold/toggle, 2×/3×/4×/unlimited, audio kept short; code exists, untested)
- [ ] RetroAchievements via rcheevos: login (token stored in `retroachievements.txt`), unlock/leaderboard notifications, achievement list in the overlay, progress saved with states, hardcore mode (no cheats or state loads) (login, notifications verified 2026-10-27; game identification fixed to hash the file bytes, untested). Badges/icons not shown yet
- [ ] Upscaling: not available yet (melonDS only upscales in its OpenGL/compute renderers, which we don't build; stage R3 of the deko3d renderer)
- [ ] Local wireless / Wi-Fi (`Platform::MP_*` / `Net_*` are stubs)

### Phase 4 — Performance
- [x] On-device FPS / frame-time readout (option `show_fps`)
- [ ] CPU clock option (stock / 1224 / 1581 / 1785 MHz via clkrst or pcv; applied while a game runs, re-applied every minute, restored on exit; code exists, untested)
- [ ] GPU clock option `gpu_clock` (stock / 460.8 / 614.4 / 768 MHz, never below the starting clock; same handling as the CPU clock; code exists, untested)
- [x] Threading: emulation on its own thread (core 1), paced to 59.83 Hz, triple-buffered frames to the UI (2026-10-26, 100% speed)
- [x] AArch64 JIT on Switch (2026-10-26: ~85% speed without fastmem on the first test game; option `jit`, default on)
- [x] JIT fastmem (2026-10-26: ~100% speed on the same game; option `fastmem`, now default on)
- [ ] Stream ROM data from SD instead of loading the whole file

## Working method

Each module has a header contract in `docs/ARCHITECTURE.md`. Work on one module never
requires editing another's `.cpp`. Integration happens in `source/main.cpp`.
