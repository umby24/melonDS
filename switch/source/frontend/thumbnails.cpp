#include "frontend/thumbnails.h"

#include <dirent.h>
#include <sys/stat.h>

#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "frontend/rom_info.h"
#include "platform/http.h"

namespace thumbnails {

namespace {

constexpr const char* kBaseUrl = "https://art.gametdb.com/ds/";

std::string g_dir;
std::mutex g_mx;
Progress g_progress;
std::thread g_thread;
std::atomic<bool> g_cancel{false};

bool exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

bool ends_with_nds(const std::string& name) {
    if (name.size() < 4)
        return false;
    std::string ext = name.substr(name.size() - 4);
    for (char& c : ext)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".nds";
}

void find_roms(const std::string& dir, std::vector<std::string>& out, int depth = 0) {
    if (depth > 4)
        return;
    DIR* d = opendir(dir.c_str());
    if (!d)
        return;
    std::vector<std::string> subdirs;
    while (dirent* e = readdir(d)) {
        std::string name = e->d_name;
        if (name.empty() || name[0] == '.')
            continue;
        std::string path = dir + (dir.back() == '/' ? "" : "/") + name;
        bool is_dir = e->d_type == DT_DIR;
        if (e->d_type == DT_UNKNOWN) {
            struct stat st;
            is_dir = stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
        }
        if (is_dir)
            subdirs.push_back(path);
        else if (ends_with_nds(name))
            out.push_back(path);
    }
    closedir(d);
    for (const std::string& s : subdirs)
        find_roms(s, out, depth + 1);
}

// GameTDB region folders to try, from the game code's region letter.
std::vector<const char*> regions_for(char region) {
    switch (region) {
    case 'E': return {"US", "EN"};
    case 'J': return {"JA", "US"};
    case 'K': return {"KO", "US"};
    case 'P': case 'X': case 'Y': case 'Z': case 'V': return {"EN", "US", "FR", "DE"};
    case 'F': return {"FR", "EN"};
    case 'D': return {"DE", "EN"};
    case 'S': return {"ES", "EN"};
    case 'I': return {"IT", "EN"};
    case 'H': return {"NL", "EN"};
    case 'U': return {"AU", "EN", "US"};
    case 'C': return {"ZH", "US"};
    default:  return {"US", "EN", "JA"};
    }
}

bool make_dirs(const std::string& path) {
    for (size_t i = path.find('/', path.find(":/") + 2); i != std::string::npos; i = path.find('/', i + 1))
        mkdir(path.substr(0, i).c_str(), 0777);
    return mkdir(path.c_str(), 0777) == 0 || errno == EEXIST;
}

bool write_file(const std::string& path, const std::string& data) {
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f)
        return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = fclose(f) == 0 && ok;
    if (ok) {
        remove(path.c_str());
        ok = rename(tmp.c_str(), path.c_str()) == 0;
    }
    if (!ok)
        remove(tmp.c_str());
    return ok;
}

void update(const std::function<void(Progress&)>& fn) {
    std::lock_guard<std::mutex> lk(g_mx);
    fn(g_progress);
}

void run(std::string rom_dir, std::string type, bool overwrite) {
    std::vector<std::string> roms;
    find_roms(rom_dir, roms);
    update([&](Progress& p) { p.total = int(roms.size()); });
    make_dirs(g_dir + "/" + type);

    for (const std::string& rom : roms) {
        if (g_cancel)
            break;
        update([&](Progress& p) { p.current = rom.substr(rom.find_last_of('/') + 1); });
        rom_info::Info info;
        std::string out = rom_info::read(rom, info) ? path_for(type, info.game_code) : "";
        if (out.empty() || (!overwrite && exists(out))) {
            update([](Progress& p) { ++p.skipped; ++p.scanned; });
            continue;
        }
        bool got = false, failed = false;
        std::string error;
        for (const char* region : regions_for(info.game_code[3])) {
            http::Response rs = http::get(std::string(kBaseUrl) + type + "/" + region + "/" + info.game_code + ".png");
            if (rs.status == 200 && rs.body.size() > 8 && rs.body.compare(1, 3, "PNG") == 0) {
                got = write_file(out, rs.body);
                failed = !got;
                if (!got)
                    error = "could not write " + out;
                break;
            }
            if (rs.status < 0) {  // network trouble, not missing art
                failed = true;
                error = rs.error;
                break;
            }
        }
        update([&](Progress& p) {
            ++p.scanned;
            if (got) ++p.downloaded;
            else if (failed) ++p.failed;
            else ++p.missing;
            if (!error.empty()) p.error = error;
        });
    }
    update([](Progress& p) {
        p.running = false;
        p.current.clear();
    });
}

}  // namespace

void init(const std::string& dir) {
    g_dir = dir;
    make_dirs(dir);
}

void shutdown() {
    cancel();
    if (g_thread.joinable())
        g_thread.join();
}

std::string path_for(const std::string& type, const std::string& code) {
    if (code.size() != 4)
        return "";
    for (char c : code)
        if (!std::isalnum(static_cast<unsigned char>(c)))
            return "";
    return g_dir + "/" + type + "/" + code + ".png";
}

void start(const std::string& rom_dir, const std::string& type, bool overwrite) {
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (g_progress.running)
            return;
        g_progress = Progress{};
        g_progress.running = true;
    }
    if (g_thread.joinable())
        g_thread.join();  // the previous job has finished (running was false)
    g_cancel = false;
    g_thread = std::thread(run, rom_dir, type, overwrite);
}

void cancel() {
    g_cancel = true;
}

Progress progress() {
    std::lock_guard<std::mutex> lk(g_mx);
    return g_progress;
}

}  // namespace thumbnails
