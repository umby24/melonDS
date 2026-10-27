#include "frontend/rom_info.h"

#include <sys/stat.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace rom_info {

namespace {

constexpr size_t kHeaderSize = 0x200;
constexpr size_t kBannerSize = 0x840;  // version 1: icon, palette, 6 titles
constexpr size_t kIconBitmap = 0x20;
constexpr size_t kIconPalette = 0x220;
constexpr size_t kTitles = 0x240;
constexpr size_t kTitleBytes = 0x100;  // 128 UTF-16 code units
constexpr int kTitleJapanese = 0, kTitleEnglish = 1;

struct Cached {
    long long size;
    bool ok;
    Info info;
};
std::mutex g_mx;
std::unordered_map<std::string, Cached> g_cache;

uint32_t le32(const uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24);
}

void append_utf8(std::string& out, uint32_t c) {
    if (c < 0x80) {
        out += char(c);
    } else if (c < 0x800) {
        out += char(0xC0 | (c >> 6));
        out += char(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += char(0xE0 | (c >> 12));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    } else {
        out += char(0xF0 | (c >> 18));
        out += char(0x80 | ((c >> 12) & 0x3F));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    }
}

std::string utf16_to_utf8(const uint8_t* p, size_t units) {
    std::string out;
    for (size_t i = 0; i < units; ++i) {
        uint32_t c = p[2 * i] | (p[2 * i + 1] << 8);
        if (c == 0)
            break;
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < units) {
            uint32_t lo = p[2 * i + 2] | (p[2 * i + 3] << 8);
            if (lo >= 0xDC00 && lo < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            }
        }
        append_utf8(out, c);
    }
    return out;
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\0'))
        s.pop_back();
    size_t start = s.find_first_not_of(' ');
    return start == std::string::npos ? "" : s.substr(start);
}

// "Title\nSubtitle\nPublisher" (2 or 3 lines; 1 for some homebrew).
void split_title(const std::string& text, Info& out) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos)
            end = text.size();
        std::string line = trim(text.substr(start, end - start));
        if (!line.empty())
            lines.push_back(line);
        start = end + 1;
    }
    if (lines.empty())
        return;
    out.title = lines[0];
    if (lines.size() >= 2)
        out.publisher = lines.back();
    for (size_t i = 1; i + 1 < lines.size(); ++i)
        out.subtitle += (out.subtitle.empty() ? "" : " ") + lines[i];
}

// 32x32, 4x4 tiles of 8x8 pixels, 4 bits per pixel (low nibble first); 16-colour
// BGR555 palette whose entry 0 is transparent.
void decode_icon(const uint8_t* banner, std::vector<uint32_t>& out) {
    uint32_t palette[16];
    for (int i = 0; i < 16; ++i) {
        uint32_t c = banner[kIconPalette + 2 * i] | (banner[kIconPalette + 2 * i + 1] << 8);
        uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
        r = (r << 3) | (r >> 2);
        g = (g << 3) | (g >> 2);
        b = (b << 3) | (b >> 2);
        palette[i] = i == 0 ? 0 : (r | (g << 8) | (b << 16) | 0xFF000000u);
    }
    out.resize(32 * 32);
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            int tile = (y / 8) * 4 + x / 8;
            int within = (y % 8) * 8 + x % 8;
            uint8_t byte = banner[kIconBitmap + tile * 32 + within / 2];
            out[y * 32 + x] = palette[(within & 1) ? byte >> 4 : byte & 0xF];
        }
    }
}

bool read_uncached(const std::string& path, Info& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    uint8_t header[kHeaderSize];
    bool ok = fread(header, 1, kHeaderSize, f) == kHeaderSize;
    if (ok) {
        char code[5] = {};
        std::memcpy(code, header + 0x0C, 4);
        out.game_code = code;
        char short_title[13] = {};
        std::memcpy(short_title, header, 12);
        for (char& c : short_title)
            if (c && (c < 0x20 || c > 0x7E))
                c = ' ';
        out.title = trim(short_title);

        uint32_t banner_at = le32(header + 0x68);
        uint8_t banner[kBannerSize];
        if (banner_at >= kHeaderSize && fseek(f, long(banner_at), SEEK_SET) == 0 &&
            fread(banner, 1, kBannerSize, f) == kBannerSize) {
            decode_icon(banner, out.icon);
            std::string text = utf16_to_utf8(banner + kTitles + kTitleEnglish * kTitleBytes, kTitleBytes / 2);
            if (trim(text).empty())
                text = utf16_to_utf8(banner + kTitles + kTitleJapanese * kTitleBytes, kTitleBytes / 2);
            Info titles;
            split_title(text, titles);
            if (!titles.title.empty()) {
                out.title = titles.title;
                out.subtitle = titles.subtitle;
                out.publisher = titles.publisher;
            }
        }
    }
    fclose(f);
    return ok;
}

}  // namespace

bool read(const std::string& path, Info& out) {
    struct stat st;
    long long size = stat(path.c_str(), &st) == 0 ? (long long)st.st_size : -1;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        auto it = g_cache.find(path);
        if (it != g_cache.end() && it->second.size == size) {
            out = it->second.info;
            return it->second.ok;
        }
    }
    Info info;
    bool ok = size >= 0 && read_uncached(path, info);
    std::lock_guard<std::mutex> lk(g_mx);
    g_cache[path] = {size, ok, info};
    out = std::move(info);
    return ok;
}

}  // namespace rom_info
