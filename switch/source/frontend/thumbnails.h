#pragma once
// Cover art for the ROM grid, downloaded from GameTDB (art.gametdb.com), which keys DS
// art by the header's 4-character game code, so it works whatever the files are named.
// Files: <dir>/<type>/<CODE>.png. One download job at a time, on its own thread.

#include <string>

namespace thumbnails {

void init(const std::string& dir);  // UI thread, at startup
void shutdown();                    // cancels and joins a running job

// Art type from option `thumb_type` ("box" or "coverS"). "" for codes that can't have art
// (homebrew: "####", blanks).
std::string path_for(const std::string& type, const std::string& game_code);

// Downloads art for every .nds under `rom_dir` (subfolders included) that has none yet,
// or for all of them with `overwrite`. No-op if a job is running.
void start(const std::string& rom_dir, const std::string& type, bool overwrite);
void cancel();

struct Progress {
    bool running = false;
    int scanned = 0, total = 0;       // ROMs handled / found
    int downloaded = 0, missing = 0;  // missing: GameTDB has no art for that code
    int skipped = 0, failed = 0;      // skipped: already present or no game code
    std::string current;              // ROM being handled
    std::string error;                // last network error
};
Progress progress();  // any thread

}  // namespace thumbnails
