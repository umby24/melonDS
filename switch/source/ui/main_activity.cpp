#include "ui/main_activity.h"

#include <borealis.hpp>

#include "frontend/options.h"
#include "ui/achievements_view.h"
#include "ui/downloader_view.h"
#include "ui/input_settings.h"
#include "ui/rom_list.h"
#include "ui/settings_list.h"

namespace ui {

brls::Activity* make_main_activity(const std::string& title) {
    auto* tabs = new brls::TabFrame();
    // Tabs are rebuilt each time they're selected, so a new ROM folder shows up directly.
    tabs->addTab("Games", [] { return make_rom_list(options::get("rom_dir")); });
    tabs->addSeparator();
    tabs->addTab("Settings", [] { return make_settings_list(); });
    tabs->addTab("Controls", [] { return make_input_settings(); });
    tabs->addTab("RetroAchievements", [] { return make_achievements_settings(); });
    tabs->addTab("Downloader", [] { return make_downloader(); });

    auto* frame = new brls::AppletFrame(tabs);
    frame->setTitle(title);
    frame->registerAction("Quit", brls::BUTTON_START, [](brls::View*) {
        brls::Application::quit();
        return true;
    });
    return new brls::Activity(frame);
}

}  // namespace ui
