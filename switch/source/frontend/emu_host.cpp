#include "frontend/emu_host.h"

#include <Args.h>
#include <CRC32.h>
#include <DSi.h>
#include <DSi_NAND.h>
#include <FreeBIOS.h>
#include <GPU.h>
#include <NDS.h>
#include <NDSCart.h>
#include <Platform.h>
#include <SPI_Firmware.h>
#include <Savestate.h>
#include <ScreenLayout.h>
#include <mic_blow.h>  // desktop melonDS's "blow into the mic" sample

#include <pthread.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <utility>
#include <vector>

#include "frontend/achievements.h"
#include "frontend/cheats.h"
#include "frontend/melon_platform.h"
#include "frontend/options.h"
#include "platform/audio.h"
#include "platform/input.h"
#include "platform/system.h"

namespace host {

namespace {

using melonDS::u8;
using melonDS::u32;

constexpr unsigned kSaveFlushDelay = 60;   // frames after the last save write before flushing
constexpr int kScreenW = 256;
constexpr int kScreenH = 192;
constexpr double kFramePeriod = 560190.0 / 33513982.0;  // DS: 560190 ARM7 cycles per frame (~59.83 Hz)
constexpr int kEmuCore = 1;                 // UI on core 0, 3D render + audio on core 2
constexpr size_t kEmuStack = 1024 * 1024;
constexpr unsigned kRtcSyncFrames = 60 * 60;  // ~1 minute
constexpr float kFastForwardAudioMs = 60.0f;  // while fast-forwarding, keep at most this queued
constexpr float kCursorDeadzone = 0.15f;
constexpr float kCursorSpeed = 6.0f;          // DS pixels per frame at full deflection

Paths g_paths;
std::string g_rom_name;  // basename without extension
std::string g_rom_dir;   // folder the ROM was loaded from
std::string g_save_path; // resolved at load (see resolve_game_file)
std::string g_last_error;

std::unique_ptr<melonDS::NDS> g_nds;
std::atomic<bool> g_stopped{false};  // console stopped itself (Platform::SignalStop)

// Battery save, mirrored from the core by Platform::WriteNDSSave.
std::vector<u8> g_save;
bool g_save_dirty = false;
unsigned g_frames_since_save_write = 0;

// --- emulation thread ------------------------------------------------------------
// The emu thread owns g_nds while it runs. The UI thread only touches g_nds after
// pause() has returned (thread parked) or after the thread has been joined.
// g_mx guards the flags below, the display rect, and every options:: access made while
// a game runs (the emu thread reads options each frame; the UI writes via set_option).
std::mutex g_mx;
std::condition_variable g_cv;
pthread_t g_thread;
bool g_thread_running = false;
bool g_quit = false;       // thread should exit
bool g_pause_req = false;  // thread should park
bool g_parked = false;     // thread is parked (safe to touch g_nds)
ScreenLayout g_ui_layout;  // from set_screen_layout, with its touch map and a version
float g_ui_touch_mtx[6] = {1, 0, 0, 1, 0, 0};
unsigned g_ui_layout_version = 0;

std::atomic<uint32_t> g_events{0};
std::atomic<int> g_auto_emphasis{screenSizing_EmphTop};
std::atomic<bool> g_fast_forward{false};
std::atomic<bool> g_mic{false};  // read by Platform::Mic_ReadInput during RunFrame
std::mutex g_cursor_mx;
Cursor g_cursor_pub = {};        // what the UI draws
std::atomic<float> g_stat_fps{0.0f};
std::atomic<float> g_stat_frame_ms{0.0f};
std::atomic<float> g_stat_audio_pct{0.0f};
size_t g_audio_pushed = 0;  // emu thread: stereo frames pushed in the current stats window

// Frames: RGBA8 bytes (R,G,B,A), triple-buffered between emu and UI threads.
std::vector<uint32_t> g_bufs[3];
int g_back = 0, g_ready = 1, g_front = 2;  // emu writes back, UI reads front
bool g_fresh = false;                       // ready holds a frame the UI hasn't taken
std::mutex g_frame_mx;

// Emu-thread-only state.
struct FrameSettings {     // option snapshot taken under g_mx before each frame
    enum CursorMode { CURSOR_OFF, CURSOR_MOUSE, CURSOR_OFFSET } cursor;
    bool touch_toggle;
    bool ff_toggle;
    int ff_speed;          // 0 = unlimited
};
ScreenLayout g_layout;     // emu thread's copy of g_ui_layout (keeps hybrid touch state)
float g_touch_mtx[6] = {1, 0, 0, 1, 0, 0};
unsigned g_layout_version = 0;
struct CursorState {
    float x = 128, y = 96;
    bool visible = false;
    bool toggled = false;  // touch_toggle mode: currently pressed
} g_cur;
bool g_ff_toggled = false;
int g_power_history[3] = {};
size_t g_mic_pos = 0;

std::vector<int16_t> g_audio;
unsigned g_frame_count = 0;

bool make_dir(const std::string& path) {
    return mkdir(path.c_str(), 0777) == 0 || errno == EEXIST;
}

std::string rom_basename(const std::string& path) {
    size_t slash = path.find_last_of('/');
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

// A per-game file (<rom>.sav / <rom>.mch) may live next to the ROM (desktop melonDS's
// default) or in our own folder. Use whichever exists, preferring the `save_location`
// choice; for a new file, use that choice. Writes go back to the same file.
std::string resolve_game_file(const std::string& app_dir, const std::string& ext) {
    std::string beside_rom = g_rom_dir + "/" + g_rom_name + ext;
    std::string in_app = app_dir + "/" + g_rom_name + ext;
    bool prefer_rom = options::is("save_location", "rom_folder");
    const std::string& first = prefer_rom ? beside_rom : in_app;
    const std::string& second = prefer_rom ? in_app : beside_rom;
    struct stat st;
    if (stat(first.c_str(), &st) == 0)
        return first;
    if (stat(second.c_str(), &st) == 0)
        return second;
    return first;
}

std::string save_path() {
    return g_save_path;
}

std::string state_path(int slot) {
    return g_paths.states + "/" + g_rom_name + ".ml" + std::to_string(slot);
}

void log_memory(const char* when) {
    unsigned long long used = 0, total = 0;
    sys::memory_usage(used, total);
    fprintf(stderr, "[host] memory %s: %llu / %llu MB used\n", when, used >> 20, total >> 20);
}

bool read_file(const std::string& path, std::unique_ptr<u8[]>& data, u32& len) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    bool ok = fseek(f, 0, SEEK_END) == 0;
    long size = ok ? ftell(f) : -1;
    ok = size > 0 && fseek(f, 0, SEEK_SET) == 0;
    if (ok) {
        data.reset(new (std::nothrow) u8[size_t(size)]);
        ok = data != nullptr;
    }
    if (ok) {
        ok = fread(data.get(), 1, size_t(size), f) == size_t(size);
        len = u32(size);
    }
    fclose(f);
    if (!ok)
        data.reset();
    return ok;
}

// Write to <path>.tmp, then rename over <path>, so a crash never leaves a torn file.
bool write_file_atomic(const std::string& path, const void* data, size_t len) {
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f)
        return false;
    bool ok = fwrite(data, 1, len, f) == len;
    ok = (fclose(f) == 0) && ok;
    if (ok) {
        remove(path.c_str());
        ok = rename(tmp.c_str(), path.c_str()) == 0;
    } else {
        remove(tmp.c_str());
    }
    return ok;
}

// Fixed-size dump from system/, or nullptr when absent or the wrong size.
template <typename Image>
std::unique_ptr<Image> load_bios(const char* name) {
    std::unique_ptr<u8[]> data;
    u32 len = 0;
    std::string path = g_paths.system + "/" + name;
    if (!read_file(path, data, len))
        return nullptr;
    auto image = std::make_unique<Image>();
    if (len != image->size()) {
        fprintf(stderr, "[host] ignoring %s: %u bytes, expected %zu\n", path.c_str(), len, image->size());
        return nullptr;
    }
    std::memcpy(image->data(), data.get(), len);
    fprintf(stderr, "[host] using %s\n", path.c_str());
    return image;
}

melonDS::Firmware load_firmware(bool dsi) {
    std::string path = g_paths.system + (dsi ? "/firmware_dsi.bin" : "/firmware.bin");
    if (FILE* f = fopen(path.c_str(), "rb")) {
        melonDS::Firmware fw(reinterpret_cast<melonDS::Platform::FileHandle*>(f));
        fclose(f);
        if (fw.Buffer()) {
            fprintf(stderr, "[host] using %s\n", path.c_str());
            return fw;
        }
        fprintf(stderr, "[host] ignoring %s: not a valid firmware image\n", path.c_str());
    }
    return melonDS::Firmware(dsi ? 1 : 0);  // generated firmware
}

// system/nand.bin, as desktop melonDS loads it (needs the DSi ARM7 BIOS for its keys),
// with the touchscreen calibration reset so touches land where they should. nullopt
// without a usable image; DSi mode then needs direct boot.
std::optional<melonDS::DSi_NAND::NANDImage> load_nand(const melonDS::DSiBIOSImage& arm7i) {
    using namespace melonDS;
    std::string path = g_paths.system + "/nand.bin";
    Platform::FileHandle* file = Platform::OpenFile(path, Platform::FileMode::ReadWriteExisting);
    if (!file)
        return std::nullopt;
    DSi_NAND::NANDImage nand(file, &arm7i[0x8308]);  // takes ownership of the file
    if (!nand) {
        fprintf(stderr, "[host] %s: not a usable DSi NAND (does bios7i.bin match it?)\n", path.c_str());
        return std::nullopt;
    }
    {
        DSi_NAND::NANDMount mount(nand);
        DSi_NAND::DSiFirmwareSystemSettings settings {};
        if (!mount || !mount.ReadUserData(settings)) {
            fprintf(stderr, "[host] %s: could not read its user settings\n", path.c_str());
            return std::nullopt;
        }
        settings.TouchCalibrationADC1 = {0, 0};
        settings.TouchCalibrationPixel1 = {0, 0};
        settings.TouchCalibrationADC2 = {255 << 4, 191 << 4};
        settings.TouchCalibrationPixel2 = {255, 191};
        settings.UpdateHash();
        if (!mount.ApplyUserData(settings))
            fprintf(stderr, "[host] %s: could not update its touchscreen calibration\n", path.c_str());
    }
    fprintf(stderr, "[host] using %s\n", path.c_str());
    return nand;
}

// DS keys in DsKey bit order, with their option keys and default buttons.
struct BindingOption {
    input::DsKey key;
    const char* option;
    const char* label;
    const char* default_buttons;
};
constexpr BindingOption kBindingOptions[] = {
    {input::DS_A, "bind_a", "A", "A"},              {input::DS_B, "bind_b", "B", "B"},
    {input::DS_SELECT, "bind_select", "Select", "Minus"},
    {input::DS_START, "bind_start", "Start", "Plus"},
    {input::DS_RIGHT, "bind_right", "Right", "Right"}, {input::DS_LEFT, "bind_left", "Left", "Left"},
    {input::DS_UP, "bind_up", "Up", "Up"},          {input::DS_DOWN, "bind_down", "Down", "Down"},
    {input::DS_R, "bind_r", "R", "R"},              {input::DS_L, "bind_l", "L", "L"},
    {input::DS_X, "bind_x", "X", "X"},              {input::DS_Y, "bind_y", "Y", "Y"},
};
struct HotkeyOption {
    input::Hotkey hotkey;
    const char* option;
    const char* label;
    const char* default_buttons;
};
constexpr HotkeyOption kHotkeyOptions[] = {
    {input::HOTKEY_MENU, "hotkey_menu", "Open menu", "Plus+Minus"},
    {input::HOTKEY_FAST_FORWARD, "hotkey_fast_forward", "Fast forward", "ZL"},
    {input::HOTKEY_EMPHASIS, "hotkey_emphasis", "Switch screen emphasis", "RStick"},
    {input::HOTKEY_MIC, "hotkey_mic", "Blow into the microphone", "LStick"},
    {input::HOTKEY_TOUCH, "hotkey_touch", "Touch at the cursor", "ZR"},
};

void define_options() {
    options::define_text("rom_dir", "ROM folder", "sdmc:/roms/nds");
    options::define("console_type", "Console (applies to the next game)", {"ds", "dsi"},
                    {"Nintendo DS", "Nintendo DSi (experimental)"});
    options::define("ui_rotation", "Rotate the interface", {"0", "90", "180", "270"},
                    {"Off", "90° clockwise", "180°", "90° counter-clockwise"});
    options::define("screen_layout", "Screen layout", {"stacked", "side_by_side", "hybrid"},
                    {"Stacked", "Side by side", "Hybrid (one large + both small)"});
    options::define("screen_sizing", "Screen sizes",
                    {"even", "emph_top", "emph_bottom", "auto", "top_only", "bottom_only"},
                    {"Equal", "Emphasize top", "Emphasize bottom", "Auto (follow the 3D screen)",
                     "Top screen only", "Bottom screen only"});
    options::define("swap_screens", "Swap screens", {"off", "on"}, {"Off", "On"});
    options::define("scaling", "Scaling", {"fit", "integer"}, {"Fit to screen", "Integer"});
    options::define("filter", "Filtering", {"nearest", "linear"}, {"Sharp (nearest)", "Smooth (linear)"});
    options::define("show_fps", "Show FPS", {"off", "on"}, {"Off", "On"});
    options::define("threaded_3d", "Threaded 3D rendering", {"on", "off"}, {"On", "Off"});
    options::define("audio_interpolation", "Audio interpolation",
                    {"none", "linear", "cosine", "cubic", "gaussian"},
                    {"None", "Linear", "Cosine", "Cubic", "SNES Gaussian"});
    options::define("save_location", "Where new save and cheat files go", {"rom_folder", "app_folder"},
                    {"Next to the ROM", "melonds-next folder"});
    options::define("cpu_clock", "CPU clock while playing", {"stock", "1224", "1581", "1785"},
                    {"Stock (1020 MHz)", "1224 MHz", "1581 MHz", "1785 MHz"});
    options::define("direct_boot", "Direct boot (skip firmware menu)", {"on", "off"}, {"On", "Off"});
    options::define("jit", "JIT recompiler (applies to the next game)", {"on", "off"}, {"On", "Off"});
    options::define("fastmem", "JIT fast memory (applies to the next game)", {"on", "off"}, {"On", "Off"});

    options::define("touch_cursor", "Touch cursor (right stick)", {"mouse", "offset", "off"},
                    {"Move like a mouse", "Point from the centre", "Off"});
    options::define("touch_click", "Touch button", {"hold", "toggle"}, {"Hold to touch", "Press to toggle"});
    options::define("fast_forward_mode", "Fast forward button", {"hold", "toggle"},
                    {"Hold", "Press to toggle"});
    options::define("fast_forward_speed", "Fast forward speed", {"3", "2", "4", "unlimited"},
                    {"3×", "2×", "4×", "Unlimited"});
    for (const BindingOption& b : kBindingOptions)
        options::define_text(b.option, std::string("DS ") + b.label, b.default_buttons);
    for (const HotkeyOption& h : kHotkeyOptions)
        options::define_text(h.option, h.label, h.default_buttons);

    options::define("card_image", "Game card picture", {"art", "icon"},
                    {"Downloaded cover art (icon if none)", "ROM icon"});
    options::define("thumb_type", "Cover art to download", {"box", "coverS"},
                    {"Box (3D)", "Front cover"});

    options::define("ra_hardcore", "Hardcore mode (applies to the next game)", {"off", "on"}, {"Off", "On"});
}

void apply_input_options() {
    for (const BindingOption& b : kBindingOptions)
        input::set_binding(b.key, input::parse_buttons(options::get(b.option)));
    for (const HotkeyOption& h : kHotkeyOptions)
        input::set_hotkey(h.hotkey, input::parse_buttons(options::get(h.option)));
    input::set_rotation(std::stoi(options::get("ui_rotation")) / 90);
}

melonDS::AudioInterpolation interpolation_option() {
    const std::string& v = options::get("audio_interpolation");
    if (v == "linear")   return melonDS::AudioInterpolation::Linear;
    if (v == "cosine")   return melonDS::AudioInterpolation::Cosine;
    if (v == "cubic")    return melonDS::AudioInterpolation::Cubic;
    if (v == "gaussian") return melonDS::AudioInterpolation::SNESGaussian;
    return melonDS::AudioInterpolation::None;
}

// Higher clocks cost battery and heat; restored to stock when the game ends.
void apply_cpu_clock() {
    const std::string& v = options::get("cpu_clock");
    if (v == "stock")
        sys::restore_cpu_clock();
    else if (!sys::set_cpu_clock_mhz(unsigned(std::stoul(v))))
        fprintf(stderr, "[host] setting the CPU clock to %s MHz failed\n", v.c_str());
}

// Pushes option values into the running core.
void apply_options() {
    if (!g_nds)
        return;
    melonDS::RendererSettings rs = {};
    rs.ScaleFactor = 1;
    rs.Threaded = options::is("threaded_3d", "on");
    g_nds->GetRenderer().SetRenderSettings(rs);
    g_nds->SPU.SetInterpolation(interpolation_option());
    apply_cpu_clock();
    apply_input_options();
}

// The DS clock otherwise only advances with emulated time (it stops while paused,
// lags when the game runs slow, and save states rewind it), so the host re-syncs it to
// the Switch's local time at boot, on every resume and once a minute.
void set_rtc_to_now() {
    sys::LocalTime t;
    if (sys::local_time(t)) {
        g_nds->RTC.SetDateTime(t.year, t.month, t.day, t.hour, t.minute, t.second);
        return;
    }
    time_t now = time(nullptr);  // UTC on Switch; better than nothing
    tm u = {};
    gmtime_r(&now, &u);
    g_nds->RTC.SetDateTime(u.tm_year + 1900, u.tm_mon + 1, u.tm_mday, u.tm_hour, u.tm_min, u.tm_sec);
}

// melonDS gives 0xAARRGGBB (bytes B,G,R,A); the UI's RGBA textures want bytes R,G,B,A.
void convert_rows(uint32_t* dst, const uint32_t* src, int rows) {
    for (int i = 0; i < rows * kScreenW; ++i) {
        uint32_t c = src[i];
        dst[i] = (c & 0xFF00FF00u) | ((c >> 16) & 0xFFu) | ((c & 0xFFu) << 16);
    }
}

// Both screens into the back buffer (see acquire_frame for the format); hands it over.
void publish_frame() {
    void* top_v = nullptr;
    void* bottom_v = nullptr;
    if (!g_nds->GPU.GetFramebuffers(&top_v, &bottom_v) || !top_v || !bottom_v)
        return;
    std::vector<uint32_t>& fb = g_bufs[g_back];
    fb.resize(size_t(kFrameWidth) * kFrameHeight);
    uint32_t* top = fb.data();
    uint32_t* bottom = fb.data() + size_t(kBottomScreenRow) * kScreenW;
    convert_rows(top, static_cast<const uint32_t*>(top_v), kScreenH);
    convert_rows(bottom, static_cast<const uint32_t*>(bottom_v), kScreenH);
    std::memcpy(top + kScreenH * kScreenW, top + (kScreenH - 1) * kScreenW, kScreenW * sizeof(uint32_t));
    std::memcpy(bottom - kScreenW, bottom, kScreenW * sizeof(uint32_t));

    std::lock_guard<std::mutex> lk(g_frame_mx);
    std::swap(g_back, g_ready);
    g_fresh = true;
}

// Real touches win; otherwise the right-stick cursor (if on) touches while its button is
// held (or toggled on).
void update_touch(const FrameSettings& fs) {
    int tx, ty;
    if (input::touch(tx, ty)) {
        g_cur.visible = false;
        const float* m = g_touch_mtx;
        int lx = int(m[0] * tx + m[2] * ty + m[4]);
        int ly = int(m[1] * tx + m[3] * ty + m[5]);
        if (g_layout.GetTouchCoords(lx, ly, false))
            g_nds->TouchScreen(uint16_t(lx), uint16_t(ly));
        else
            g_nds->ReleaseScreen();
        return;
    }

    bool pressed = false;
    if (fs.cursor != FrameSettings::CURSOR_OFF) {
        float sx, sy;
        input::right_stick(sx, sy);
        float mag = std::sqrt(sx * sx + sy * sy);
        if (fs.cursor == FrameSettings::CURSOR_MOUSE) {
            if (mag > kCursorDeadzone) {
                // Quadratic response: small deflections for precision, full for speed.
                float speed = kCursorSpeed * (mag - kCursorDeadzone) / (1.0f - kCursorDeadzone);
                g_cur.x += sx / mag * speed * mag;
                g_cur.y += sy / mag * speed * mag;
                g_cur.visible = true;
            }
        } else {
            if (mag > kCursorDeadzone)
                g_cur.visible = true;
            g_cur.x = kScreenW / 2.0f + sx * kScreenW / 2.0f;
            g_cur.y = kScreenH / 2.0f + sy * kScreenH / 2.0f;
        }
        g_cur.x = std::clamp(g_cur.x, 0.0f, kScreenW - 1.0f);
        g_cur.y = std::clamp(g_cur.y, 0.0f, kScreenH - 1.0f);

        uint32_t held = input::hotkeys_held(), down = input::hotkeys_pressed();
        if (fs.touch_toggle) {
            if (down & input::HOTKEY_TOUCH)
                g_cur.toggled = !g_cur.toggled;
            pressed = g_cur.toggled;
        } else {
            pressed = held & input::HOTKEY_TOUCH;
        }
        if (pressed)
            g_cur.visible = true;
    } else {
        g_cur.visible = false;
        g_cur.toggled = false;
    }

    if (pressed)
        g_nds->TouchScreen(uint16_t(g_cur.x), uint16_t(g_cur.y));
    else
        g_nds->ReleaseScreen();

    std::lock_guard<std::mutex> lk(g_cursor_mx);
    g_cursor_pub = {g_cur.visible, g_cur.x, g_cur.y, pressed};
}

// Which screen gets engine A (the 3D one): POWCNT1 bit 15. Flipping every frame means
// the game shows 3D on both screens.
void update_auto_emphasis() {
    g_power_history[2] = g_power_history[1];
    g_power_history[1] = g_power_history[0];
    g_power_history[0] = g_nds->PowerControl9 >> 15;
    if (g_power_history[0] == g_power_history[2] && g_power_history[0] != g_power_history[1])
        g_auto_emphasis = screenSizing_Even;
    else
        g_auto_emphasis = g_power_history[0] ? screenSizing_EmphTop : screenSizing_EmphBot;
}

// While fast-forwarding, audio is produced faster than it plays; drop what doesn't fit
// in a short queue rather than let latency pile up.
void drain_audio(bool fast_forward) {
    constexpr int kChunk = 1024;  // stereo frames
    g_audio.resize(kChunk * 2);
    int n;
    while ((n = g_nds->SPU.ReadOutput(g_audio.data(), kChunk)) > 0) {
        if (!fast_forward || audio::stats().buffered_ms < kFastForwardAudioMs)
            audio::push(g_audio.data(), size_t(n));
        g_audio_pushed += size_t(n);
        if (n < kChunk)
            break;
    }
}

// One emulated frame: input -> RunFrame -> audio -> publish. Emu thread only. Returns
// whether fast forward is on.
bool run_frame(const FrameSettings& fs) {
    input::poll();
    g_nds->SetKeyMask(~input::ds_keys() & input::DS_ALL);
    update_touch(fs);
    uint32_t held = input::hotkeys_held(), down = input::hotkeys_pressed();
    g_mic = held & input::HOTKEY_MIC;
    if (down & input::HOTKEY_FAST_FORWARD)
        g_ff_toggled = !g_ff_toggled;
    bool ff = fs.ff_toggle ? g_ff_toggled : (held & input::HOTKEY_FAST_FORWARD) != 0;
    g_fast_forward = ff;

    // Log the first frames step by step so a crash report can be placed.
    bool trace = g_frame_count < 3;
    if (trace)
        fprintf(stderr, "[host] frame %u: RunFrame\n", g_frame_count);
    g_nds->RunFrame();
    if (trace)
        fprintf(stderr, "[host] frame %u: audio + compose\n", g_frame_count);
    ++g_frame_count;
    achievements::do_frame();
    update_auto_emphasis();

    drain_audio(ff);
    publish_frame();

    if (g_save_dirty && ++g_frames_since_save_write >= kSaveFlushDelay)
        flush_save();

    if (down & input::HOTKEY_EMPHASIS)
        g_events |= EVENT_EMPHASIS;
    if (down & input::HOTKEY_MENU) {
        std::lock_guard<std::mutex> lk(g_mx);
        g_pause_req = true;  // park right away; the UI opens the overlay
        g_events |= EVENT_MENU;
        g_fast_forward = false;
    }
    return ff;
}

FrameSettings snapshot_settings() {
    FrameSettings fs;
    const std::string& cursor = options::get("touch_cursor");
    fs.cursor = cursor == "mouse" ? FrameSettings::CURSOR_MOUSE
              : cursor == "offset" ? FrameSettings::CURSOR_OFFSET : FrameSettings::CURSOR_OFF;
    fs.touch_toggle = options::is("touch_click", "toggle");
    fs.ff_toggle = options::is("fast_forward_mode", "toggle");
    const std::string& speed = options::get("fast_forward_speed");
    fs.ff_speed = speed == "unlimited" ? 0 : std::stoi(speed);
    return fs;
}

void* emu_thread_main(void*) {
    sys::set_current_thread_core(kEmuCore);
    using clock = std::chrono::steady_clock;
    const auto period = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(kFramePeriod));

    clock::time_point next = clock::now();
    clock::time_point stat_start = next;
    clock::duration stat_work{0};
    unsigned stat_frames = 0;

    for (;;) {
        FrameSettings fs;
        {
            std::unique_lock<std::mutex> lk(g_mx);
            if (g_pause_req || g_stopped) {
                g_parked = true;
                g_fast_forward = false;
                g_cv.notify_all();
                // Wake up once a second so RetroAchievements can keep its session alive.
                while (!g_cv.wait_for(lk, std::chrono::seconds(1), [] { return g_quit || (!g_pause_req && !g_stopped); })) {
                    lk.unlock();
                    achievements::idle();
                    lk.lock();
                }
                g_parked = false;
                if (!g_quit)
                    set_rtc_to_now();  // time passed while paused (or a state rewound it)
                next = stat_start = clock::now();
                stat_work = clock::duration{0};
                stat_frames = 0;
                g_audio_pushed = 0;
            }
            if (g_quit)
                break;
            if (options::consume_dirty())
                apply_options();
            fs = snapshot_settings();
            if (g_layout_version != g_ui_layout_version) {
                g_layout = g_ui_layout;
                std::memcpy(g_touch_mtx, g_ui_touch_mtx, sizeof(g_touch_mtx));
                g_layout_version = g_ui_layout_version;
            }
        }

        if (g_frame_count % kRtcSyncFrames == 0) {
            set_rtc_to_now();
            std::lock_guard<std::mutex> lk(g_mx);
            apply_cpu_clock();  // the OS resets it after HOME-menu visits
        }

        clock::time_point t0 = clock::now();
        bool ff = run_frame(fs);
        clock::time_point t1 = clock::now();
        stat_work += t1 - t0;
        ++stat_frames;

        if (t1 - stat_start >= std::chrono::milliseconds(500)) {
            double secs = std::chrono::duration<double>(t1 - stat_start).count();
            g_stat_fps = float(stat_frames / secs);
            g_stat_frame_ms = float(std::chrono::duration<double, std::milli>(stat_work).count() / stat_frames);
            g_stat_audio_pct = float(g_audio_pushed / secs / kAudioRate * 100.0);
            g_audio_pushed = 0;
            stat_start = t1;
            stat_work = clock::duration{0};
            stat_frames = 0;
        }

        // Pace to the DS refresh rate (or a multiple of it). Running late by more than a
        // few frames: give up catching up rather than speeding through them.
        if (ff && fs.ff_speed == 0) {
            next = t1;
            continue;
        }
        next += ff ? period / fs.ff_speed : period;
        if (next > t1)
            std::this_thread::sleep_until(next);
        else if (t1 - next > 3 * period)
            next = t1;
    }

    std::lock_guard<std::mutex> lk(g_mx);
    g_parked = true;
    g_cv.notify_all();
    return nullptr;
}

bool start_emu_thread() {
    g_quit = g_pause_req = g_parked = false;
    g_events = 0;
    g_fast_forward = g_mic = false;
    g_ff_toggled = false;
    g_cur = CursorState{};
    g_cursor_pub = {};
    g_layout_version = g_ui_layout_version - 1;  // pick up the UI's layout on the first frame
    g_stat_fps = g_stat_frame_ms = 0.0f;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kEmuStack);
    g_thread_running = pthread_create(&g_thread, &attr, emu_thread_main, nullptr) == 0;
    pthread_attr_destroy(&attr);
    return g_thread_running;
}

void stop_emu_thread() {
    if (!g_thread_running)
        return;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_quit = true;
    }
    g_cv.notify_all();
    pthread_join(g_thread, nullptr);
    g_thread_running = false;
}

// Parks the emu thread for the guard's lifetime (no-op if it already was).
class PauseGuard {
  public:
    PauseGuard() {
        std::lock_guard<std::mutex> lk(g_mx);
        was_paused_ = g_pause_req;
    }
    void park() { pause(); }
    ~PauseGuard() {
        if (!was_paused_)
            resume();
    }

  private:
    bool was_paused_;
};

}  // namespace

bool init(const Paths& paths) {
    g_paths = paths;
    for (const std::string* dir : {&g_paths.base, &g_paths.system, &g_paths.saves, &g_paths.states, &g_paths.cheats})
        if (!make_dir(*dir))
            fprintf(stderr, "[host] mkdir %s failed: %s\n", dir->c_str(), strerror(errno));

    melon_platform::set_local_dir(g_paths.system);
    options::load(g_paths.config);
    define_options();
    apply_input_options();
    achievements::set_hardcore(options::is("ra_hardcore", "on"));
    return true;
}

void shutdown() {
    unload_game();
    options::save(g_paths.config);
}

bool load_game(const std::string& rom_path) {
    unload_game();
    g_last_error.clear();
    g_rom_name = rom_basename(rom_path);
    g_rom_dir = rom_path.substr(0, rom_path.find_last_of('/'));
    g_save_path = resolve_game_file(g_paths.saves, ".sav");
    fprintf(stderr, "[host] save file: %s\n", g_save_path.c_str());

    fprintf(stderr, "[host] loading %s\n", rom_path.c_str());
    log_memory("before load");
    std::unique_ptr<u8[]> rom;
    u32 rom_len = 0;
    if (!read_file(rom_path, rom, rom_len)) {
        g_last_error = "could not read the ROM (too large for available memory?)";
        return false;
    }

    // RetroAchievements identifies the game by hashing the file as it is on the SD.
    std::string ra_hash = achievements::hash_rom(rom.get(), rom_len);

    melonDS::NDSCart::NDSCartArgs cart_args;
    std::unique_ptr<u8[]> sav;
    u32 sav_len = 0;
    if (read_file(save_path(), sav, sav_len)) {
        cart_args.SRAM = std::move(sav);
        cart_args.SRAMLength = sav_len;
    }
    fprintf(stderr, "[host] read %u-byte ROM, %u-byte save; parsing\n", rom_len, cart_args.SRAMLength);
    auto cart = melonDS::NDSCart::ParseROM(std::move(rom), rom_len, nullptr, std::move(cart_args));
    if (!cart) {
        if (g_last_error.empty())
            g_last_error = "not a valid NDS ROM";
        return false;
    }

    const bool dsi = options::is("console_type", "dsi");
    melonDS::NDSArgs args;
    if (auto bios9 = load_bios<melonDS::ARM9BIOSImage>("bios9.bin"))
        args.ARM9BIOS = std::move(bios9);
    if (auto bios7 = load_bios<melonDS::ARM7BIOSImage>("bios7.bin"))
        args.ARM7BIOS = std::move(bios7);
    args.Firmware = load_firmware(dsi);
#ifdef JIT_ENABLED
    if (options::is("jit", "on")) {
        melonDS::JITArgs jit;
        jit.FastMemory = options::is("fastmem", "on");
        args.JIT = jit;
        fprintf(stderr, "[host] JIT on, fastmem %s\n", jit.FastMemory ? "on" : "off");
    } else {
        args.JIT = std::nullopt;
    }
#else
    args.JIT = std::nullopt;
#endif
    args.Interpolation = interpolation_option();
    args.OutputSampleRate = kAudioRate;

    // DSi mode: DSi BIOS dumps (or FreeBIOS) and an optional NAND image.
    std::optional<melonDS::DSiArgs> dsi_args;
    if (dsi) {
        auto arm9i = load_bios<melonDS::DSiBIOSImage>("bios9i.bin");
        auto arm7i = load_bios<melonDS::DSiBIOSImage>("bios7i.bin");
        if (!arm9i)
            arm9i = std::make_unique<melonDS::DSiBIOSImage>(melonDS::FreeBIOSGetTwlArm9());
        if (!arm7i)
            arm7i = std::make_unique<melonDS::DSiBIOSImage>(melonDS::FreeBIOSGetTwlArm7());
        auto nand = load_nand(*arm7i);
        if (!nand)
            fprintf(stderr, "[host] DSi without a NAND image: direct boot only\n");
        dsi_args = melonDS::DSiArgs{std::move(args), std::move(arm9i), std::move(arm7i), std::move(nand),
                                    std::nullopt, true /* DSP HLE */};
    }

    g_stopped = false;
    g_frames_since_save_write = 0;
    g_save.clear();
    g_save_dirty = false;
    log_memory("before creating NDS");
    fprintf(stderr, "[host] creating %s\n", dsi ? "DSi" : "NDS");
    // Construct on zeroed memory. Desktop melonDS effectively gets that (a fresh large
    // allocation comes straight from the OS), and the core has relied on it: reading
    // uninitialized members during construction crashed here at random (see
    // docs/CORE_PATCHES.md, GPU.h). Freed by unique_ptr's default operator delete.
    size_t size = dsi ? sizeof(melonDS::DSi) : sizeof(melonDS::NDS);
    void* mem = ::operator new(size, std::nothrow);
    if (!mem) {
        g_last_error = "not enough memory to create the console";
        return false;
    }
    std::memset(mem, 0, size);
    if (dsi)
        g_nds = std::unique_ptr<melonDS::NDS>(new (mem) melonDS::DSi(std::move(*dsi_args), nullptr));
    else
        g_nds = std::unique_ptr<melonDS::NDS>(new (mem) melonDS::NDS(std::move(args), nullptr));
    fprintf(stderr, "[host] NDS created; resetting\n");
    log_memory("after creating NDS");
    g_nds->Reset();
    fprintf(stderr, "[host] inserting cart\n");
    g_nds->SetNDSCart(std::move(cart));
    g_nds->Reset();
    if (const auto* cart = g_nds->GetNDSCart()) {
        // Before cheats: hardcore mode decides whether they apply.
        achievements::on_game_loaded(*g_nds, ra_hash);
        uint32_t checksum = ~melonDS::CRC32(cart->GetROM(), 0x200, 0);
        cheats::on_game_loaded(resolve_game_file(g_paths.cheats, ".mch"),
                               g_paths.system + "/usrcheat.dat",
                               cart->GetHeader().GameCodeAsU32(), checksum);
    }
    if (options::is("direct_boot", "on") || g_nds->NeedsDirectBoot()) {
        fprintf(stderr, "[host] direct boot\n");
        g_nds->SetupDirectBoot(rom_path.substr(rom_path.find_last_of('/') + 1));
    }
    set_rtc_to_now();
    apply_options();
    options::consume_dirty();
    g_power_history[0] = g_power_history[1] = g_power_history[2] = 0;
    g_mic_pos = 0;
    g_nds->Start();
    g_frame_count = 0;
    if (!start_emu_thread()) {
        g_last_error = "could not start the emulation thread";
        achievements::on_game_unloaded();
        cheats::on_game_unloaded();
        g_nds.reset();
        return false;
    }
    fprintf(stderr, "[host] started\n");
    return true;
}

void unload_game() {
    if (!g_nds)
        return;
    stop_emu_thread();
    sys::restore_cpu_clock();
    achievements::on_game_unloaded();
    cheats::on_game_unloaded();
    flush_save();
    g_nds->Stop();
    g_nds.reset();
    g_rom_name.clear();
    {
        std::lock_guard<std::mutex> lk(g_frame_mx);
        for (auto& fb : g_bufs)
            fb.clear();
        g_fresh = false;
    }
    g_fast_forward = false;
    {
        std::lock_guard<std::mutex> lk(g_cursor_mx);
        g_cursor_pub = {};
    }
    log_memory("after unload");
}

bool game_loaded() {
    return g_nds != nullptr;
}

void pause() {
    std::unique_lock<std::mutex> lk(g_mx);
    if (!g_thread_running)
        return;
    g_pause_req = true;
    g_cv.wait(lk, [] { return g_parked; });
}

void resume() {
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_pause_req = false;
    }
    g_cv.notify_all();
}

bool stopped() {
    return g_nds && g_stopped;
}

uint32_t consume_events() {
    return g_events.exchange(0);
}

int auto_emphasis() {
    return g_auto_emphasis;
}

bool fast_forwarding() {
    return g_fast_forward;
}

Cursor cursor() {
    std::lock_guard<std::mutex> lk(g_cursor_mx);
    return g_cursor_pub;
}

Stats stats() {
    audio::Stats a = audio::stats();
    return {g_stat_fps.load(), g_stat_frame_ms.load(), g_stat_audio_pct.load(), a.buffered_ms, a.underruns, a.overflows};
}

bool set_option(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!options::set(key, value))
        return false;
    options::save(g_paths.config);
    if (key == "ra_hardcore")
        achievements::set_hardcore(value == "on");
    if (!g_thread_running)
        apply_input_options();  // otherwise the emu thread applies dirty options itself
    return true;
}

const uint32_t* acquire_frame(bool& fresh) {
    std::lock_guard<std::mutex> lk(g_frame_mx);
    fresh = g_fresh;
    if (g_fresh) {
        std::swap(g_front, g_ready);
        g_fresh = false;
    }
    const auto& fb = g_bufs[g_front];
    return fb.empty() ? nullptr : fb.data();
}

void set_screen_layout(const ScreenLayout& layout, const float touch_to_layout[6]) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (std::memcmp(&g_ui_layout, &layout, sizeof(ScreenLayout)) == 0 &&
        std::memcmp(g_ui_touch_mtx, touch_to_layout, sizeof(g_ui_touch_mtx)) == 0)
        return;
    g_ui_layout = layout;
    std::memcpy(g_ui_touch_mtx, touch_to_layout, sizeof(g_ui_touch_mtx));
    ++g_ui_layout_version;
}

bool flush_save() {
    if (!g_save_dirty)
        return true;
    if (!write_file_atomic(save_path(), g_save.data(), g_save.size())) {
        fprintf(stderr, "[host] failed to write %s\n", save_path().c_str());
        return false;
    }
    g_save_dirty = false;
    return true;
}

bool with_console(const std::function<void(melonDS::NDS&)>& fn) {
    if (!g_nds)
        return false;
    PauseGuard guard;
    guard.park();
    fn(*g_nds);
    return true;
}

bool save_state(int slot) {
    if (!g_nds)
        return false;
    PauseGuard guard;
    guard.park();
    melonDS::Savestate state;
    if (state.Error || !g_nds->DoSavestate(&state) || state.Error)
        return false;
    state.Finish();
    if (!write_file_atomic(state_path(slot), state.Buffer(), state.Length()))
        return false;
    // Achievement progress (e.g. partially met conditions) travels with the state.
    std::vector<uint8_t> progress = achievements::serialize();
    std::string ra_path = state_path(slot) + ".ra";
    if (progress.empty())
        remove(ra_path.c_str());
    else
        write_file_atomic(ra_path, progress.data(), progress.size());
    return true;
}

bool load_state(int slot) {
    if (!g_nds)
        return false;
    if (achievements::hardcore_active()) {
        g_last_error = "loading states is disabled in RetroAchievements hardcore mode";
        return false;
    }
    std::unique_ptr<u8[]> data;
    u32 len = 0;
    if (!read_file(state_path(slot), data, len))
        return false;
    PauseGuard guard;
    guard.park();

    // A state that fails halfway leaves the console in an undefined state; roll back.
    melonDS::Savestate backup;
    if (backup.Error || !g_nds->DoSavestate(&backup) || backup.Error)
        return false;
    backup.Finish();

    melonDS::Savestate state(data.get(), len, false);
    if (g_nds->DoSavestate(&state) && !state.Error) {
        std::unique_ptr<u8[]> ra;
        u32 ra_len = 0;
        std::vector<uint8_t> progress;
        if (read_file(state_path(slot) + ".ra", ra, ra_len))
            progress.assign(ra.get(), ra.get() + ra_len);
        achievements::deserialize(progress);
        return true;
    }

    melonDS::Savestate restore(backup.Buffer(), backup.Length(), false);
    g_nds->DoSavestate(&restore);
    return false;
}

const std::string& last_error() {
    return g_last_error;
}

}  // namespace host

// --- melonDS Platform:: callbacks that touch host state --------------------------------

namespace melonDS::Platform {

void Log(LogLevel level, const char* fmt, ...) {
    static const char* const kLevel[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

#ifdef NDEBUG
    if (level == LogLevel::Debug)
        return;
#endif
    fprintf(stderr, "[melonDS %s] %s", kLevel[level <= LogLevel::Error ? level : LogLevel::Error], buf);
    size_t n = strlen(buf);
    if (n == 0 || buf[n - 1] != '\n')
        fprintf(stderr, "\n");

    if (level == LogLevel::Error) {
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == '.'))
            buf[--n] = '\0';
        host::g_last_error = buf;
    }
}

void SignalStop(StopReason reason, void*) {
    fprintf(stderr, "[host] emulation stopped (reason %d)\n", int(reason));
    host::g_stopped = true;
}

void WriteNDSSave(const u8* savedata, u32 savelen, u32, u32, void*) {
    host::g_save.assign(savedata, savedata + savelen);
    host::g_save_dirty = true;
    host::g_frames_since_save_write = 0;
}

// Generated firmware is rebuilt every boot and we never write back user dumps, so
// changes made in the firmware settings menu don't persist yet.
void WriteFirmware(const Firmware&, u32, u32, void*) {}

void WriteDateTime(int, int, int, int, int, int, void*) {}

void Mic_Start(void*) {}
void Mic_Stop(void*) {}

// The mic hotkey plays desktop melonDS's breath sample (looped); otherwise silence.
int Mic_ReadInput(s16* data, int maxlength, void*) {
    constexpr size_t kLen = sizeof(mic_blow) / sizeof(mic_blow[0]);
    if (!host::g_mic) {
        host::g_mic_pos = 0;
        std::memset(data, 0, size_t(maxlength) * sizeof(s16));
        return maxlength;
    }
    for (int i = 0; i < maxlength; ++i) {
        data[i] = mic_blow[host::g_mic_pos];
        host::g_mic_pos = (host::g_mic_pos + 1) % kLen;
    }
    return maxlength;
}

}  // namespace melonDS::Platform
