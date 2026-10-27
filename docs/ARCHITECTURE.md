# Architecture

```
            +----------------- melonds-next.nro --------------------+
            |   borealis (window, deko3d/NanoVG, main loop, UI input) |
            |                                                        |
 SD card -->| ui/rom_list --path--> ui/game: GameActivity/GameView    |
            |   ui/settings_list      | run_frame, frame()  ^ overlay |
            |                         v                   ui/game:    |
            |                     frontend/emu_host       Overlay-    |
            |                                             Activity    |
            |          owns melonDS::NDS, saves, layout, options     |
            |               |  NDS::RunFrame etc.   ^ Platform::     |
            |               v                       |  callbacks     |
            |      libmelonds.a  (mds_upstream/src + teakra)         |
            |                                       |                |
            |                     frontend/melon_platform (files,    |
            |                     threads, sync, time, stubs)        |
            |                                                        |
            |  emu_host -> platform/audio  platform/input  platform/system
            +-----------------------|-------------|---------|------------+
                                    v             v         v
                                 audren          hid    svc (core pinning)
```

## Data formats at the core boundary

- **Video:** melonDS's software renderer gives two 256x192 framebuffers
  (`GPU::GetFramebuffers`), 32-bit `0xAARRGGBB` (bytes B,G,R,A). `emu_host` converts both
  to RGBA8 into one 256x386 frame (top screen, two padding rows, bottom screen; see
  `host::acquire_frame`). `ui::GameView` uploads it into one NanoVG image and lays the
  screens out with upstream melonDS's `ScreenLayout` (`mds_upstream/src/frontend/`;
  options `screen_layout`, `screen_sizing` incl. "auto" from `host::auto_emphasis()`,
  `swap_screens`, `scaling`), drawing each screen through its 2x3 transform. It hands the
  layout plus a raw-touch → layout map (undoing borealis' rotation and scale) to
  `host::set_screen_layout` whenever they change.
- **Audio:** the host sets `NDSArgs::OutputSampleRate = 48000` (`host::kAudioRate`), so
  melonDS resamples internally and audren plays it unconverted. After each `RunFrame` the
  host drains `SPU::ReadOutput` (interleaved stereo `s16`) into `audio::push`.
- **Input:** `input::ds_keys()` returns DS buttons in KEYINPUT bit order (+X/Y at bits
  10-11); the host passes the inverted mask to `NDS::SetKeyMask`. Bindings and hotkeys are
  free-form options (`bind_*`: any of the buttons; `hotkey_*`: all of them, e.g.
  `Plus+Minus`) that the host pushes into `input::set_binding` / `set_hotkey`; the D-pad
  and sticks follow `ui_rotation`. `input::touch()` returns raw touchscreen pixels; the
  host maps them through the UI's touch map and `ScreenLayout::GetTouchCoords` and calls
  `NDS::TouchScreen` / `ReleaseScreen`. Without a real touch, the right-stick cursor
  (`touch_cursor`, `touch_click`, `hotkey_touch`) touches instead; `host::cursor()` tells
  the UI where to draw it. The mic hotkey makes `Platform::Mic_ReadInput` play
  desktop melonDS's breath sample. borealis' own input is
  blocked (`blockInputs`) while a game runs and unblocked while the overlay is open; our
  `input` module reads hid independently of borealis (borealis does `padConfigureInput`).
- **ROMs:** read fully into memory and handed to `NDSCart::ParseROM`.
- **Saves:** `<rom>.sav` next to the ROM (desktop melonDS convention) or in `saves/`:
  whichever exists, preferring option `save_location` (default: next to the ROM), which
  also decides where a new save goes; cheat files (`.mch`, `cheats/`) resolve the same
  way. Loaded into `NDSCartArgs::SRAM`. The core reports
  every write via `Platform::WriteNDSSave` (whole buffer); the host mirrors it and writes
  the file (tmp + rename) 60 frames after the last write, and on unload.
- **Clock:** the DS RTC is set from the Switch's local time (`sys::local_time`: system
  clock + time zone; libnx's `time()` alone is UTC) at boot, whenever the emu thread
  resumes (overlay, save-state load) and every ~60 s of play. In-game clock changes are
  therefore not persistent (`Platform::WriteDateTime` is ignored).
- **BIOS/firmware:** `system/bios9.bin`, `bios7.bin`, `firmware.bin` when present and the
  right size, else FreeBIOS + generated firmware. Without real dumps (or with the
  `direct_boot` option on, the default) the game is direct-booted. DSi mode
  (`console_type`, read at load) builds a `melonDS::DSi` from `DSiArgs`: `bios9i.bin` /
  `bios7i.bin` (else FreeBIOS), `firmware_dsi.bin` (else generated), `nand.bin` opened
  read-write with its touch calibration reset as desktop melonDS does (absent: direct boot
  only), DSP HLE, no SD card.
- **RetroAchievements:** `frontend/achievements` wraps rcheevos' `rc_client`. Server calls
  go to `platform/http` (curl, one worker thread, in order). Games are identified by
  hashing the ROM bytes *before* `NDSCart::ParseROM` (melonDS re-encrypts the secure area
  of its copy, which changes the hash); `rc_client_do_frame` runs on
  the emu thread after each frame and `rc_client_idle` once a second while parked. Memory
  reads (main RAM at 0, DSi RAM up to 16 MB, DTCM at 0x1000000) are guarded by a mutex
  because rc_client also validates addresses from the network thread. The token lives in
  `<base>/retroachievements.txt`; progress is saved next to a state as `<state>.ra`.
  Hardcore (`ra_hardcore`, applied at load) stops cheats being applied and state loads.

## Module contracts

Headers under `switch/source/` are the contract. Summary:

| Header | Owner | Responsibility |
|---|---|---|
| `platform/audio.h` | platform | Accept stereo s16 batches, play them without blocking the emu thread |
| `platform/input.h` | platform | Poll hid each frame; report DS buttons, raw touch point, right stick, hotkeys; button-set parsing; raw pad reads for bind dialogs |
| `platform/http.h` | platform | HTTP(S) on curl: an ordered async queue on one worker thread (RetroAchievements), plus blocking `get` for bulk downloads |
| `platform/keyboard.h` | platform | System software keyboard (text and password) |
| `platform/system.h` | platform | Misc libnx services for the frontend (pin a thread to a CPU core, local time, CPU clock, memory usage) |
| `frontend/emu_host.h` | frontend | Own the NDS instance, paths, saves/states, layout and the emulation thread; pause/resume, stats, composed frames |
| `frontend/achievements.h` | frontend | RetroAchievements (rc_client): login, game load, per-frame processing, notifications, list, hardcore, state progress |
| `frontend/rom_info.h` | frontend | Title, publisher and 32x32 icon from a ROM's header + banner (no full read), cached |
| `frontend/thumbnails.h` | frontend | Cover art downloads from GameTDB by game code (one job thread, progress), and where the art lives |
| `frontend/cheats.h` | frontend | Per-game `.mch` cheat file, usrcheat.dat import (entries matched by game code; the header-checksum match is taken automatically, otherwise the player picks the version), pushes enabled codes into `NDS::AREngine` via `host::with_console` |
| `frontend/melon_platform.h` | frontend | Config for our `melonDS::Platform::` implementation (local-file dir) |
| `frontend/options.h` | frontend | Host-declared key/value options (enumerated, or free-form text like `rom_dir`) with defaults, persisted to `config.ini` |
| `ui/main_activity.h` | ui | Root activity: tab frame with Games, Settings, Controls, RetroAchievements, Downloader |
| `ui/downloader_view.h` | ui | Downloader tab: art type / card picture options, start / stop, live progress |
| `ui/rom_list.h` | ui | ROM/folder card grid for a directory (one custom view: draws visible cards, own D-pad/touch/scroll); folders push a new activity |
| `ui/dir_list.h` | ui | SD directory listing + path helpers shared by the ROM list and folder picker |
| `ui/folder_picker.h` | ui | File-browser folder picker (A open, B up, X pick); used for the ROM folder |
| `ui/settings_list.h` | ui | SelectorCells for enumerated options (Settings tab and overlay share them); the Settings tab adds the ROM folder picker |
| `ui/cheats_view.h` | ui | Cheat list (categories, on/off toggles, import) opened from the overlay |
| `ui/input_settings.h` | ui | Controls: cursor + fast-forward options, hotkey and DS button bindings (press-to-bind) |
| `ui/achievements_view.h` | ui | RetroAchievements tab (login via the system keyboard, hardcore) and the game's achievement list |
| `ui/game.h` | ui | `launch_game`: game activity + GameView (runs emulation) + in-game overlay |

`frontend/` never includes `<switch.h>`; it only talks to `platform/` through these headers.
That keeps the host logic testable on a PC build later if we want one.

`melonDS::Platform::` (declared in `mds_upstream/src/Platform.h`) is the core's half of the
contract. Its definitions are split: stateless services in `frontend/melon_platform.cpp`;
`Log`, `SignalStop`, `WriteNDSSave`, `WriteFirmware`, `WriteDateTime`, `Mic_*` in
`frontend/emu_host.cpp`. Local wifi, networking, camera, AAC (DSi), addons and dynamic
libraries are stubs.

## UI flow

`main.cpp` initializes borealis (window, dark theme, Plus doesn't auto-quit), the host
and input, pushes the main activity and runs `brls::Application::mainLoop()`.

- **Games tab** lists `options::get("rom_dir")` (rebuilt whenever the tab is selected, so
  a folder chosen in **Settings → ROM folder** shows up directly) → pick a ROM → `ui::launch_game`: opens audio, `host::load_game`, pushes a
  `GameActivity` (or shows a dialog with `host::last_error()`).
- **GameView::draw** never runs emulation: it draws the newest frame from
  `host::acquire_frame()`, exits when `host::stopped()`, reacts to
  `host::consume_events()` (menu hotkey → overlay; emphasis hotkey → flips
  `screen_sizing`), and draws the touch cursor, a fast-forward badge and the FPS readout
  (option `show_fps`).
- **OverlayActivity** calls `host::pause()` when shown and `host::resume()` after it
  closes. It is translucent, so the paused game stays visible behind a dimmed
  backdrop and a right-hand panel: resume, save/load state (slots 1-4), display options,
  controls, emulation options, cheats, achievements (when the game is known to
  RetroAchievements), quit. New sections go in `OverlayActivity::createContentView`. B or
  Plus resumes.
- **Bind dialogs** (`BindActivity`) block borealis' input and poll
  `input::ui_held_buttons()` themselves, so any button (A and B included) can be bound.
- **Rotation:** `ui_rotation` calls the borealis patch `Application::setRotation`
  (startup and on change, deferred with `brls::sync`).
- **Cheats** (overlay → Manage cheats) pushes `CheatsActivity` on top of the overlay; the
  emu thread stays parked, so `cheats::set_enabled` applies codes through
  `host::with_console` without racing the core. Files: `<base>/cheats/<rom>.mch`
  (desktop melonDS format); database: `system/usrcheat.dat`.
- Teardown (`host::unload_game`, `audio::shutdown`, popping the activity) is deferred with
  `brls::sync` so a view is never destroyed inside its own `draw`.

## borealis limits

- **Long lists must use `brls::RecyclerFrame`.** A `Box` of cells draws every cell each
  frame (borealis only culls leaf views, and cells are boxes); a few thousand cells
  overflow deko3d NanoVG's per-frame command buffer (`dk::CmdBuf::requestCmdMem` abort).
  The cheat list (2000+ codes for some games) is a recycler; the ROM grid is a single
  custom view that draws only visible cards. The folder picker is still a plain box:
  fine for hundreds of entries, not thousands.
- **Never delete the focused view without giving focus elsewhere first** (e.g.
  `clearViews()` under the focused cell): `ScrollingFrame` dereferences the stale focus
  on the next frame. Rebuild in a `brls::sync` and `giveFocus` a new view, or reopen the
  activity.

## Threading

- Core 0: main thread — borealis UI, NanoVG/deko3d rendering, texture upload.
- Core 1: emulation thread (`emu_host.cpp`, pthread with a 1 MB stack): input poll,
  `NDS::RunFrame` (JIT or interpreter), RetroAchievements, audio drain, frame conversion,
  save flushing. Paced by `sleep_until` to the DS rate (560190 / 33513982 s ≈ 16.715 ms),
  or N times that while fast-forwarding (unpaced for "unlimited"; audio beyond 60 ms
  queued is dropped); if it falls more than 3 frames behind it resets the deadline.
- Any core: the HTTP worker thread (`platform/http`, RetroAchievements requests), the
  cover-art download thread (`frontend/thumbnails`, its own blocking requests) and the ROM
  grid's banner/art loader (one per grid, joined when the grid goes away).
- Core 2: melonDS's soft 3D render thread (option `threaded_3d`, default on; created via
  `Platform::Thread_Create`, pinned with `sys::set_current_thread_core`) and the audio
  thread (priority 0x2B, above the render thread's 0x2C), fed by a lock-free SPSC ring.
- Core 3: reserved by the OS.

Never `std::thread::detach()`: libnx doesn't implement thread detaching, so it throws and
the app exits through `std::terminate` (silently, with no crash report; `main.cpp` logs
uncaught exceptions). Own every thread and join it.

Hand-offs between the UI and emu threads (all in `emu_host.cpp`):

- **Frames:** triple buffer (`back` written by emu, `ready` swapped under `g_frame_mx`,
  `front` read by the UI), so neither side waits for the other. The UI redraws the
  previous frame when nothing new arrived (~1 repeat every 6 s at 60 Hz vs 59.83 Hz).
- **Control:** `g_mx` + `g_cv`. `pause()` sets `g_pause_req` and waits until the thread is
  parked at the top of its loop; only then may the UI touch `g_nds` (save/load state).
  The thread parks itself on the menu hotkey or when the console stops.
- **Options:** the emu thread reads options under `g_mx` at the start of each frame (and
  applies dirty ones there); the UI writes them only via `host::set_option`, which takes
  `g_mx`. UI-thread reads (scaling, filter, show_fps) need no lock: the UI is the only
  writer.
- **Touch:** the screen layout + touch map are set by the UI under `g_mx` (only when they
  change; versioned) and copied by the emu thread before the next frame. The cursor is
  published back under `g_cursor_mx`.
- **Events:** hotkey events and auto-emphasis are atomics read by the UI.

The JIT's fastmem faults happen on the emu thread; `NDS::Current` (thread-local, set by
`RunFrame`) is what `__libnx_exception_handler` uses to find the console.
