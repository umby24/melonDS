// hid -> DS input. First controller / handheld only, plus the touchscreen.

#include "platform/input.h"

#include <switch.h>

#include <algorithm>
#include <tuple>

namespace input {

namespace {

constexpr int32_t kStickDeadzone = 0x4000;  // left stick as D-pad: ~50% deflection
constexpr float kStickMax = 32767.0f;

struct Name {
    const char* text;
    u64 mask;
};
constexpr Name kNames[] = {
    {"A", HidNpadButton_A},           {"B", HidNpadButton_B},
    {"X", HidNpadButton_X},           {"Y", HidNpadButton_Y},
    {"L", HidNpadButton_L},           {"R", HidNpadButton_R},
    {"ZL", HidNpadButton_ZL},         {"ZR", HidNpadButton_ZR},
    {"Plus", HidNpadButton_Plus},     {"Minus", HidNpadButton_Minus},
    {"Up", HidNpadButton_Up},         {"Down", HidNpadButton_Down},
    {"Left", HidNpadButton_Left},     {"Right", HidNpadButton_Right},
    {"LStick", HidNpadButton_StickL}, {"RStick", HidNpadButton_StickR},
    {"LeftSL", HidNpadButton_LeftSL}, {"LeftSR", HidNpadButton_LeftSR},
    {"RightSL", HidNpadButton_RightSL}, {"RightSR", HidNpadButton_RightSR},
};
constexpr u64 kAllNamed = HidNpadButton_A | HidNpadButton_B | HidNpadButton_X | HidNpadButton_Y |
                          HidNpadButton_L | HidNpadButton_R | HidNpadButton_ZL | HidNpadButton_ZR |
                          HidNpadButton_Plus | HidNpadButton_Minus | HidNpadButton_Up |
                          HidNpadButton_Down | HidNpadButton_Left | HidNpadButton_Right |
                          HidNpadButton_StickL | HidNpadButton_StickR | HidNpadButton_LeftSL |
                          HidNpadButton_LeftSR | HidNpadButton_RightSL | HidNpadButton_RightSR;

constexpr int kDsKeys = 12;
constexpr int kHotkeys = 5;

// Defaults mirror the DS layout (Switch and DS share the Nintendo face layout).
u64 g_bindings[kDsKeys] = {
    HidNpadButton_A, HidNpadButton_B, HidNpadButton_Minus, HidNpadButton_Plus,
    HidNpadButton_Right, HidNpadButton_Left, HidNpadButton_Up, HidNpadButton_Down,
    HidNpadButton_R, HidNpadButton_L, HidNpadButton_X, HidNpadButton_Y,
};
u64 g_hotkeys[kHotkeys] = {
    HidNpadButton_Plus | HidNpadButton_Minus, HidNpadButton_ZL, HidNpadButton_StickL,
    HidNpadButton_StickR, HidNpadButton_ZR,
};
bool g_hotkey_latched[kHotkeys] = {};
int g_rotation = 0;

PadState g_pad;
PadState g_ui_pad;
bool g_ready = false;

uint32_t g_keys = 0;
bool g_touch = false;
int g_touch_x = 0;
int g_touch_y = 0;
float g_rs_x = 0.0f, g_rs_y = 0.0f;
uint32_t g_held = 0;
uint32_t g_pressed = 0;

int bit_index(uint32_t single_bit) {
    return __builtin_ctz(single_bit);
}

// Physical D-pad direction -> the direction it points on the rotated screen.
u64 rotate_dpad(u64 b) {
    if (g_rotation == 0)
        return b;
    bool u = b & HidNpadButton_Up, r = b & HidNpadButton_Right;
    bool d = b & HidNpadButton_Down, l = b & HidNpadButton_Left;
    if (g_rotation == 1)
        std::tie(u, r, d, l) = std::make_tuple(r, d, l, u);
    else if (g_rotation == 2)
        std::tie(u, r, d, l) = std::make_tuple(d, l, u, r);
    else
        std::tie(u, r, d, l) = std::make_tuple(l, u, r, d);
    b &= ~u64(HidNpadButton_Up | HidNpadButton_Right | HidNpadButton_Down | HidNpadButton_Left);
    return b | (u ? HidNpadButton_Up : 0) | (r ? HidNpadButton_Right : 0) |
           (d ? HidNpadButton_Down : 0) | (l ? HidNpadButton_Left : 0);
}

// Physical stick vector (y down) -> on-screen vector.
void rotate_vector(float& x, float& y) {
    float px = x, py = y;
    if (g_rotation == 1)      { x = py;  y = -px; }
    else if (g_rotation == 2) { x = -px; y = -py; }
    else if (g_rotation == 3) { x = -py; y = px; }
}

}  // namespace

Buttons parse_buttons(const std::string& text) {
    Buttons out = 0;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('+', start);
        if (end == std::string::npos)
            end = text.size();
        std::string name = text.substr(start, end - start);
        for (const Name& n : kNames)
            if (name == n.text)
                out |= n.mask;
        start = end + 1;
    }
    return out;
}

std::string format_buttons(Buttons buttons) {
    std::string out;
    for (const Name& n : kNames) {
        if (buttons & n.mask) {
            if (!out.empty())
                out += '+';
            out += n.text;
        }
    }
    return out;
}

std::string describe_buttons(Buttons buttons) {
    std::string out;
    for (const Name& n : kNames) {
        if (buttons & n.mask) {
            if (!out.empty())
                out += " + ";
            out += n.text;
        }
    }
    return out.empty() ? "None" : out;
}

void set_binding(DsKey key, Buttons any_of) {
    g_bindings[bit_index(key)] = any_of;
}

void set_hotkey(Hotkey hotkey, Buttons all_of) {
    int i = bit_index(hotkey);
    g_hotkeys[i] = all_of;
    g_hotkey_latched[i] = false;
}

void set_rotation(int quarter_turns) {
    g_rotation = quarter_turns & 3;
}

bool init() {
    if (g_ready)
        return true;
    // borealis already calls padConfigureInput; we just read our own pad states.
    padInitializeDefault(&g_pad);
    padInitializeDefault(&g_ui_pad);
    hidInitializeTouchScreen();
    g_ready = true;
    return true;
}

void shutdown() {
    g_ready = false;
}

void poll() {
    if (!g_ready)
        return;
    padUpdate(&g_pad);

    u64 buttons = padGetButtons(&g_pad) & kAllNamed;

    // Left stick doubles as the D-pad.
    HidAnalogStickState l = padGetStickPos(&g_pad, 0);
    if (l.x < -kStickDeadzone) buttons |= HidNpadButton_Left;
    if (l.x > kStickDeadzone)  buttons |= HidNpadButton_Right;
    if (l.y > kStickDeadzone)  buttons |= HidNpadButton_Up;
    if (l.y < -kStickDeadzone) buttons |= HidNpadButton_Down;
    buttons = rotate_dpad(buttons);

    HidAnalogStickState r = padGetStickPos(&g_pad, 1);
    g_rs_x = std::clamp(r.x / kStickMax, -1.0f, 1.0f);
    g_rs_y = std::clamp(-r.y / kStickMax, -1.0f, 1.0f);
    rotate_vector(g_rs_x, g_rs_y);

    // Hotkeys. A hotkey stays latched (its buttons withheld from the DS) until all of
    // its buttons are released, so e.g. Plus + Minus never leaks START/SELECT.
    uint32_t held = 0;
    u64 withheld = 0;
    for (int i = 0; i < kHotkeys; ++i) {
        u64 mask = g_hotkeys[i];
        bool active = mask && (buttons & mask) == mask;
        g_hotkey_latched[i] = active || (g_hotkey_latched[i] && (buttons & mask));
        if (active)
            held |= 1u << i;
        if (g_hotkey_latched[i])
            withheld |= mask;
    }
    g_pressed = held & ~g_held;
    g_held = held;
    buttons &= ~withheld;

    g_keys = 0;
    for (int i = 0; i < kDsKeys; ++i)
        if (buttons & g_bindings[i])
            g_keys |= 1u << i;

    g_touch = false;
    HidTouchScreenState ts = {};
    if (hidGetTouchScreenStates(&ts, 1) && ts.count > 0) {
        g_touch = true;
        g_touch_x = int(ts.touches[0].x);
        g_touch_y = int(ts.touches[0].y);
    }
}

uint32_t ds_keys() {
    return g_keys;
}

bool touch(int& x, int& y) {
    if (!g_touch)
        return false;
    x = g_touch_x;
    y = g_touch_y;
    return true;
}

void right_stick(float& x, float& y) {
    x = g_rs_x;
    y = g_rs_y;
}

uint32_t hotkeys_held() {
    return g_held;
}

uint32_t hotkeys_pressed() {
    return g_pressed;
}

Buttons ui_held_buttons() {
    if (!g_ready)
        return 0;
    padUpdate(&g_ui_pad);
    return padGetButtons(&g_ui_pad) & kAllNamed;
}

}  // namespace input
