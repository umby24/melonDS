// melonds-next — app lifecycle. borealis owns the window and the main loop; games run
// inside ui::GameView (see ui/game.h).

#include <arpa/inet.h>
#include <switch.h>
#include <sys/stat.h>
#include <unistd.h>

#include <borealis.hpp>

#include <cstdio>
#include <exception>
#include <string>

#include "frontend/achievements.h"
#include "frontend/emu_host.h"
#include "frontend/options.h"
#include "frontend/thumbnails.h"
#include "platform/input.h"
#include "ui/main_activity.h"

namespace {

constexpr const char* kAppTitle = "melonds-next";
constexpr const char* kBaseDir = "sdmc:/switch/melonds-next";
constexpr const char* kOldBaseDir = "sdmc:/switch/desume";  // pre-rename data dir

// Launched via `nxlink -s`: stdout + stderr are streamed to the PC (borealis'
// switch_wrapper.c has already initialized sockets). Otherwise stderr (our logs) goes to
// log.txt on the SD.
int g_nxlink_fd = -1;

// Carry saves/states/config over from before the rename (only if the new dir is absent).
void migrate_old_data_dir() {
    struct stat st;
    if (stat(kBaseDir, &st) != 0 && stat(kOldBaseDir, &st) == 0 && S_ISDIR(st.st_mode))
        rename(kOldBaseDir, kBaseDir);
}

void init_logging() {
    if (__nxlink_host.s_addr != 0) {
        g_nxlink_fd = nxlinkStdio();
        if (g_nxlink_fd >= 0)
            return;
    }
    mkdir(kBaseDir, 0777);
    if (freopen("sdmc:/switch/melonds-next/log.txt", "w", stderr))
        setvbuf(stderr, nullptr, _IONBF, 0);  // unbuffered: survive a crash
}

// An uncaught exception otherwise ends the app silently (libnx's abort runs the normal
// exit path, so there is no crash report).
void log_terminate() {
    const char* what = "unknown exception";
    try {
        if (auto e = std::current_exception())
            std::rethrow_exception(e);
        what = "std::terminate without an exception";
    } catch (const std::exception& e) {
        fprintf(stderr, "[main] uncaught exception: %s\n", e.what());
        fflush(stdout);
        std::abort();
    } catch (...) {
    }
    fprintf(stderr, "[main] %s\n", what);
    fflush(stdout);
    std::abort();
}

void shutdown_logging() {
    if (g_nxlink_fd >= 0)
        close(g_nxlink_fd);
}

}  // namespace

int main(int, char**) {
    migrate_old_data_dir();
    init_logging();
    std::set_terminate(log_terminate);
    fprintf(stderr, "[main] start\n");

    host::Paths paths;
    paths.base = kBaseDir;
    paths.system = paths.base + "/system";
    paths.saves = paths.base + "/saves";
    paths.states = paths.base + "/states";
    paths.cheats = paths.base + "/cheats";
    paths.config = paths.base + "/config.ini";

    if (!brls::Application::init()) {
        fprintf(stderr, "[main] borealis init failed\n");
        return 1;
    }
    brls::Application::createWindow(kAppTitle);
    brls::Application::getPlatform()->setThemeVariant(brls::ThemeVariant::DARK);
    brls::Application::setGlobalQuit(false);

    if (!input::init() || !host::init(paths))
        return 1;
    brls::Application::setRotation(std::stoi(options::get("ui_rotation")) / 90);

    // RetroAchievements messages come from the network and emulation threads.
    achievements::set_notifier([](const std::string& text) {
        brls::sync([text] { brls::Application::notify(text); });
    });
    achievements::init(paths.base + "/retroachievements.txt");
    thumbnails::init(paths.base + "/thumbnails");

    brls::Application::pushActivity(ui::make_main_activity(kAppTitle));
    while (brls::Application::mainLoop())
        ;

    host::shutdown();
    thumbnails::shutdown();
    achievements::set_notifier(nullptr);
    achievements::shutdown();
    input::shutdown();
    shutdown_logging();
    return 0;
}
