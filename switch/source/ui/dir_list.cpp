#include "ui/dir_list.h"

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

#include <algorithm>

namespace ui {

namespace {

bool has_nds_ext(const std::string& name) {
    return name.size() > 4 && strcasecmp(name.c_str() + name.size() - 4, ".nds") == 0;
}

}  // namespace

bool is_dir(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string join_path(const std::string& dir, const std::string& name) {
    return !dir.empty() && dir.back() == '/' ? dir + name : dir + "/" + name;
}

std::string parent_path(const std::string& dir) {
    std::string d = dir;
    while (d.size() > 1 && d.back() == '/' && d[d.size() - 2] != ':')
        d.pop_back();
    size_t slash = d.find_last_of('/');
    if (slash == std::string::npos)
        return d;
    if (slash > 0 && d[slash - 1] == ':')
        return d.substr(0, slash + 1);  // keep "sdmc:/"
    return d.substr(0, slash);
}

bool is_root(const std::string& dir) {
    return parent_path(dir) == dir;
}

std::vector<DirEntry> list_dir(const std::string& dir, bool with_nds) {
    std::vector<DirEntry> out;
    DIR* d = opendir(dir.c_str());
    if (!d)
        return out;
    while (dirent* e = readdir(d)) {
        std::string name = e->d_name;
        if (name.empty() || name[0] == '.')
            continue;
        bool dir_entry = e->d_type == DT_DIR;
        if (e->d_type == DT_UNKNOWN)
            dir_entry = is_dir(join_path(dir, name));
        if (dir_entry || (with_nds && has_nds_ext(name)))
            out.push_back({name, dir_entry});
    }
    closedir(d);
    std::sort(out.begin(), out.end(), [](const DirEntry& a, const DirEntry& b) {
        if (a.is_dir != b.is_dir)
            return a.is_dir;
        return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return out;
}

}  // namespace ui
