#include "frontend/cheats.h"

#include <ARCodeFile.h>
#include <ARDatabaseDAT.h>
#include <NDS.h>

#include <sys/stat.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <variant>

#include "frontend/achievements.h"
#include "frontend/emu_host.h"

namespace cheats {

namespace {

std::string g_path;
std::string g_db_path;
uint32_t g_game_code = 0;
uint32_t g_checksum = 0;
std::string g_imported_from;  // usrcheat.dat entry of the last import this session

std::unique_ptr<melonDS::ARCodeFile> g_file;
std::vector<Category> g_categories;
std::vector<Cheat> g_list;
// Parallel to g_list / g_categories. ARCodeFile keeps items in std::lists, so these stay
// valid until the file's contents are replaced.
std::vector<melonDS::ARCode*> g_codes;
std::vector<melonDS::ARCodeCat*> g_cats;

bool file_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

// Flattens the file's tree (root codes and one level of categories, which is all the
// .mch format has) into g_list / g_categories.
void rebuild() {
    g_categories.clear();
    g_list.clear();
    g_codes.clear();
    g_cats.clear();
    if (!g_file)
        return;
    for (auto& item : g_file->RootCat.Children) {
        if (auto* cat = std::get_if<melonDS::ARCodeCat>(&item)) {
            int index = int(g_categories.size());
            g_categories.push_back({cat->Name, cat->OnlyOneCodeEnabled});
            g_cats.push_back(cat);
            for (auto& child : cat->Children) {
                if (auto* code = std::get_if<melonDS::ARCode>(&child)) {
                    g_list.push_back({code->Name, code->Description, index, code->Enabled});
                    g_codes.push_back(code);
                }
            }
        } else if (auto* code = std::get_if<melonDS::ARCode>(&item)) {
            g_list.push_back({code->Name, code->Description, -1, code->Enabled});
            g_codes.push_back(code);
        }
    }
}

// RetroAchievements hardcore mode forbids cheats: codes stay editable but aren't applied.
void apply() {
    std::vector<melonDS::ARCode> codes;
    if (g_file && !achievements::hardcore_active())
        codes = g_file->GetCodes();
    host::with_console([&codes](melonDS::NDS& nds) { nds.AREngine.Cheats = std::move(codes); });
}

void save() {
    if (g_file && !g_file->Save())
        fprintf(stderr, "[cheats] failed to write %s\n", g_path.c_str());
}

}  // namespace

void on_game_loaded(const std::string& cheat_file, const std::string& database,
                    uint32_t game_code, uint32_t rom_checksum) {
    g_path = cheat_file;
    g_db_path = database;
    g_game_code = game_code;
    g_checksum = rom_checksum;
    g_file = std::make_unique<melonDS::ARCodeFile>(g_path);
    if (g_file->Error && file_exists(g_path))
        fprintf(stderr, "[cheats] could not parse %s\n", g_path.c_str());
    rebuild();
    apply();
    fprintf(stderr, "[cheats] %zu codes from %s\n", g_list.size(), g_path.c_str());
}

void on_game_unloaded() {
    g_imported_from.clear();
    g_file.reset();
    rebuild();
    g_path.clear();
}

const std::vector<Category>& categories() {
    return g_categories;
}

const std::vector<Cheat>& list() {
    return g_list;
}

bool set_enabled(size_t index, bool enabled) {
    if (index >= g_list.size())
        return false;
    int cat = g_list[index].category;
    if (enabled && cat >= 0 && g_categories[cat].exclusive) {
        for (size_t i = 0; i < g_list.size(); ++i) {
            if (i != index && g_list[i].category == cat) {
                g_list[i].enabled = false;
                g_codes[i]->Enabled = false;
            }
        }
    }
    g_list[index].enabled = enabled;
    g_codes[index]->Enabled = enabled;
    apply();
    save();
    return true;
}

bool database_available() {
    return !g_db_path.empty() && file_exists(g_db_path);
}

namespace {

size_t count_codes(const melonDS::ARCodeCat& cat) {
    size_t n = 0;
    for (const auto& item : cat.Children)
        n += std::holds_alternative<melonDS::ARCodeCat>(item) ? count_codes(std::get<melonDS::ARCodeCat>(item)) : 1;
    return n;
}

bool load_entries(melonDS::ARDatabaseEntryList& out, std::string& message) {
    if (!g_file) {
        message = "No game is running.";
        return false;
    }
    melonDS::ARDatabaseDAT db(g_db_path);
    if (db.Error) {
        message = "Could not read " + g_db_path + ".";
        return false;
    }
    out = db.GetEntriesByGameCode(g_game_code);
    if (out.empty()) {
        message = "usrcheat.dat has no cheats for this game.";
        return false;
    }
    return true;
}

}  // namespace

std::vector<DatabaseEntry> database_entries(std::string& message) {
    std::vector<DatabaseEntry> out;
    melonDS::ARDatabaseEntryList entries;
    if (!load_entries(entries, message))
        return out;
    char code[5] = {};
    std::memcpy(code, &g_game_code, 4);
    fprintf(stderr, "[cheats] usrcheat.dat: %zu entries for %s, ROM checksum %08X\n", entries.size(), code,
            g_checksum);
    for (const auto& e : entries) {
        size_t n = count_codes(e.RootCat);
        fprintf(stderr, "[cheats]   \"%s\" checksum %08X%s, %zu codes\n", e.Name.c_str(), e.Checksum,
                e.Checksum == g_checksum ? " (matches)" : "", n);
        out.push_back({e.Name, e.Checksum == g_checksum, n});
    }
    return out;
}

bool import_from_database(size_t index, std::string& message) {
    melonDS::ARDatabaseEntryList entries;
    if (!load_entries(entries, message))
        return false;
    if (index >= entries.size()) {
        message = "That database entry is gone.";
        return false;
    }
    melonDS::ARDatabaseEntry& pick = entries[index];

    melonDS::ARCodeEnableMap all;  // empty: import everything
    g_file->Import(pick, all, true);
    g_file->Error = false;  // the file may not have existed before
    rebuild();
    for (size_t i = 0; i < g_list.size(); ++i) {  // start with everything off
        g_list[i].enabled = false;
        g_codes[i]->Enabled = false;
    }
    apply();
    save();
    g_imported_from = pick.Name;
    fprintf(stderr, "[cheats] imported %zu codes from \"%s\"%s\n", g_list.size(), pick.Name.c_str(),
            pick.Checksum == g_checksum ? "" : " (checksum differs from the ROM)");
    return true;
}

const std::string& imported_from() {
    return g_imported_from;
}

const std::string& file_path() {
    return g_path;
}

}  // namespace cheats
