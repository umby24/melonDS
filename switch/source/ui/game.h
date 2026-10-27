#pragma once
// Gameplay inside borealis. A GameActivity holds one full-screen GameView, which runs
// one emulated frame per UI frame and draws the composed DS screens as a NanoVG image.
// The in-game menu (Plus + Minus) is a translucent OverlayActivity pushed on top:
// while it is up, GameView stops calling host::run_frame (emulation is paused) but keeps
// drawing the last frame underneath.
//
// borealis' own input handling is blocked during gameplay (the DS gets every button) and
// unblocked while the overlay is open.

#include <string>

namespace ui {

// Loads the ROM and pushes the game activity; shows a dialog on failure.
void launch_game(const std::string& rom_path);

}  // namespace ui
