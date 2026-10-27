#pragma once
// SD card directory helpers shared by the ROM list and the folder picker.

#include <string>
#include <vector>

namespace ui {

struct DirEntry {
    std::string name;
    bool is_dir;
};

// Folders first, then files, each sorted case-insensitively. Hidden entries (".*") are
// skipped. Files are included only if `with_nds` and they end in ".nds".
std::vector<DirEntry> list_dir(const std::string& dir, bool with_nds);

bool is_dir(const std::string& path);
std::string join_path(const std::string& dir, const std::string& name);

// "sdmc:/roms/nds" -> "sdmc:/roms", "sdmc:/roms" -> "sdmc:/"; the root maps to itself.
std::string parent_path(const std::string& dir);
bool is_root(const std::string& dir);

}  // namespace ui
