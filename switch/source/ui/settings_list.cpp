#include "ui/settings_list.h"

#include <borealis.hpp>

#include <algorithm>

#include "frontend/emu_host.h"
#include "frontend/options.h"
#include "ui/folder_picker.h"

namespace ui {

void add_option_cells(brls::Box* box, const std::vector<std::string>& keys) {
    for (const std::string& key : keys) {
        const auto& all = options::all();
        auto it = std::find_if(all.begin(), all.end(), [&](const options::Option& o) { return o.key == key; });
        if (it == all.end())
            continue;
        const options::Option& o = *it;
        int selected = int(std::find(o.values.begin(), o.values.end(), o.current) - o.values.begin());

        auto* cell = new brls::SelectorCell();
        std::vector<std::string> values = o.values;
        cell->init(o.description, o.labels, selected, [](int) {}, [key, values](int i) {
            if (i >= 0 && i < int(values.size()) && host::set_option(key, values[i]))
                apply_ui_option(key);
        });
        box->addView(cell);
    }
}

void apply_ui_option(const std::string& key) {
    if (key == "ui_rotation") {
        int turns = std::stoi(options::get("ui_rotation")) / 90;
        // After this frame: the whole UI is re-laid out, including the cell that called us.
        brls::sync([turns] { brls::Application::setRotation(turns); });
    }
}

namespace {

void add_header(brls::Box* list, const std::string& text) {
    auto* header = new brls::Header();
    header->setTitle(text);
    list->addView(header);
}

}  // namespace

brls::View* make_settings_list() {
    auto* list = new brls::Box(brls::Axis::COLUMN);
    list->setPadding(40, 60, 40, 60);

    add_header(list, "General");

    // Free-form options get their own editors; only the ROM folder exists so far.
    auto* rom_dir = new brls::DetailCell();
    rom_dir->setText("ROM folder");
    rom_dir->setDetailText(options::get("rom_dir"));
    rom_dir->registerClickAction([rom_dir](brls::View*) {
        pick_folder("ROM folder", options::get("rom_dir"), [rom_dir](const std::string& dir) {
            if (host::set_option("rom_dir", dir))
                rom_dir->setDetailText(dir);
        });
        return true;
    });
    list->addView(rom_dir);
    add_option_cells(list, {"console_type", "save_location", "direct_boot"});

    add_header(list, "Display");
    add_option_cells(list, {"ui_rotation", "screen_layout", "screen_sizing", "swap_screens", "scaling",
                            "filter", "show_fps"});

    add_header(list, "Emulation");
    add_option_cells(list, {"cpu_clock", "jit", "fastmem", "renderer", "gpu_3d_delay", "threaded_3d", "audio_interpolation"});

    auto* hint = new brls::Label();
    hint->setText("DSi mode uses bios9i.bin, bios7i.bin, firmware_dsi.bin and nand.bin from the system "
                  "folder when present; without nand.bin, games are direct-booted.");
    hint->setFontSize(16);
    hint->setMargins(16, 0, 0, 0);
    list->addView(hint);

    auto* scroll = new brls::ScrollingFrame();
    scroll->setContentView(list);
    scroll->setGrow(1.0f);
    return scroll;
}

}  // namespace ui
