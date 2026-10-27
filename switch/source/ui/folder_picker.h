#pragma once
// File-browser style folder picker: one activity listing the subfolders of the current
// folder. A opens a folder, B goes up (closes at the SD root), X or the "Use this folder"
// row picks the current folder and closes the picker.

#include <functional>
#include <string>

namespace ui {

// `start` falls back to the SD root if it doesn't exist. `on_pick` runs on the UI thread.
void pick_folder(const std::string& title, const std::string& start,
                 std::function<void(const std::string&)> on_pick);

}  // namespace ui
