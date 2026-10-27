#pragma once
// Games tab: .nds files and folders under a directory as a grid of cards. ROM cards
// show the icon and title from the ROM's banner (frontend/rom_info, read in the
// background); folder cards open a new activity for that folder (B goes back); choosing
// a ROM calls ui::launch_game.

#include <string>

namespace brls {
class View;
}

namespace ui {

brls::View* make_rom_list(const std::string& dir);

}  // namespace ui
