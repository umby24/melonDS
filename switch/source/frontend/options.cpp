#include "frontend/options.h"

#include <algorithm>
#include <cstdio>
#include <map>

namespace options {

namespace {

std::vector<Option> g_options;
std::map<std::string, std::string> g_loaded;     // values read from config.ini or set()
bool g_dirty = false;

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

Option* find(const std::string& key) {
    for (auto& o : g_options)
        if (o.key == key)
            return &o;
    return nullptr;
}

bool allowed(const Option& o, const std::string& v) {
    if (o.free_form)
        return !v.empty();
    return std::find(o.values.begin(), o.values.end(), v) != o.values.end();
}

}  // namespace

void define(const std::string& key, const std::string& description,
            const std::vector<std::string>& values,
            const std::vector<std::string>& labels) {
    Option o;
    o.key = key;
    o.description = description;
    o.values = values;
    o.labels = labels.size() == values.size() ? labels : values;
    o.current = values.empty() ? "" : values[0];
    auto ld = g_loaded.find(key);
    if (ld != g_loaded.end() && allowed(o, ld->second))
        o.current = ld->second;

    if (Option* existing = find(key))
        *existing = o;
    else
        g_options.push_back(o);
    g_dirty = true;
}

void define_text(const std::string& key, const std::string& description,
                 const std::string& default_value) {
    define(key, description, {default_value});
    Option* o = find(key);
    o->free_form = true;
    auto ld = g_loaded.find(key);
    if (ld != g_loaded.end() && allowed(*o, ld->second))
        o->current = ld->second;
}

const std::string& get(const std::string& key) {
    static const std::string kEmpty;
    Option* o = find(key);
    return o ? o->current : kEmpty;
}

bool is(const std::string& key, const std::string& value) {
    return get(key) == value;
}

bool set(const std::string& key, const std::string& value) {
    Option* o = find(key);
    if (!o || !allowed(*o, value))
        return false;
    if (o->current != value) {
        o->current = value;
        g_loaded[key] = value;  // user choice from now on; persisted by save()
        g_dirty = true;
    }
    return true;
}

bool consume_dirty() {
    bool d = g_dirty;
    g_dirty = false;
    return d;
}

const std::vector<Option>& all() {
    return g_options;
}

bool load(const std::string& path) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f)
        return false;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        std::string s = trim(line);
        if (s.empty() || s[0] == '#' || s[0] == ';')
            continue;
        size_t eq = s.find('=');
        if (eq == std::string::npos)
            continue;
        std::string key = trim(s.substr(0, eq));
        std::string value = trim(s.substr(eq + 1));
        g_loaded[key] = value;
        if (Option* o = find(key))
            if (allowed(*o, value))
                o->current = value;
    }
    fclose(f);
    return true;
}

// Writes user-chosen values; options still at their default are written commented out, so
// the file documents what exists without pinning defaults that may change between versions.
bool save(const std::string& path) {
    if (g_options.empty())
        return false;
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f)
        return false;
    for (const auto& o : g_options) {
        bool user_set = g_loaded.count(o.key) != 0;
        fprintf(f, "# %s (%s)\n%s%s=%s\n", o.description.c_str(),
                o.values.empty() ? "" : o.values[0].c_str(), user_set ? "" : "# ",
                o.key.c_str(), o.current.c_str());
    }
    bool ok = fclose(f) == 0;
    if (ok) {
        remove(path.c_str());
        ok = rename(tmp.c_str(), path.c_str()) == 0;
    }
    return ok;
}

}  // namespace options
