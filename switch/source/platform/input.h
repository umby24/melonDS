#pragma once
// Controller + touchscreen on libnx hid, reported in DS terms. DS buttons and frontend
// hotkeys are bound to sets of Switch buttons (Buttons), configured by the host from
// options. The host maps touch points onto the emulated bottom screen (it owns the
// screen layout).
//
// Threading: poll() and the getters below it belong to the emulation thread, and so do
// the set_*() calls (the host applies options there, or before the thread starts).
// ui_held_buttons() is for the UI thread (it reads its own pad state).

#include <cstdint>
#include <string>

namespace input {

// DS buttons, in KEYINPUT bit order (bits 0-9) plus the ARM7 EXTKEYIN X/Y packed at
// 10-11, i.e. the layout melonDS's NDS::SetKeyMask expects (inverted: 1 = released).
enum DsKey : uint32_t {
    DS_A      = 1u << 0,
    DS_B      = 1u << 1,
    DS_SELECT = 1u << 2,
    DS_START  = 1u << 3,
    DS_RIGHT  = 1u << 4,
    DS_LEFT   = 1u << 5,
    DS_UP     = 1u << 6,
    DS_DOWN   = 1u << 7,
    DS_R      = 1u << 8,
    DS_L      = 1u << 9,
    DS_X      = 1u << 10,
    DS_Y      = 1u << 11,
    DS_ALL    = 0xFFFu,
};

// Frontend-level actions that never reach the emulator.
enum Hotkey : uint32_t {
    HOTKEY_NONE         = 0,
    HOTKEY_MENU         = 1u << 0,  // open the in-game menu
    HOTKEY_FAST_FORWARD = 1u << 1,
    HOTKEY_MIC          = 1u << 2,  // blow into the microphone
    HOTKEY_EMPHASIS     = 1u << 3,  // switch which screen is shown large
    HOTKEY_TOUCH        = 1u << 4,  // touch at the stick-driven cursor
};

// A set of Switch buttons (opaque bitmask). Text form: names joined by '+', e.g.
// "ZL+ZR"; "" means none. Names: A B X Y L R ZL ZR Plus Minus Up Down Left Right
// LStick RStick LeftSL LeftSR RightSL RightSR.
using Buttons = uint64_t;
Buttons parse_buttons(const std::string& text);  // unknown names are ignored
std::string format_buttons(Buttons buttons);     // canonical text form
std::string describe_buttons(Buttons buttons);   // for display: "ZL + ZR", "None"

// A DS key is pressed while any of its buttons is; a hotkey while all of its buttons
// are. Buttons held for an active hotkey don't reach the DS until released.
void set_binding(DsKey key, Buttons any_of);
void set_hotkey(Hotkey hotkey, Buttons all_of);

// Quarter turns clockwise the picture is rotated by (borealis' UI rotation); the D-pad,
// left stick and right stick are rotated to match, so "up" stays up on screen.
void set_rotation(int quarter_turns);

bool init();
void shutdown();

// Call once per frame before running the emulator.
void poll();

// DS buttons currently held (bitmask of DsKey, 1 = pressed). The left stick doubles as
// the D-pad.
uint32_t ds_keys();

// First handheld touchscreen contact in raw touchscreen pixels (always 1280x720,
// unrotated). False when nothing is touching.
bool touch(int& x, int& y);

// Right stick, -1..1 each way, x right and y down on screen (rotation applied).
void right_stick(float& x, float& y);

uint32_t hotkeys_held();     // bitmask of Hotkey
uint32_t hotkeys_pressed();  // newly pressed this frame

// UI thread: Switch buttons held right now (handheld or player 1), sticks excluded.
// For "press a button" binding dialogs.
Buttons ui_held_buttons();

}  // namespace input
