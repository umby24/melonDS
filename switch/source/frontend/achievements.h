#pragma once
// RetroAchievements through rcheevos' rc_client. Server calls go through platform/http
// (a worker thread); achievement processing runs on the emulation thread once per
// frame. Messages for the player (login, unlocks, errors) go to the notifier, from
// whichever thread produced them.
//
// The login token (never the password) is kept in a small file of its own, so
// config.ini can be shared without it.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace melonDS {
class NDS;
}

namespace achievements {

// UI thread, at startup: creates the client and logs in with a stored token, if any.
void init(const std::string& token_file);
void shutdown();

// Shows `text` to the player. Called from any thread.
void set_notifier(std::function<void(const std::string& text)> notify);

struct Status {
    bool logged_in = false;
    bool busy = false;        // a login is in flight
    std::string user;         // display name
    unsigned score = 0;       // hardcore points (softcore when hardcore is off)
    bool game_loaded = false; // the running game is known to RetroAchievements
    std::string game_title;
    unsigned unlocked = 0, total = 0;
};
Status status();  // any thread

// UI thread. `done` runs on the network thread when the attempt finishes.
void login(const std::string& user, const std::string& password,
           std::function<void(bool ok, const std::string& message)> done);
void logout();

// Hardcore mode (applied to the next game). While on and logged in, cheats are not
// applied and save states can't be loaded.
void set_hardcore(bool enabled);
bool hardcore_active();

// RetroAchievements' identifier for a ROM image, as read from the file. Must be taken
// before the ROM goes to melonDS, which re-encrypts the secure area of its copy in place
// (that changes the hash). "" if it can't be computed.
std::string hash_rom(const uint8_t* rom, size_t rom_size);

// Host, around a game's lifetime (emu thread not running).
void on_game_loaded(melonDS::NDS& nds, const std::string& hash);
void on_game_unloaded();

void do_frame();  // emu thread, after each emulated frame
void idle();      // emu thread, about once a second while paused

// Achievement progress to keep alongside a save state ({} without a game).
std::vector<uint8_t> serialize();
void deserialize(const std::vector<uint8_t>& data);  // empty: progress as at game start

struct Entry {
    std::string title, description;
    std::string state;  // "Unlocked", "Locked", progress like "3/10", "Unsupported"
    unsigned points;
    bool unlocked;
};
std::vector<Entry> list();  // the running game's achievements; any thread

}  // namespace achievements
