#pragma once
// Title and icon of an NDS ROM, read from its header and banner (a few KB, never the
// whole file). Results are cached per path (and file size). Any thread.

#include <cstdint>
#include <string>
#include <vector>

namespace rom_info {

struct Info {
    std::string title;      // banner title, e.g. "Mario Kart DS" (first line)
    std::string subtitle;   // further title lines, if any
    std::string publisher;  // last banner line, e.g. "Nintendo"
    std::string game_code;  // e.g. "AMCE"
    std::vector<uint32_t> icon;  // 32x32 RGBA8 (bytes R,G,B,A); empty without a banner
};

// False if the file can't be read or isn't an NDS ROM. Without a banner the title
// falls back to the header's short game title.
bool read(const std::string& path, Info& out);

}  // namespace rom_info
