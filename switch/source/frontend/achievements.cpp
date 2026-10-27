#include "frontend/achievements.h"

#include <NDS.h>

extern "C" {
#include <rc_client.h>
#include <rc_consoles.h>
#include <rc_hash.h>
}

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "platform/http.h"

namespace achievements {

namespace {

constexpr const char* kUserAgent = "melonds-next/0.1.0 (Nintendo Switch)";

rc_client_t* g_client = nullptr;
std::string g_token_file;
std::atomic<bool> g_hardcore{false};
std::atomic<bool> g_busy{false};

std::mutex g_notify_mx;
std::function<void(const std::string&)> g_notify;

// Console memory for read_memory. rc_client also reads from the network thread while a
// game finishes loading, so the pointers are guarded against the console going away.
std::mutex g_mem_mx;
uint8_t* g_main_ram = nullptr;
uint32_t g_main_ram_size = 0;
uint8_t* g_dtcm = nullptr;
bool g_game_ready = false;  // RetroAchievements recognised the game

void notify(const std::string& text) {
    fprintf(stderr, "[ra] %s\n", text.c_str());
    std::lock_guard<std::mutex> lk(g_notify_mx);
    if (g_notify)
        g_notify(text);
}

// DS map: 0x000000-0x3FFFFF main RAM (0x02000000), 0x400000-0xFFFFFF DSi-only RAM,
// 0x1000000-0x1003FFF data TCM. DSi: main RAM covers 0x000000-0xFFFFFF.
uint32_t read_memory(uint32_t address, uint8_t* buffer, uint32_t num_bytes, rc_client_t*) {
    std::lock_guard<std::mutex> lk(g_mem_mx);
    if (!g_main_ram)
        return 0;
    for (uint32_t i = 0; i < num_bytes; ++i) {
        uint32_t a = address + i;
        if (a < 0x1000000)
            buffer[i] = a < g_main_ram_size ? g_main_ram[a] : 0;
        else if (a < 0x1000000 + melonDS::DTCMPhysicalSize && g_dtcm)
            buffer[i] = g_dtcm[a - 0x1000000];
        else
            return i;
    }
    return num_bytes;
}

void server_call(const rc_api_request_t* request, rc_client_server_callback_t callback, void* callback_data,
                 rc_client_t*) {
    http::request(request->url, request->post_data ? request->post_data : "",
                  request->content_type ? request->content_type : "",
                  [callback, callback_data](const http::Response& rs) {
                      rc_api_server_response_t response = {};
                      response.body = rs.body.c_str();
                      response.body_length = rs.body.size();
                      response.http_status_code =
                          rs.status >= 0 ? rs.status : RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
                      callback(&response, callback_data);
                  });
}

void log_message(const char* message, const rc_client_t*) {
    fprintf(stderr, "[ra] %s\n", message);
}

std::string game_title() {
    const rc_client_game_t* game = rc_client_get_game_info(g_client);
    return game && game->title ? game->title : "";
}

void event_handler(const rc_client_event_t* event, rc_client_t*) {
    switch (event->type) {
    case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
        notify("Achievement unlocked: " + std::string(event->achievement->title) + " (" +
               std::to_string(event->achievement->points) + " points)");
        break;
    case RC_CLIENT_EVENT_GAME_COMPLETED:
        notify((g_hardcore ? "Mastered " : "Completed ") + game_title() + "!");
        break;
    case RC_CLIENT_EVENT_LEADERBOARD_STARTED:
        notify("Leaderboard attempt started: " + std::string(event->leaderboard->title));
        break;
    case RC_CLIENT_EVENT_LEADERBOARD_FAILED:
        notify("Leaderboard attempt failed: " + std::string(event->leaderboard->title));
        break;
    case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
        notify("Submitted " + std::string(event->leaderboard->tracker_value) + " for " +
               event->leaderboard->title);
        break;
    case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_SHOW:
        notify(std::string(event->achievement->title) + ": " + event->achievement->measured_progress);
        break;
    case RC_CLIENT_EVENT_SERVER_ERROR:
        notify("RetroAchievements error: " + std::string(event->server_error->error_message
                                                             ? event->server_error->error_message : "unknown"));
        break;
    case RC_CLIENT_EVENT_DISCONNECTED:
        notify("RetroAchievements: connection lost; unlocks will be sent when it's back");
        break;
    case RC_CLIENT_EVENT_RECONNECTED:
        notify("RetroAchievements: reconnected");
        break;
    case RC_CLIENT_EVENT_RESET:
        // Only raised when hardcore is enabled mid-game, which we never do.
        break;
    default:
        break;
    }
}

void write_token(const std::string& user, const std::string& token) {
    if (user.empty()) {
        remove(g_token_file.c_str());
        return;
    }
    if (FILE* f = fopen(g_token_file.c_str(), "w")) {
        fprintf(f, "%s\n%s\n", user.c_str(), token.c_str());
        fclose(f);
    }
}

bool read_token(std::string& user, std::string& token) {
    FILE* f = fopen(g_token_file.c_str(), "r");
    if (!f)
        return false;
    char u[128] = {}, t[128] = {};
    bool ok = fgets(u, sizeof(u), f) && fgets(t, sizeof(t), f);
    fclose(f);
    if (!ok)
        return false;
    user = u;
    token = t;
    while (!user.empty() && (user.back() == '\n' || user.back() == '\r')) user.pop_back();
    while (!token.empty() && (token.back() == '\n' || token.back() == '\r')) token.pop_back();
    return !user.empty() && !token.empty();
}

struct LoginRequest {
    std::function<void(bool, const std::string&)> done;
    bool quiet_failure;  // token login at startup: don't nag if offline
};

void login_callback(int result, const char* error_message, rc_client_t* client, void* userdata) {
    auto* rq = static_cast<LoginRequest*>(userdata);
    g_busy = false;
    std::string message;
    bool ok = result == RC_OK;
    if (ok) {
        const rc_client_user_t* user = rc_client_get_user_info(client);
        write_token(user->username, user->token);
        message = "Logged in to RetroAchievements as " + std::string(user->display_name) + " (" +
                  std::to_string(g_hardcore ? user->score : user->score_softcore) + " points)";
        notify(message);
    } else {
        message = "RetroAchievements login failed: " + std::string(error_message ? error_message : "unknown error");
        if (result == RC_INVALID_CREDENTIALS || result == RC_EXPIRED_TOKEN)
            write_token("", "");  // the stored token is no good any more
        if (!rq->quiet_failure || result == RC_INVALID_CREDENTIALS || result == RC_EXPIRED_TOKEN)
            notify(message);
        else
            fprintf(stderr, "[ra] %s\n", message.c_str());
    }
    if (rq->done)
        rq->done(ok, message);
    delete rq;
}

void load_callback(int result, const char* error_message, rc_client_t* client, void*) {
    if (result != RC_OK) {
        // Unknown games are common (homebrew, hacks, unsupported dumps); keep it short.
        if (result == RC_NO_GAME_LOADED)
            fprintf(stderr, "[ra] game not recognised (hash not in the RetroAchievements database)\n");
        else
            notify("RetroAchievements: " + std::string(error_message ? error_message : "could not load the game"));
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_mem_mx);
        g_game_ready = true;
    }
    rc_client_user_game_summary_t summary;
    rc_client_get_user_game_summary(client, &summary);
    if (summary.num_core_achievements == 0)
        notify(game_title() + ": no achievements yet");
    else
        notify(game_title() + ": " + std::to_string(summary.num_unlocked_achievements) + " of " +
               std::to_string(summary.num_core_achievements) + " achievements unlocked" +
               (rc_client_get_hardcore_enabled(client) ? " (hardcore)" : ""));
}

}  // namespace

void init(const std::string& token_file) {
    if (g_client)
        return;
    g_token_file = token_file;
    char clause[64] = {};
    std::string agent = kUserAgent;
    g_client = rc_client_create(read_memory, server_call);
    if (!g_client)
        return;
    if (rc_client_get_user_agent_clause(g_client, clause, sizeof(clause)) > 0)
        agent += std::string(" ") + clause;
    http::init(agent);
    rc_client_enable_logging(g_client, RC_CLIENT_LOG_LEVEL_WARN, log_message);
    rc_client_set_event_handler(g_client, event_handler);
    rc_client_set_hardcore_enabled(g_client, g_hardcore ? 1 : 0);

    std::string user, token;
    if (read_token(user, token)) {
        g_busy = true;
        rc_client_begin_login_with_token(g_client, user.c_str(), token.c_str(), login_callback,
                                         new LoginRequest{nullptr, true});
    }
}

void shutdown() {
    http::shutdown();  // no callbacks after this
    if (g_client) {
        rc_client_destroy(g_client);
        g_client = nullptr;
    }
}

void set_notifier(std::function<void(const std::string&)> notify_fn) {
    std::lock_guard<std::mutex> lk(g_notify_mx);
    g_notify = std::move(notify_fn);
}

Status status() {
    Status s;
    if (!g_client)
        return s;
    s.busy = g_busy;
    if (const rc_client_user_t* user = rc_client_get_user_info(g_client)) {
        s.logged_in = true;
        s.user = user->display_name ? user->display_name : user->username;
        s.score = g_hardcore ? user->score : user->score_softcore;
    }
    {
        std::lock_guard<std::mutex> lk(g_mem_mx);
        s.game_loaded = g_game_ready;
    }
    if (s.game_loaded) {
        s.game_title = game_title();
        rc_client_user_game_summary_t summary;
        rc_client_get_user_game_summary(g_client, &summary);
        s.unlocked = summary.num_unlocked_achievements;
        s.total = summary.num_core_achievements;
    }
    return s;
}

void login(const std::string& user, const std::string& password,
           std::function<void(bool, const std::string&)> done) {
    if (!g_client) {
        if (done)
            done(false, "RetroAchievements is unavailable");
        return;
    }
    g_busy = true;
    rc_client_begin_login_with_password(g_client, user.c_str(), password.c_str(), login_callback,
                                        new LoginRequest{std::move(done), false});
}

void logout() {
    if (!g_client)
        return;
    rc_client_logout(g_client);
    write_token("", "");
    notify("Logged out of RetroAchievements");
}

void set_hardcore(bool enabled) {
    g_hardcore = enabled;
    // With a game running this only takes effect at the next load (see on_game_loaded);
    // switching mid-game would require a console reset.
    if (g_client && !rc_client_is_game_loaded(g_client))
        rc_client_set_hardcore_enabled(g_client, enabled ? 1 : 0);
}

bool hardcore_active() {
    return g_client && g_hardcore && rc_client_get_user_info(g_client) != nullptr;
}

std::string hash_rom(const uint8_t* rom, size_t rom_size) {
    char hash[33] = {};
    // DS and DSi titles hash the same way (header, ARM9/ARM7 code, banner).
    if (!rc_hash_generate_from_buffer(hash, RC_CONSOLE_NINTENDO_DS, rom, rom_size))
        return "";
    return hash;
}

void on_game_loaded(melonDS::NDS& nds, const std::string& hash) {
    if (!g_client)
        return;
    {
        std::lock_guard<std::mutex> lk(g_mem_mx);
        g_main_ram = nds.MainRAM;
        g_main_ram_size = std::min<uint32_t>(nds.MainRAMMask + 1, 0x1000000);
        g_dtcm = nds.ARM9.DTCM;
        g_game_ready = false;
    }
    if ((!rc_client_get_user_info(g_client) && !g_busy) || hash.empty())
        return;  // not logged in (a login still in flight queues ahead of this load)
    fprintf(stderr, "[ra] ROM hash %s\n", hash.c_str());
    rc_client_set_hardcore_enabled(g_client, g_hardcore ? 1 : 0);
    rc_client_begin_load_game(g_client, hash.c_str(), load_callback, nullptr);
}

void on_game_unloaded() {
    if (g_client)
        rc_client_unload_game(g_client);
    std::lock_guard<std::mutex> lk(g_mem_mx);
    g_main_ram = nullptr;
    g_main_ram_size = 0;
    g_dtcm = nullptr;
    g_game_ready = false;
}

void do_frame() {
    if (g_client)
        rc_client_do_frame(g_client);
}

void idle() {
    if (g_client)
        rc_client_idle(g_client);
}

std::vector<uint8_t> serialize() {
    std::vector<uint8_t> out;
    if (!g_client || !rc_client_is_game_loaded(g_client))
        return out;
    out.resize(rc_client_progress_size(g_client));
    if (out.empty() || rc_client_serialize_progress_sized(g_client, out.data(), out.size()) != RC_OK)
        out.clear();
    return out;
}

void deserialize(const std::vector<uint8_t>& data) {
    if (!g_client || !rc_client_is_game_loaded(g_client))
        return;
    rc_client_deserialize_progress_sized(g_client, data.empty() ? nullptr : data.data(), data.size());
}

std::vector<Entry> list() {
    std::vector<Entry> out;
    if (!g_client || !rc_client_is_game_loaded(g_client))
        return out;
    rc_client_achievement_list_t* l = rc_client_create_achievement_list(
        g_client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_PROGRESS);
    if (!l)
        return out;
    for (uint32_t b = 0; b < l->num_buckets; ++b) {
        const rc_client_achievement_bucket_t& bucket = l->buckets[b];
        for (uint32_t i = 0; i < bucket.num_achievements; ++i) {
            const rc_client_achievement_t* a = bucket.achievements[i];
            Entry e;
            e.title = a->title ? a->title : "";
            e.description = a->description ? a->description : "";
            e.points = a->points;
            e.unlocked = a->unlocked != 0;
            if (bucket.bucket_type == RC_CLIENT_ACHIEVEMENT_BUCKET_UNSUPPORTED)
                e.state = "Unsupported";
            else if (e.unlocked)
                e.state = "Unlocked";
            else if (a->measured_progress[0])
                e.state = a->measured_progress;
            else
                e.state = "Locked";
            out.push_back(std::move(e));
        }
    }
    rc_client_destroy_achievement_list(l);
    return out;
}

}  // namespace achievements
