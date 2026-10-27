#pragma once
// RetroAchievements screens: the main menu tab (account, hardcore mode) and the running
// game's achievement list (from the in-game overlay). See frontend/achievements.h.

namespace brls {
class View;
}

namespace ui {

brls::View* make_achievements_settings();  // main menu tab
void open_achievements();                  // the running game's list

}  // namespace ui
