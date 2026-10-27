#include "ui/downloader_view.h"

#include <borealis.hpp>

#include "frontend/options.h"
#include "frontend/thumbnails.h"
#include "ui/settings_list.h"

namespace ui {

namespace {

std::string status_text(const thumbnails::Progress& p) {
    if (p.total == 0 && !p.running)
        return "Idle";
    if (p.running && p.total == 0)
        return "Looking for ROMs...";
    std::string s = std::to_string(p.scanned) + " / " + std::to_string(p.total);
    if (p.running)
        s += " - " + p.current;
    else
        s += " done";
    return s;
}

std::string summary_text(const thumbnails::Progress& p) {
    if (p.total == 0)
        return "";
    std::string s = std::to_string(p.downloaded) + " downloaded, " + std::to_string(p.skipped) +
                    " skipped, " + std::to_string(p.missing) + " not on GameTDB";
    if (p.failed)
        s += ", " + std::to_string(p.failed) + " failed (" + p.error + ")";
    return s;
}

// Polls the download job each frame and updates its cells in place.
class DownloaderPanel : public brls::Box {
  public:
    DownloaderPanel() : brls::Box(brls::Axis::COLUMN) {
        setPadding(40, 60, 40, 60);

        auto* header = new brls::Header();
        header->setTitle("Cover art");
        addView(header);
        add_option_cells(this, {"thumb_type", "card_image"});

        auto* hint = new brls::Label();
        hint->setText("Art comes from GameTDB (art.gametdb.com), matched by the game code inside each ROM, "
                      "so file names don't matter. Searches " + options::get("rom_dir") +
                      " and its subfolders. Needs an internet connection.");
        hint->setFontSize(16);
        hint->setMargins(10, 0, 20, 0);
        addView(hint);

        auto* missing = new brls::DetailCell();
        missing->setText("Download missing art");
        missing->registerClickAction([](brls::View*) {
            thumbnails::start(options::get("rom_dir"), options::get("thumb_type"), false);
            return true;
        });
        addView(missing);

        auto* all = new brls::DetailCell();
        all->setText("Re-download all art");
        all->registerClickAction([](brls::View*) {
            auto* dialog = new brls::Dialog("Download art again for every game, replacing what's there?");
            dialog->addButton("Cancel", [] {});
            dialog->addButton("Re-download", [] {
                thumbnails::start(options::get("rom_dir"), options::get("thumb_type"), true);
            });
            dialog->open();
            return true;
        });
        addView(all);

        auto* stop = new brls::DetailCell();
        stop->setText("Stop downloading");
        stop->registerClickAction([](brls::View*) {
            thumbnails::cancel();
            return true;
        });
        addView(stop);

        auto* header2 = new brls::Header();
        header2->setTitle("Progress");
        addView(header2);

        status_ = new brls::DetailCell();
        status_->setText("Status");
        addView(status_);

        summary_ = new brls::Label();
        summary_->setFontSize(16);
        summary_->setMargins(10, 0, 0, 0);
        addView(summary_);
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style,
              brls::FrameContext* ctx) override {
        thumbnails::Progress p = thumbnails::progress();
        std::string status = status_text(p), summary = summary_text(p);
        if (status != last_status_) {
            status_->setDetailText(status);
            last_status_ = status;
        }
        if (summary != last_summary_) {
            summary_->setText(summary);
            last_summary_ = summary;
        }
        brls::Box::draw(vg, x, y, width, height, style, ctx);
    }

  private:
    brls::DetailCell* status_;
    brls::Label* summary_;
    std::string last_status_ = "-", last_summary_ = "-";
};

}  // namespace

brls::View* make_downloader() {
    auto* scroll = new brls::ScrollingFrame();
    scroll->setContentView(new DownloaderPanel());
    scroll->setGrow(1.0f);
    return scroll;
}

}  // namespace ui
