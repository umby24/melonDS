#pragma once
// Controls: the touch cursor and fast-forward options, hotkeys, and the DS button
// mapping. Bindings are captured by pressing the buttons (see BindActivity in the .cpp).

namespace brls {
class View;
}

namespace ui {

brls::View* make_input_settings();  // the main menu's Controls tab
void open_input_settings();         // as its own screen (from the in-game overlay)

}  // namespace ui
