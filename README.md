# Melon-Next Switch

A Nintendo DS / DSi emulator for the Nintendo Switch (homebrew), built on the current
desktop [melonDS](https://melonds.kuribo64.net) core with a Switch-native interface
([borealis](https://github.com/XITRIX/borealis), rendered with deko3d).

The app is called **melonds-next** on the console.

## Features

- **Full speed** on stock hardware for most games, thanks to melonDS's AArch64 JIT with fast memory.
  - A CPU overclock option helps heavy 3D games such as Super Mario 64 DS.
- **Game browser:** a grid of game cards showing each ROM's own icon and title, or cover art you download.
  - It works with any file names.
- **In-game menu:** press Plus + Minus by default to open it. From there you can:
  - save and load states (4 slots)
  - change the display options
  - edit the controls
  - manage cheats
  - view achievements
  - quit the game
- **Screens:**
  - Layouts: stacked, side by side or hybrid.
  - Sizes: equal, emphasize top or bottom, auto (follows the 3D screen), or one screen only.
  - Screens can be swapped, scaled to fit or at integer sizes, with sharp or smooth filtering.
  - A hotkey switches the emphasis while you play.
- **Rotation:** rotate the whole interface for holding the console sideways (book-style games).
- **Controls:**
  - Remap every DS button and hotkey by pressing the buttons you want.
  - Touchscreen works in handheld mode.
  - In docked mode, a right-stick touch cursor replaces the touchscreen.
  - Fast forward (hold or toggle, 2× / 3× / 4× / unlimited) and a "blow into the mic" hotkey.
- **Cheats:** Action Replay codes in desktop melonDS's `.mch` format, plus import from an R4 `usrcheat.dat`.
- **RetroAchievements:**
  - Log in, then get unlock and leaderboard notifications and a per-game achievement list.
  - Achievement progress is kept in save states.
  - Hardcore mode is supported.
- **Saves:** battery saves next to your ROMs (desktop melonDS's format) or in the app folder.
  - The DS clock follows the Switch's clock.
- **DSi mode (experimental):** uses your DSi BIOS and NAND dumps when you have them.

## Installing

1. Copy `melonds-next.nro` to `sdmc:/switch/`.
2. Put your ROMs (`.nds`) in `sdmc:/roms/nds/`, or pick another folder in **Settings → ROM folder**.
3. Launch it from the Homebrew Menu.
   - Prefer **title-override mode**: hold R while starting a game, then open the Homebrew Menu.
   - ROMs are loaded fully into memory, and applet mode (album) has much less of it.

Everything the app creates lives in `sdmc:/switch/melonds-next/`:

```
sdmc:/switch/melonds-next/
  config.ini                settings
  retroachievements.txt     RetroAchievements login token (created when you log in)
  system/                   optional dumps, see below
  saves/                    battery saves (if Settings → "Where new save and cheat files go" says so)
  states/                   save states (<rom>.ml1-4, plus .ra achievement progress)
  cheats/                   cheat files (<rom>.mch) if not next to the ROM
  thumbnails/               downloaded cover art (<type>/<GAMECODE>.png)
  log.txt                   log of the last run (when not launched via nxlink)
```

### Optional files in `system/`

None are required. melonDS's FreeBIOS and generated firmware are used otherwise, and games are booted directly.

| File | Used for |
|---|---|
| `bios9.bin`, `bios7.bin`, `firmware.bin` | DS BIOS / firmware dumps (needed to boot through the DS menu) |
| `bios9i.bin`, `bios7i.bin`, `firmware_dsi.bin`, `nand.bin` | DSi mode (Settings → Console). Without `nand.bin`, DSi games are direct-booted |
| `usrcheat.dat` | Cheat database for "Import cheats" |

No BIOS, firmware or game files are included or provided. Dump your own.

## Controls

Defaults; all can be changed in the **Controls** tab or from the in-game menu.

| | |
|---|---|
| DS buttons | Same positions as on the Switch (A, B, X, Y, L, R, Start = Plus, Select = Minus) |
| D-pad | D-pad or left stick |
| Touch screen | Handheld touchscreen, or the right-stick cursor + **ZR** to touch |
| In-game menu | **Plus + Minus** |
| Fast forward | **ZL** |
| Switch screen emphasis | **Right stick click** |
| Blow into the microphone | **Left stick click** |

## RetroAchievements

1. Open the **RetroAchievements** tab and choose **Log in**.
2. Enter your username and password with the system keyboard.
   - Only the login token the server returns is stored, in `retroachievements.txt`. Your password is never stored.
3. Games are recognised by their contents, not their file names.

Hardcore mode applies from the next game you start. While it's on, cheats and loading save states are disabled.

## Cover art

The **Downloader** tab fetches box art or front covers for every ROM in your ROM folder
from [GameTDB](https://www.gametdb.com). Art is matched by the game code inside each ROM.
Its **Game card picture** option chooses between downloaded art (the default, with the ROM icon as fallback) and ROM icons.

## Building

The build runs in Docker using the official devkitPro image, so you don't need a local devkitPro install.

```sh
git clone --recursive <this repo>
cd <repo>
scripts/build.sh          # -> switch/melonds-next.nro
scripts/build.sh clean
```

How the build works:
- The core, the UI library and the RetroAchievements client are git submodules, pinned to upstream commits:
  - `mds_upstream/`: melonDS
  - `borealis/`: borealis
  - `rcheevos/`: rcheevos
- `scripts/build.sh` first runs `scripts/apply_patches.sh`, which applies our changes to them from `patches/`. Every change is explained in [docs/CORE_PATCHES.md](docs/CORE_PATCHES.md).
- If you cloned without `--recursive`, run `git submodule update --init`. borealis' own submodules (SDL, GLFW) are not needed for the Switch build.

To try a build quickly, open the Homebrew Menu, press Y (NetLoader), then run
`nxlink -s switch/melonds-next.nro`. The app's log is streamed back to your terminal.

Developer docs:
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): module contracts, threading, data formats.
- [docs/PLAN.md](docs/PLAN.md): roadmap and status.
- [docs/CORE_PATCHES.md](docs/CORE_PATCHES.md): every change to the submodules.
- [docs/GPU_RENDERER.md](docs/GPU_RENDERER.md): plan for a deko3d (GPU) renderer.

## Credits

- [melonDS](https://github.com/melonDS-emu/melonDS) by Arisotura and the melonDS team, the emulation core. The microphone breath sample comes from melonDS too.
- [umby24/melonDS](https://github.com/umby24/melonDS): the earlier Switch port, whose JIT and fast-memory code paths were restored here.
- [borealis](https://github.com/XITRIX/borealis) (XITRIX fork of natinusala/borealis), the UI.
- [rcheevos](https://github.com/RetroAchievements/rcheevos), the RetroAchievements client.
- [GameTDB](https://www.gametdb.com), the cover art.
- [devkitPro](https://devkitpro.org) / libnx, the toolchain.
- The app icon is melonDS's two-screen icon (as used by the earlier Switch port), recoloured.

## License

GPL-3.0-or-later (see [LICENSE](LICENSE)), as required by melonDS. The bundled
dependencies keep their own licenses:
- borealis: Apache-2.0.
- rcheevos: MIT.
- NanoVG, Yoga, fmt, tinyxml2 and others inside borealis: see their own files.

This project is not affiliated with Nintendo. Use it only with games and system files you own.
