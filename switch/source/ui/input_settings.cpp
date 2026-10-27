#include "ui/input_settings.h"

#include <borealis.hpp>

#include <chrono>
#include <functional>

#include "frontend/emu_host.h"
#include "frontend/options.h"
#include "platform/input.h"
#include "ui/settings_list.h"

namespace ui {

namespace {

using clock = std::chrono::steady_clock;
constexpr auto kCaptureTimeout = std::chrono::seconds(5);

bool starts_with(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

// "Press the buttons" prompt. borealis' input is blocked meanwhile (so A or B bind
// rather than click / go back); the view polls the pad itself. Waits for everything to
// be released, collects every button held during the next press, and finishes when they
// are all released again. Nothing pressed for a few seconds cancels.
class BindActivity : public brls::Activity {
  public:
    BindActivity(std::string title, bool combo, std::function<void(input::Buttons)> done)
        : title_(std::move(title)), combo_(combo), done_(std::move(done)) {}

    bool isTranslucent() override {
        return true;
    }

    brls::View* createContentView() override;

    void onContentAvailable() override {
        brls::Application::blockInputs(true);
        blocked_ = true;
        start_ = clock::now();
    }

    void tick() {
        if (finished_ || !blocked_)
            return;
        input::Buttons held = input::ui_held_buttons();
        if (!armed_) {
            if (held == 0) {
                armed_ = true;
                start_ = clock::now();
            }
            return;
        }
        if (held) {
            got_ |= held;
        } else if (got_) {
            finish(true);
            return;
        }
        if (!got_ && clock::now() - start_ > kCaptureTimeout)
            finish(false);
    }

  private:
    std::string title_;
    bool combo_;
    std::function<void(input::Buttons)> done_;
    bool blocked_ = false, armed_ = false, finished_ = false;
    input::Buttons got_ = 0;
    clock::time_point start_;

    void finish(bool ok) {
        finished_ = true;
        brls::Application::unblockInputs();
        blocked_ = false;
        input::Buttons got = got_;
        auto done = done_;
        brls::sync([ok, got, done] {
            brls::Application::popActivity(brls::TransitionAnimation::FADE, [ok, got, done] {
                if (ok)
                    done(got);
                else
                    brls::Application::notify("No button pressed; binding unchanged");
            });
        });
    }
};

class BindView : public brls::Box {
  public:
    explicit BindView(BindActivity* activity) : brls::Box(brls::Axis::COLUMN), activity_(activity) {}

    void draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style,
              brls::FrameContext* ctx) override {
        activity_->tick();
        brls::Box::draw(vg, x, y, width, height, style, ctx);
    }

  private:
    BindActivity* activity_;
};

brls::View* BindActivity::createContentView() {
    auto* root = new BindView(this);
    root->setBackgroundColor(nvgRGBA(0, 0, 0, 170));
    root->setJustifyContent(brls::JustifyContent::CENTER);
    root->setAlignItems(brls::AlignItems::CENTER);

    auto* panel = new brls::Box(brls::Axis::COLUMN);
    panel->setBackgroundColor(brls::Application::getTheme().getColor("brls/background"));
    panel->setPadding(40, 50, 40, 50);
    panel->setCornerRadius(8);
    panel->setAlignItems(brls::AlignItems::CENTER);

    auto* heading = new brls::Label();
    heading->setText(title_);
    heading->setFontSize(26);
    panel->addView(heading);

    auto* hint = new brls::Label();
    hint->setText(combo_ ? "Press the button, or hold several together for a combination."
                         : "Press the button (hold several together to bind them all).");
    hint->setMargins(16, 0, 0, 0);
    panel->addView(hint);

    auto* hint2 = new brls::Label();
    hint2->setText("Release to confirm. Press nothing for 5 seconds to cancel.");
    hint2->setFontSize(16);
    hint2->setMargins(8, 0, 0, 0);
    panel->addView(hint2);

    root->addView(panel);
    return root;
}

void add_header(brls::Box* list, const std::string& text) {
    auto* header = new brls::Header();
    header->setTitle(text);
    list->addView(header);
}

// One DetailCell per option whose key starts with `prefix` (bindings are free-form
// options holding button names; see frontend/emu_host.cpp).
void add_binding_cells(brls::Box* list, const char* prefix, bool combo) {
    for (const options::Option& o : options::all()) {
        if (!o.free_form || !starts_with(o.key, prefix))
            continue;
        auto* cell = new brls::DetailCell();
        cell->setText(o.description);
        cell->setDetailText(input::describe_buttons(input::parse_buttons(o.current)));
        std::string key = o.key, title = o.description;
        cell->registerClickAction([cell, key, title, combo](brls::View*) {
            brls::Application::pushActivity(
                new BindActivity(title, combo, [cell, key](input::Buttons b) {
                    if (host::set_option(key, input::format_buttons(b)))
                        cell->setDetailText(input::describe_buttons(b));
                }),
                brls::TransitionAnimation::FADE);
            return true;
        });
        cell->registerAction("Clear", brls::BUTTON_X, [cell, key](brls::View*) {
            if (key == "hotkey_menu") {
                brls::Application::notify("The menu hotkey can't be cleared: it's the only way out of a game");
                return true;
            }
            if (host::set_option(key, "none"))
                cell->setDetailText(input::describe_buttons(0));
            return true;
        });
        list->addView(cell);
    }
}

void reset_bindings() {
    for (const options::Option& o : options::all())
        if (o.free_form && (starts_with(o.key, "bind_") || starts_with(o.key, "hotkey_")))
            host::set_option(o.key, o.values[0]);
}

brls::Box* make_list() {
    auto* list = new brls::Box(brls::Axis::COLUMN);
    list->setPadding(40, 60, 40, 60);

    add_header(list, "Touch screen");
    add_option_cells(list, {"touch_cursor", "touch_click"});

    add_header(list, "Fast forward");
    add_option_cells(list, {"fast_forward_mode", "fast_forward_speed"});

    add_header(list, "Hotkeys (X clears)");
    add_binding_cells(list, "hotkey_", true);

    add_header(list, "DS buttons (X clears)");
    add_binding_cells(list, "bind_", false);

    auto* hint = new brls::Label();
    hint->setText("A hotkey's buttons don't reach the game while the hotkey is held. The left stick "
                  "always works as the D-pad; the right stick drives the touch cursor.");
    hint->setFontSize(16);
    hint->setMargins(16, 0, 16, 0);
    list->addView(hint);

    auto* reset = new brls::DetailCell();
    reset->setText("Reset hotkeys and buttons to defaults");
    reset->registerClickAction([](brls::View*) {
        auto* dialog = new brls::Dialog("Reset every hotkey and DS button binding?");
        dialog->addButton("Cancel", [] {});
        dialog->addButton("Reset", [] {
            reset_bindings();
            // Rebuild the screen around fresh values (never under the focused cell).
            brls::Application::notify("Controls reset; reopen this screen to see them");
        });
        dialog->open();
        return true;
    });
    list->addView(reset);
    return list;
}

}  // namespace

brls::View* make_input_settings() {
    auto* scroll = new brls::ScrollingFrame();
    scroll->setContentView(make_list());
    scroll->setGrow(1.0f);
    return scroll;
}

void open_input_settings() {
    auto* frame = new brls::AppletFrame(make_input_settings());
    frame->setTitle("Controls and hotkeys");
    brls::Application::pushActivity(new brls::Activity(frame));
}

}  // namespace ui
