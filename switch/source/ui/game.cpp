#include "ui/game.h"

#include <ScreenLayout.h>
#include <borealis.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "frontend/achievements.h"
#include "frontend/emu_host.h"
#include "frontend/options.h"
#include "platform/audio.h"
#include "ui/achievements_view.h"
#include "ui/cheats_view.h"
#include "ui/input_settings.h"
#include "ui/settings_list.h"

namespace ui {

namespace {

constexpr float kTouchWidth = 1280.0f;  // hid touchscreen coordinates are always 1280x720
constexpr float kTouchHeight = 720.0f;
constexpr int kStateSlots = 4;
constexpr float kPanelWidth = 460.0f;
constexpr float kDsFps = 33513982.0f / 560190.0f;  // ~59.83

class GameActivity;
void open_overlay(GameActivity* game);
void exit_game(GameActivity* game);

// borealis counts block tokens without bounds checks; keep exactly one while in a game.
bool g_inputs_blocked = false;
void block_ui_inputs(bool block) {
    if (block == g_inputs_blocked)
        return;
    g_inputs_blocked = block;
    if (block)
        brls::Application::blockInputs(true);
    else
        brls::Application::unblockInputs();
}

class GameView : public brls::View {
  public:
    explicit GameView(GameActivity* activity) : activity_(activity) {
        setFocusable(true);
        setHideHighlight(true);
        setGrow(1.0f);
    }

    ~GameView() override {
        if (image_)
            nvgDeleteImage(brls::Application::getNVGContext(), image_);
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style,
              brls::FrameContext*) override;

  private:
    GameActivity* activity_;
    int image_ = 0;
    bool image_linear_ = false;

    // Layout inputs last sent to the host (it only needs a new layout when they change).
    struct LayoutKey {
        float x, y, w, h;
        int type, sizing, rotation;
        bool integer, swap;
        bool operator==(const LayoutKey& o) const {
            return x == o.x && y == o.y && w == o.w && h == o.h && type == o.type && sizing == o.sizing &&
                   rotation == o.rotation && integer == o.integer && swap == o.swap;
        }
    };
    LayoutKey last_key_ = {};
    bool have_layout_ = false;
    ScreenLayout layout_;

    void upload(NVGcontext* vg, const uint32_t* pixels);
    void update_layout(float x, float y, float width, float height);
    void draw_cursor(NVGcontext* vg, float x, float y, const float* bottom_mtx);
    void draw_stats(NVGcontext* vg, float x, float y);
    void draw_fast_forward(NVGcontext* vg, float right, float y);
};

// The emphasis hotkey: swap which screen is large (from "equal" / "auto", emphasize the
// screen the game draws 3D on, or the other one).
void toggle_emphasis() {
    const std::string& v = options::get("screen_sizing");
    bool bottom_now = host::auto_emphasis() == screenSizing_EmphBot;
    std::string next;
    if (v == "emph_top")         next = "emph_bottom";
    else if (v == "emph_bottom") next = "emph_top";
    else if (v == "top_only")    next = "bottom_only";
    else if (v == "bottom_only") next = "top_only";
    else if (v == "auto")        next = bottom_now ? "emph_top" : "emph_bottom";
    else                         next = bottom_now ? "emph_bottom" : "emph_top";
    host::set_option("screen_sizing", next);
}

ScreenSizing sizing_option() {
    const std::string& v = options::get("screen_sizing");
    if (v == "emph_top")    return screenSizing_EmphTop;
    if (v == "emph_bottom") return screenSizing_EmphBot;
    if (v == "top_only")    return screenSizing_TopOnly;
    if (v == "bottom_only") return screenSizing_BotOnly;
    if (v == "auto")        return ScreenSizing(host::auto_emphasis());
    return screenSizing_Even;
}

ScreenLayoutType layout_option() {
    const std::string& v = options::get("screen_layout");
    if (v == "side_by_side") return screenLayout_Horizontal;
    if (v == "hybrid")       return screenLayout_Hybrid;
    return screenLayout_Natural;
}

class GameActivity : public brls::Activity {
  public:
    brls::View* createContentView() override {
        auto* box = new brls::Box(brls::Axis::COLUMN);
        box->setBackgroundColor(nvgRGB(0, 0, 0));
        box->addView(new GameView(this));
        return box;
    }

    void onContentAvailable() override {
        block_ui_inputs(true);
    }

    int slot = 1;          // save-state slot chosen in the overlay
    bool exiting = false;  // teardown scheduled; the view must stop running frames
};

// Tears the game down after the current UI frame (never from inside GameView::draw).
// Idempotent: the activity stays on the stack (and drawn) during its fade-out, and a
// second popActivity would leak one of borealis' input-block tokens, locking the menu.
void exit_game(GameActivity* game) {
    if (game->exiting)
        return;
    game->exiting = true;
    brls::sync([] {
        host::unload_game();
        audio::shutdown();
        block_ui_inputs(false);
        brls::Application::popActivity(brls::TransitionAnimation::FADE);
    });
}

void GameView::upload(NVGcontext* vg, const uint32_t* pixels) {
    bool linear = options::is("filter", "linear");
    const auto* data = reinterpret_cast<const unsigned char*>(pixels);
    if (image_ && linear != image_linear_) {
        nvgDeleteImage(vg, image_);
        image_ = 0;
    }
    if (!image_) {
        image_ = nvgCreateImageRGBA(vg, int(host::kFrameWidth), int(host::kFrameHeight),
                                    linear ? 0 : NVG_IMAGE_NEAREST, data);
        image_linear_ = linear;
    } else {
        nvgUpdateImage(vg, image_, data);
    }
}

// Recomputes the screen layout (upstream melonDS's ScreenLayout, in this view's content
// units) and, when it changed, hands it to the host along with the map from raw touch
// pixels into it, which undoes borealis' rotation and scale and this view's offset.
void GameView::update_layout(float x, float y, float width, float height) {
    LayoutKey key = {x, y, width, height, int(layout_option()), int(sizing_option()),
                     brls::Application::getRotation(), options::is("scaling", "integer"),
                     options::is("swap_screens", "on")};
    if (have_layout_ && key == last_key_)
        return;
    last_key_ = key;
    have_layout_ = true;
    layout_.Setup(int(width), int(height), ScreenLayoutType(key.type), screenRot_0Deg, ScreenSizing(key.sizing),
                  0, key.integer, key.swap, 1.0f, 1.0f);

    // Touch pixel -> physical window pixel -> content unit.
    float k = float(brls::Application::windowWidth) / kTouchWidth / brls::Application::windowScale;
    float W = kTouchWidth * k, H = kTouchHeight * k;
    float m[6];
    switch (key.rotation) {
    case 1:  m[0] = 0;  m[1] = -k; m[2] = k;  m[3] = 0;  m[4] = -x;    m[5] = W - y; break;
    case 2:  m[0] = -k; m[1] = 0;  m[2] = 0;  m[3] = -k; m[4] = W - x; m[5] = H - y; break;
    case 3:  m[0] = 0;  m[1] = k;  m[2] = -k; m[3] = 0;  m[4] = H - x; m[5] = -y;    break;
    default: m[0] = k;  m[1] = 0;  m[2] = 0;  m[3] = k;  m[4] = -x;    m[5] = -y;    break;
    }
    host::set_screen_layout(layout_, m);
}

void GameView::draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style,
                    brls::FrameContext*) {
    auto stack = brls::Application::getActivitiesStack();
    bool on_top = !stack.empty() && stack.back() == activity_;

    // Emulation runs on its own thread (frontend/emu_host); this view only reacts to it.
    if (!activity_->exiting) {
        if (host::stopped()) {
            exit_game(activity_);  // the console powered itself off
        } else if (on_top) {
            uint32_t events = host::consume_events();
            if (events & host::EVENT_EMPHASIS)
                toggle_emphasis();
            if (events & host::EVENT_MENU)
                brls::sync([a = activity_] { open_overlay(a); });
        }
    }

    bool fresh = false;
    const uint32_t* pixels = host::acquire_frame(fresh);
    if (!pixels)
        return;
    if (fresh || !image_)
        upload(vg, pixels);

    update_layout(x, y, width, height);
    float mtx[kMaxScreenTransforms * 6];
    int kind[kMaxScreenTransforms];
    int count = layout_.GetScreenTransforms(mtx, kind);
    const float* bottom_mtx = nullptr;
    for (int i = 0; i < count; ++i) {
        const float* m = &mtx[i * 6];
        float src_y = kind[i] ? float(host::kBottomScreenRow) : 0.0f;
        nvgSave(vg);
        nvgTranslate(vg, x, y);
        nvgTransform(vg, m[0], m[1], m[2], m[3], m[4], m[5]);
        nvgBeginPath(vg);
        nvgRect(vg, 0, 0, 256, 192);
        nvgFillPaint(vg, nvgImagePattern(vg, 0, -src_y, float(host::kFrameWidth), float(host::kFrameHeight),
                                         0.0f, image_, 1.0f));
        nvgFill(vg);
        nvgRestore(vg);
        if (kind[i] == 1 && !bottom_mtx)
            bottom_mtx = m;
    }

    if (bottom_mtx)
        draw_cursor(vg, x, y, bottom_mtx);
    if (host::fast_forwarding())
        draw_fast_forward(vg, x + width - 12, y + 12);
    if (options::is("show_fps", "on"))
        draw_stats(vg, x + 12, y + 12);
}

// An arrow pointer at the cursor's spot on the bottom screen; darker while touching.
void GameView::draw_cursor(NVGcontext* vg, float x, float y, const float* m) {
    host::Cursor c = host::cursor();
    if (!c.visible)
        return;
    float px = x + m[0] * c.x + m[2] * c.y + m[4];
    float py = y + m[1] * c.x + m[3] * c.y + m[5];
    static const float kArrow[][2] = {{0, 0}, {0, 22}, {6, 17}, {10, 26}, {14, 24}, {10, 16}, {17, 16}};
    nvgBeginPath(vg);
    for (size_t i = 0; i < sizeof(kArrow) / sizeof(kArrow[0]); ++i) {
        if (i == 0)
            nvgMoveTo(vg, px + kArrow[i][0], py + kArrow[i][1]);
        else
            nvgLineTo(vg, px + kArrow[i][0], py + kArrow[i][1]);
    }
    nvgClosePath(vg);
    nvgFillColor(vg, c.pressed ? nvgRGB(120, 190, 255) : nvgRGB(255, 255, 255));
    nvgFill(vg);
    nvgStrokeColor(vg, nvgRGBA(0, 0, 0, 220));
    nvgStrokeWidth(vg, 2.0f);
    nvgStroke(vg);
}

void GameView::draw_fast_forward(NVGcontext* vg, float right, float y) {
    const char* text = ">> Fast forward";
    nvgFontFaceId(vg, brls::Application::getDefaultFont());
    nvgFontSize(vg, 20);
    nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
    float bounds[4];
    nvgTextBounds(vg, right, y, text, nullptr, bounds);
    constexpr float kPad = 6;
    nvgBeginPath(vg);
    nvgRoundedRect(vg, bounds[0] - kPad, bounds[1] - kPad, bounds[2] - bounds[0] + 2 * kPad,
                   bounds[3] - bounds[1] + 2 * kPad, 4);
    nvgFillColor(vg, nvgRGBA(0, 0, 0, 170));
    nvgFill(vg);
    nvgFillColor(vg, nvgRGB(255, 255, 255));
    nvgText(vg, right, y, text, nullptr);
}

// "59.8 FPS (100%) · 9.4 ms": emulated frame rate, speed vs the DS, and emulation time
// per frame (the budget at full speed is ~16.7 ms).
// "Audio 100.0% · 40 ms queued": audio produced vs real time, and output latency; gap /
// skip counts appear once any happened.
void GameView::draw_stats(NVGcontext* vg, float x, float y) {
    host::Stats st = host::stats();
    char text[160];
    int len = snprintf(text, sizeof(text), "%.1f FPS (%.0f%%) · %.1f ms\nAudio %.1f%% · %.0f ms queued",
                       st.fps, st.fps / kDsFps * 100.0f, st.frame_ms, st.audio_pct, st.audio_ms);
    if (st.underruns || st.overflows)
        snprintf(text + len, sizeof(text) - len, " · %u gaps, %u skips", st.underruns, st.overflows);

    nvgFontFaceId(vg, brls::Application::getDefaultFont());
    nvgFontSize(vg, 20);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    float bounds[4];
    nvgTextBoxBounds(vg, x, y, 1000, text, nullptr, bounds);

    constexpr float kPad = 6;
    nvgBeginPath(vg);
    nvgRoundedRect(vg, bounds[0] - kPad, bounds[1] - kPad, bounds[2] - bounds[0] + 2 * kPad,
                   bounds[3] - bounds[1] + 2 * kPad, 4);
    nvgFillColor(vg, nvgRGBA(0, 0, 0, 170));
    nvgFill(vg);

    nvgFillColor(vg, nvgRGB(255, 255, 255));
    nvgTextBox(vg, x, y, 1000, text, nullptr);
}

// --- in-game overlay ---------------------------------------------------------------

class OverlayActivity : public brls::Activity {
  public:
    explicit OverlayActivity(GameActivity* game) : game_(game) {}

    bool isTranslucent() override {
        return true;
    }

    brls::View* createContentView() override;

    void onContentAvailable() override {
        host::pause();  // usually already parked by the emu thread on the hotkey
        block_ui_inputs(false);
    }

  private:
    GameActivity* game_;

    void resume() {
        block_ui_inputs(true);
        brls::Application::popActivity(brls::TransitionAnimation::FADE, [] { host::resume(); });
    }

    brls::DetailCell* action(brls::Box* list, const std::string& text, std::function<void()> fn) {
        auto* cell = new brls::DetailCell();
        cell->setText(text);
        cell->registerClickAction([fn](brls::View*) {
            fn();
            return true;
        });
        list->addView(cell);
        return cell;
    }

    void section(brls::Box* list, const std::string& text) {
        auto* header = new brls::Header();
        header->setTitle(text);
        list->addView(header);
    }
};

brls::View* OverlayActivity::createContentView() {
    // Dim the paused game; menu panel on the right.
    auto* root = new brls::Box(brls::Axis::ROW);
    root->setBackgroundColor(nvgRGBA(0, 0, 0, 150));
    root->setJustifyContent(brls::JustifyContent::FLEX_END);

    auto* panel = new brls::Box(brls::Axis::COLUMN);
    panel->setWidth(kPanelWidth);
    panel->setBackgroundColor(brls::Application::getTheme().getColor("brls/background"));

    auto* list = new brls::Box(brls::Axis::COLUMN);
    list->setPadding(30, 30, 30, 30);

    action(list, "Resume", [this] { resume(); });

    section(list, "Save states");
    std::vector<std::string> slots;
    for (int i = 1; i <= kStateSlots; ++i)
        slots.push_back("Slot " + std::to_string(i));
    auto* slot = new brls::SelectorCell();
    slot->init("Slot", slots, game_->slot - 1, [](int) {}, [g = game_](int i) { g->slot = i + 1; });
    list->addView(slot);
    action(list, "Save state", [g = game_] {
        brls::Application::notify(host::save_state(g->slot)
            ? "Saved state to slot " + std::to_string(g->slot) : "Saving the state failed");
    });
    action(list, "Load state", [this, g = game_] {
        if (achievements::hardcore_active())
            brls::Application::notify("Loading states is off in RetroAchievements hardcore mode");
        else if (host::load_state(g->slot))
            resume();
        else
            brls::Application::notify("No usable state in slot " + std::to_string(g->slot));
    });

    section(list, "Display");
    add_option_cells(list, {"screen_layout", "screen_sizing", "swap_screens", "scaling", "filter",
                            "ui_rotation", "show_fps"});

    section(list, "Controls");
    action(list, "Controls and hotkeys", [] { open_input_settings(); });

    section(list, "Emulation");
    add_option_cells(list, {"cpu_clock", "audio_interpolation", "threaded_3d"});

    section(list, "Cheats");
    action(list, "Manage cheats", [] {
        if (achievements::hardcore_active())
            brls::Application::notify("Cheats are off in RetroAchievements hardcore mode");
        open_cheats();
    });

    achievements::Status ra = achievements::status();
    if (ra.game_loaded) {
        section(list, "RetroAchievements");
        auto* cell = action(list, "Achievements", [] { open_achievements(); });
        cell->setDetailText(std::to_string(ra.unlocked) + " of " + std::to_string(ra.total) + " unlocked");
    }

    section(list, "Game");
    action(list, "Quit game", [g = game_] {
        brls::Application::popActivity(brls::TransitionAnimation::NONE, [g] { exit_game(g); });
    });

    auto* scroll = new brls::ScrollingFrame();
    scroll->setContentView(list);
    scroll->setGrow(1.0f);
    panel->addView(scroll);
    root->addView(panel);

    root->registerAction("Resume", brls::BUTTON_B, [this](brls::View*) {
        resume();
        return true;
    });
    root->registerAction("Resume", brls::BUTTON_START, [this](brls::View*) {
        resume();
        return true;
    }, true);
    return root;
}

void open_overlay(GameActivity* game) {
    auto stack = brls::Application::getActivitiesStack();
    if (!stack.empty() && stack.back() == game)
        brls::Application::pushActivity(new OverlayActivity(game), brls::TransitionAnimation::FADE);
}

}  // namespace

void launch_game(const std::string& rom_path) {
    audio::init(host::kAudioRate);
    if (!host::load_game(rom_path)) {
        audio::shutdown();
        auto* dialog = new brls::Dialog("Failed to load the ROM:\n" + host::last_error());
        dialog->addButton("OK", [] {});
        dialog->open();
        return;
    }
    brls::Application::pushActivity(new GameActivity(), brls::TransitionAnimation::FADE);
}

}  // namespace ui
