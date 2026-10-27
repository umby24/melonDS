#include "ui/folder_picker.h"

#include <borealis.hpp>

#include "ui/dir_list.h"

namespace ui {

namespace {

constexpr const char* kSdRoot = "sdmc:/";

class FolderPicker : public brls::Activity {
  public:
    FolderPicker(std::string title, std::string start, std::function<void(const std::string&)> on_pick)
        : title_(std::move(title)), dir_(std::move(start)), on_pick_(std::move(on_pick)) {}

    brls::View* createContentView() override {
        list_ = new brls::Box(brls::Axis::COLUMN);
        list_->setPadding(40, 60, 40, 60);

        auto* scroll = new brls::ScrollingFrame();
        scroll->setContentView(list_);
        scroll->setGrow(1.0f);

        frame_ = new brls::AppletFrame(scroll);
        // Replaces AppletFrame's own B (pop) action.
        frame_->registerAction("Up", brls::BUTTON_B, [this](brls::View*) {
            if (is_root(dir_))
                brls::Application::popActivity();
            else
                navigate(parent_path(dir_));
            return true;
        });
        frame_->registerAction("Use this folder", brls::BUTTON_X, [this](brls::View*) {
            pick();
            return true;
        });
        return frame_;
    }

    void onContentAvailable() override {
        show(is_dir(dir_) ? dir_ : kSdRoot);
    }

  private:
    std::string title_;
    std::string dir_;
    std::function<void(const std::string&)> on_pick_;
    brls::AppletFrame* frame_ = nullptr;
    brls::Box* list_ = nullptr;

    void pick() {
        on_pick_(dir_);
        brls::Application::popActivity();
    }

    // Rebuilding the list deletes the cell whose action is running; defer it.
    void navigate(const std::string& dir) {
        brls::sync([this, dir] { show(dir); });
    }

    brls::DetailCell* add_cell(const std::string& text, const std::string& detail,
                               std::function<void()> fn) {
        auto* cell = new brls::DetailCell();
        cell->setText(text);
        cell->setDetailText(detail);
        cell->registerClickAction([fn](brls::View*) {
            fn();
            return true;
        });
        list_->addView(cell);
        return cell;
    }

    void show(const std::string& dir) {
        dir_ = dir;
        frame_->setTitle(title_ + ": " + dir_);
        list_->clearViews();

        std::vector<DirEntry> entries = list_dir(dir_, true);
        size_t roms = 0;
        for (const DirEntry& e : entries)
            roms += e.is_dir ? 0 : 1;

        brls::DetailCell* first = add_cell("Use this folder",
            std::to_string(roms) + (roms == 1 ? " ROM" : " ROMs"), [this] { pick(); });
        if (!is_root(dir_))
            add_cell("..", "Up one folder", [this] { navigate(parent_path(dir_)); });
        for (const DirEntry& e : entries) {
            if (!e.is_dir)
                continue;
            std::string path = join_path(dir_, e.name);
            add_cell(e.name + "/", "", [this, path] { navigate(path); });
        }
        brls::Application::giveFocus(first);
    }
};

}  // namespace

void pick_folder(const std::string& title, const std::string& start,
                 std::function<void(const std::string&)> on_pick) {
    brls::Application::pushActivity(new FolderPicker(title, start, std::move(on_pick)));
}

}  // namespace ui
