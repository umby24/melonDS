#pragma once
// Host for the statically linked melonDS core. Owns the melonDS::NDS instance, BIOS /
// firmware / save files, screen layout, and the emulation thread, which runs frames
// paced to the DS refresh rate (input -> NDS -> audio + composed frame). The UI thread
// draws the composed frames and controls the thread (pause/resume) through this API.
//
// Threading: every function here is for the UI (main) thread. The emu thread is
// created by load_game and joined by unload_game.

#include <cstdint>
#include <functional>
#include <string>

class ScreenLayout;  // mds_upstream/src/frontend/ScreenLayout.h

namespace melonDS {
class NDS;
}

namespace host {

// Sample rate the host asks melonDS to output; open audio:: at this rate.
constexpr unsigned kAudioRate = 48000;

struct Paths {
    std::string base;    // sdmc:/switch/melonds-next
    std::string system;  // base/system   (optional bios7.bin, bios9.bin, firmware.bin; DSi:
                         //                bios7i.bin, bios9i.bin, firmware_dsi.bin, nand.bin)
    std::string saves;   // base/saves    (<rom>.sav, raw save memory)
    std::string states;  // base/states   (<rom>.ml<slot>, melonDS savestates)
    std::string cheats;  // base/cheats   (<rom>.mch, melonDS cheat files)
    std::string config;  // base/config.ini
};

// Creates directories, declares options, loads config.
bool init(const Paths& paths);
void shutdown();  // unloads any game, saves config

bool load_game(const std::string& rom_path);  // false on failure; see last_error()
void unload_game();                           // writes the battery save, destroys the NDS
bool game_loaded();

// Parks the emulation thread and waits until it has stopped touching the console
// (emulation paused, audio underruns to silence). No-op without a game.
void pause();
void resume();

// True once the emulated console has stopped itself (e.g. powered off).
bool stopped();

// Hotkey events for the UI, collected since the last call.
enum Event : uint32_t {
    EVENT_MENU     = 1u << 0,  // menu hotkey: the emu thread has already paused itself; the
                               // UI should open the overlay and resume() later
    EVENT_EMPHASIS = 1u << 1,  // emphasis hotkey: the UI should switch screen_sizing
};
uint32_t consume_events();

// Which screen the game currently shows its main (3D) picture on, for screen_sizing
// "auto": 1 = top (ScreenSizing EmphTop), 2 = bottom (EmphBot), 0 = both (Even).
int auto_emphasis();

bool fast_forwarding();

// The touch cursor driven by the right stick, in DS bottom-screen pixels (0-255,
// 0-191). Hidden while the cursor is off, unused, or the real touchscreen is in use.
struct Cursor {
    bool visible;
    float x, y;
    bool pressed;
};
Cursor cursor();

struct Stats {
    float fps;       // emulated frames per second (DS native: ~59.83)
    float frame_ms;  // average emulation time per frame, excluding pacing sleep
    float audio_pct;     // audio produced vs real time (100 = exactly kAudioRate Hz)
    float audio_ms;      // audio queued for output (latency)
    unsigned underruns;  // audio gaps / skips since the game started
    unsigned overflows;
};
Stats stats();

// Latest frame: RGBA8 (bytes R,G,B,A), kFrameWidth x kFrameHeight, tightly packed. The
// top screen is rows 0-191 and the bottom screen starts at kBottomScreenRow; the two
// rows between repeat the neighbouring edge rows so linear filtering doesn't bleed one
// screen into the other. `fresh` is true if it changed since the previous call. nullptr
// before the first frame. Valid until the next call.
constexpr unsigned kFrameWidth = 256;
constexpr unsigned kFrameHeight = 386;
constexpr unsigned kBottomScreenRow = 194;
const uint32_t* acquire_frame(bool& fresh);

// The layout the UI draws the screens with, and the affine map (2x3, melonDS M23 order:
// x' = m0*x + m2*y + m4, y' = m1*x + m3*y + m5) from raw touchscreen pixels (1280x720)
// into that layout's coordinates, so touches land on the DS bottom screen. Cheap to
// call every frame; the emu thread picks up changes.
void set_screen_layout(const ScreenLayout& layout, const float touch_to_layout[6]);

// Changes an option (safe while the emu thread runs; it applies the change before its
// next frame) and writes config.ini. False if the key or value isn't allowed.
bool set_option(const std::string& key, const std::string& value);

// Writes saves/<rom>.sav if the game changed its save memory since the last write.
// The emu thread already does this shortly after the game stops writing. Only call
// while paused (unload_game does).
bool flush_save();

// Runs `fn` on the console with the emu thread parked (restoring its previous state
// afterwards). False without a game. For frontend modules that need the core directly.
bool with_console(const std::function<void(melonDS::NDS&)>& fn);

// Pause the emu thread themselves for the duration (and restore its previous state).
// Loading is refused in RetroAchievements hardcore mode.
bool save_state(int slot);
bool load_state(int slot);

// Last error from the core or host, for showing in UI on failure.
const std::string& last_error();

}  // namespace host
