#pragma once
// Per-game Action Replay cheats. Codes live in a melonDS .mch file (same format as
// desktop melonDS; the host uses <base>/cheats/<rom>.mch), can be imported from an
// R4-style usrcheat.dat database, and are pushed into the running console (melonDS
// AREngine) whenever they change (except in RetroAchievements hardcore mode, where
// none are applied). UI thread only.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cheats {

struct Category {
    std::string name;
    bool exclusive;  // at most one code in the category may be enabled
};

struct Cheat {
    std::string name;
    std::string description;
    int category;  // index into categories(), or -1 for codes outside any category
    bool enabled;
};

// Called by the host around a game's lifetime (console created, emu thread not started
// yet / already joined). Loads the .mch file if there is one and applies its codes.
void on_game_loaded(const std::string& cheat_file, const std::string& database,
                    uint32_t game_code, uint32_t rom_checksum);
void on_game_unloaded();

const std::vector<Category>& categories();
const std::vector<Cheat>& list();

// Turns a code on or off (turning siblings off in an exclusive category), applies the
// set to the console and saves the .mch file. False if the index is out of range.
bool set_enabled(size_t index, bool enabled);

// usrcheat.dat found next to the BIOS files?
bool database_available();

// usrcheat.dat entries for this game's code (a database can hold several: regions,
// revisions, translations). Only an entry whose checksum matches the ROM header is
// certain to fit; another one's codes may target the wrong addresses. Empty on failure,
// with `message` set.
struct DatabaseEntry {
    std::string name;
    bool checksum_match;
    size_t codes;
};
std::vector<DatabaseEntry> database_entries(std::string& message);

// Replaces this game's codes with entry `index` of database_entries(), all disabled,
// and saves. On failure returns false and sets `message`.
bool import_from_database(size_t index, std::string& message);

// Name of the database entry the current codes were imported from ("" if unknown).
const std::string& imported_from();

// Where this game's .mch file lives (for "put your codes here" hints).
const std::string& file_path();

}  // namespace cheats
