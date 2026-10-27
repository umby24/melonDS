#pragma once
// Root activity: a tab frame with the ROM list ("Games", from the rom_dir option) and
// the host options ("Settings"). Plus quits the app.

#include <string>

namespace brls {
class Activity;
}

namespace ui {

brls::Activity* make_main_activity(const std::string& title);

}  // namespace ui
