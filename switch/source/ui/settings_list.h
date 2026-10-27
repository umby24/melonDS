#pragma once
// Builds borealis cells for host options. Shared by the Settings tab and the in-game
// overlay, so a setting looks and behaves the same in both places.

#include <string>
#include <vector>

namespace brls {
class Box;
class View;
}  // namespace brls

namespace ui {

// One SelectorCell per key, appended to `box`. Changes apply immediately (the host
// re-applies dirty options before the next frame) and are saved to config.ini.
void add_option_cells(brls::Box* box, const std::vector<std::string>& keys);

// UI-side effects of an option the user just changed (e.g. rotating borealis).
void apply_ui_option(const std::string& key);

// The Settings tab (main menu only): the ROM folder (folder picker) and the general,
// display and emulation options, in sections. Controls and RetroAchievements have tabs
// of their own.
brls::View* make_settings_list();

}  // namespace ui
