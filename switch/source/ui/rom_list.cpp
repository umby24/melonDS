#include "ui/rom_list.h"

#include <borealis.hpp>
#include <borealis/extern/nanovg/stb_image.h>  // implementation compiled into borealis' nanovg

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "frontend/options.h"
#include "frontend/rom_info.h"
#include "frontend/thumbnails.h"
#include "ui/dir_list.h"
#include "ui/game.h"

namespace ui {

namespace {

constexpr float kCardW = 186;
constexpr float kCardH = 236;
constexpr float kGap = 22;
constexpr float kPadX = 40;
constexpr float kPadY = 30;
constexpr float kIconSize = 128;  // the 32x32 banner icon at 4x
constexpr float kRadius = 8;
constexpr int kArtMaxW = 166;     // cover art is scaled down to fit this on the card
constexpr int kArtMaxH = 128;

// Decoded cover art, already scaled to card size (keeps CPU and GPU memory small).
struct Art {
    std::vector<uint32_t> px;  // RGBA8
    int w = 0, h = 0;
};

// Box-filtered downscale of an RGBA8 image to fit kArtMaxW x kArtMaxH.
Art fit_art(const unsigned char* src, int sw, int sh) {
    Art a;
    float scale = std::min({1.0f, float(kArtMaxW) / sw, float(kArtMaxH) / sh});
    a.w = std::max(1, int(sw * scale));
    a.h = std::max(1, int(sh * scale));
    a.px.resize(size_t(a.w) * a.h);
    for (int y = 0; y < a.h; ++y) {
        int y0 = y * sh / a.h, y1 = std::max(y0 + 1, (y + 1) * sh / a.h);
        for (int x = 0; x < a.w; ++x) {
            int x0 = x * sw / a.w, x1 = std::max(x0 + 1, (x + 1) * sw / a.w);
            unsigned sum[4] = {};
            for (int yy = y0; yy < y1; ++yy)
                for (int xx = x0; xx < x1; ++xx)
                    for (int c = 0; c < 4; ++c)
                        sum[c] += src[(size_t(yy) * sw + xx) * 4 + c];
            unsigned n = unsigned((y1 - y0) * (x1 - x0));
            a.px[size_t(y) * a.w + x] = (sum[0] / n) | ((sum[1] / n) << 8) | ((sum[2] / n) << 16) | ((sum[3] / n) << 24);
        }
    }
    return a;
}

bool load_art(const std::string& path, Art& out) {
    int w, h, channels;
    unsigned char* px = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (!px)
        return false;
    out = fit_art(px, w, h);
    stbi_image_free(px);
    return true;
}

struct Card {
    std::string name;  // file / folder name
    std::string path;
    bool is_dir;
    bool loaded = false;  // banner read (or failed)
    rom_info::Info info;
    Art art;              // downloaded cover art, if any (pixels dropped once uploaded)
    int image = 0;        // NanoVG image of info.icon, created on first draw
    int art_image = 0;    // NanoVG image of art
    int art_w = 0, art_h = 0;
};

struct Result {
    rom_info::Info info;
    Art art;
};

// Banner reads happen on a background thread; results are handed over here. The grid
// cancels and joins it on destruction (at most one small file read to wait for; libnx
// can't detach threads, std::thread::detach throws).
struct Loader {
    std::mutex mx;
    std::vector<std::optional<Result>> results;  // per card; set when read
    bool cancel = false;
};

// One view for the whole grid: it draws only the visible cards (a Box of cards would draw
// every card each frame, see docs/ARCHITECTURE.md "borealis limits"), and does its own
// selection, scrolling and touch handling. D-pad presses that would leave the grid fall
// through to borealis' normal navigation (e.g. left into the tab sidebar).
class RomGrid : public brls::View {
  public:
    explicit RomGrid(std::vector<Card> cards) : cards_(std::move(cards)), loader_(std::make_shared<Loader>()) {
        setFocusable(true);
        setHideHighlight(true);
        setGrow(1.0f);

        registerAction("", brls::BUTTON_NAV_UP, [this](brls::View*) { return move(0, -1); }, true, true);
        registerAction("", brls::BUTTON_NAV_DOWN, [this](brls::View*) { return move(0, 1); }, true, true);
        registerAction("", brls::BUTTON_NAV_LEFT, [this](brls::View*) { return move(-1, 0); }, true, true);
        registerAction("", brls::BUTTON_NAV_RIGHT, [this](brls::View*) { return move(1, 0); }, true, true);
        registerAction("Open", brls::BUTTON_A, [this](brls::View*) {
            open(selected_);
            return true;
        }, false, false, brls::SOUND_CLICK);

        addGestureRecognizer(new brls::TapGestureRecognizer([this](brls::TapGestureStatus status, brls::Sound*) {
            if (status.state != brls::GestureState::END)
                return;
            int i = hit_test(status.position.x, status.position.y);
            if (i >= 0) {
                selected_ = i;
                open(i);
            }
        }));
        addGestureRecognizer(new brls::PanGestureRecognizer([this](brls::PanGestureStatus status, brls::Sound*) {
            if (status.state == brls::GestureState::UNSURE || status.state == brls::GestureState::STAY) {
                scroll_ = target_ = clamp_scroll(scroll_ - status.delta.y);
            }
        }, brls::PanAxis::VERTICAL));

        start_loading();
    }

    ~RomGrid() override {
        {
            std::lock_guard<std::mutex> lk(loader_->mx);
            loader_->cancel = true;
        }
        if (thread_.joinable())
            thread_.join();
        NVGcontext* vg = brls::Application::getNVGContext();
        for (Card& c : cards_) {
            if (c.image)
                nvgDeleteImage(vg, c.image);
            if (c.art_image)
                nvgDeleteImage(vg, c.art_image);
        }
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style,
              brls::FrameContext* ctx) override;

  private:
    std::vector<Card> cards_;
    std::shared_ptr<Loader> loader_;
    std::thread thread_;
    int selected_ = 0;
    int cols_ = 1;
    float scroll_ = 0, target_ = 0;  // content offset; scroll_ eases toward target_
    float view_x_ = 0, view_y_ = 0, view_w_ = 0, view_h_ = 0;
    float origin_x_ = 0;             // left edge of the first column

    void start_loading() {
        std::vector<std::string> paths;
        for (const Card& c : cards_)
            paths.push_back(c.is_dir ? "" : c.path);
        loader_->results.resize(paths.size());
        std::string art_type = options::is("card_image", "art") ? options::get("thumb_type") : "";
        thread_ = std::thread([loader = loader_, paths, art_type] {
            for (size_t i = 0; i < paths.size(); ++i) {
                {
                    std::lock_guard<std::mutex> lk(loader->mx);
                    if (loader->cancel)
                        return;
                }
                Result r;
                if (!paths[i].empty() && rom_info::read(paths[i], r.info) && !art_type.empty()) {
                    std::string art = thumbnails::path_for(art_type, r.info.game_code);
                    if (!art.empty())
                        load_art(art, r.art);
                }
                std::lock_guard<std::mutex> lk(loader->mx);
                loader->results[i] = std::move(r);
            }
        });
    }

    void collect_results() {
        std::lock_guard<std::mutex> lk(loader_->mx);
        for (size_t i = 0; i < cards_.size(); ++i) {
            if (!cards_[i].loaded && loader_->results[i]) {
                cards_[i].info = std::move(loader_->results[i]->info);
                cards_[i].art = std::move(loader_->results[i]->art);
                cards_[i].loaded = true;
                loader_->results[i].reset();
            }
        }
    }

    int rows() const {
        return (int(cards_.size()) + cols_ - 1) / cols_;
    }

    float content_height() const {
        return kPadY * 2 + rows() * kCardH + std::max(0, rows() - 1) * kGap;
    }

    float clamp_scroll(float s) const {
        return std::clamp(s, 0.0f, std::max(0.0f, content_height() - view_h_));
    }

    void ensure_visible() {
        int row = selected_ / cols_;
        float top = kPadY + row * (kCardH + kGap);
        float bottom = top + kCardH;
        if (top - kPadY < target_)
            target_ = top - kPadY;
        else if (bottom + kPadY > target_ + view_h_)
            target_ = bottom + kPadY - view_h_;
        target_ = clamp_scroll(target_);
    }

    bool move(int dx, int dy) {
        int n = int(cards_.size());
        int col = selected_ % cols_, row = selected_ / cols_;
        int next = selected_;
        if (dx < 0 && col > 0)
            next = selected_ - 1;
        else if (dx > 0 && col < cols_ - 1 && selected_ + 1 < n)
            next = selected_ + 1;
        else if (dy < 0 && row > 0)
            next = selected_ - cols_;
        else if (dy > 0 && row < rows() - 1)
            next = std::min(selected_ + cols_, n - 1);
        if (next == selected_)
            return false;  // at the edge: let borealis navigate out of the grid
        selected_ = next;
        ensure_visible();
        brls::Application::getAudioPlayer()->play(brls::SOUND_FOCUS_CHANGE);
        return true;
    }

    int hit_test(float px, float py) const {
        float lx = px - origin_x_, ly = py - view_y_ + scroll_ - kPadY;
        if (lx < 0 || ly < 0)
            return -1;
        int col = int(lx / (kCardW + kGap)), row = int(ly / (kCardH + kGap));
        if (col >= cols_ || lx - col * (kCardW + kGap) > kCardW || ly - row * (kCardH + kGap) > kCardH)
            return -1;
        int i = row * cols_ + col;
        return i < int(cards_.size()) ? i : -1;
    }

    void open(int i) {
        if (i < 0 || i >= int(cards_.size()))
            return;
        const Card& c = cards_[i];
        if (c.is_dir) {
            auto* frame = new brls::AppletFrame(make_rom_list(c.path));
            frame->setTitle(c.name);
            brls::Application::pushActivity(new brls::Activity(frame));
        } else {
            launch_game(c.path);
        }
    }

    void draw_card(NVGcontext* vg, Card& c, float cx, float cy, bool selected);
};

void draw_lines(NVGcontext* vg, float x, float y, float width, const std::string& text, int max_lines,
                float line_height) {
    NVGtextRow rows[4];
    const char* start = text.c_str();
    const char* end = start + text.size();
    int n = nvgTextBreakLines(vg, start, end, width, rows, std::min(max_lines, 4));
    for (int i = 0; i < n; ++i) {
        std::string line(rows[i].start, rows[i].end);
        if (i == n - 1 && rows[i].next < end && *rows[i].next) {
            // More text than fits: end on an ellipsis.
            while (!line.empty() && nvgTextBounds(vg, 0, 0, (line + "...").c_str(), nullptr, nullptr) > width)
                line.pop_back();
            line += "...";
        }
        nvgText(vg, x + width / 2, y + i * line_height, line.c_str(), nullptr);
    }
}

void draw_folder_icon(NVGcontext* vg, float x, float y, float size, NVGcolor color) {
    float w = size * 0.8f, h = size * 0.6f;
    float fx = x + (size - w) / 2, fy = y + (size - h) / 2 + size * 0.05f;
    nvgBeginPath(vg);
    nvgRoundedRect(vg, fx, fy - h * 0.12f, w * 0.4f, h * 0.3f, 4);
    nvgRoundedRect(vg, fx, fy, w, h, 6);
    nvgFillColor(vg, color);
    nvgFill(vg);
}

void RomGrid::draw_card(NVGcontext* vg, Card& c, float cx, float cy, bool selected) {
    brls::Theme theme = brls::Application::getTheme();

    nvgBeginPath(vg);
    nvgRoundedRect(vg, cx, cy, kCardW, kCardH, kRadius);
    nvgFillColor(vg, selected ? theme["brls/highlight/background"] : theme["brls/sidebar/background"]);
    nvgFill(vg);

    float ix = cx + (kCardW - kIconSize) / 2, iy = cy + 14;
    if (!c.art.px.empty() && !c.art_image) {
        c.art_image = nvgCreateImageRGBA(vg, c.art.w, c.art.h, 0, reinterpret_cast<const unsigned char*>(c.art.px.data()));
        c.art_w = c.art.w;
        c.art_h = c.art.h;
        c.art = Art{};
    }
    if (c.is_dir) {
        draw_folder_icon(vg, ix, iy, kIconSize, theme["brls/highlight/color1"]);
    } else if (c.art_image) {
        float ax = cx + (kCardW - c.art_w) / 2, ay = iy + (kIconSize - c.art_h) / 2;
        nvgBeginPath(vg);
        nvgRect(vg, ax, ay, float(c.art_w), float(c.art_h));
        nvgFillPaint(vg, nvgImagePattern(vg, ax, ay, float(c.art_w), float(c.art_h), 0, c.art_image, 1.0f));
        nvgFill(vg);
    } else if (c.loaded && !c.info.icon.empty()) {
        if (!c.image)
            c.image = nvgCreateImageRGBA(vg, 32, 32, NVG_IMAGE_NEAREST,
                                         reinterpret_cast<const unsigned char*>(c.info.icon.data()));
        nvgBeginPath(vg);
        nvgRect(vg, ix, iy, kIconSize, kIconSize);
        nvgFillPaint(vg, nvgImagePattern(vg, ix, iy, kIconSize, kIconSize, 0, c.image, 1.0f));
        nvgFill(vg);
    } else {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, ix, iy, kIconSize, kIconSize, 6);
        nvgFillColor(vg, theme["brls/backdrop"]);
        nvgFill(vg);
    }

    // Title from the banner (file name until it's read, or if there is none).
    std::string title = c.name;
    if (!c.is_dir) {
        if (title.size() > 4)
            title = title.substr(0, title.size() - 4);
        if (c.loaded && !c.info.title.empty())
            title = c.info.subtitle.empty() ? c.info.title : c.info.title + " " + c.info.subtitle;
    }
    float tx = cx + 10, tw = kCardW - 20, ty = iy + kIconSize + 12;
    nvgFontFaceId(vg, brls::Application::getDefaultFont());
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
    nvgFontSize(vg, 17);
    nvgFillColor(vg, theme["brls/text"]);
    draw_lines(vg, tx, ty, tw, title, 2, 21);

    std::string sub = c.is_dir ? "Folder" : c.info.publisher;
    if (!sub.empty()) {
        nvgFontSize(vg, 14);
        nvgFillColor(vg, theme["brls/text_disabled"]);
        draw_lines(vg, tx, cy + kCardH - 24, tw, sub, 1, 16);
    }

    if (selected && isFocused()) {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, cx - 3, cy - 3, kCardW + 6, kCardH + 6, kRadius + 2);
        nvgStrokeColor(vg, theme["brls/highlight/color1"]);
        nvgStrokeWidth(vg, 4);
        nvgStroke(vg);
    }
}

void RomGrid::draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style,
                   brls::FrameContext*) {
    collect_results();
    view_x_ = x, view_y_ = y, view_w_ = width, view_h_ = height;

    int cols = std::max(1, int((width - 2 * kPadX + kGap) / (kCardW + kGap)));
    if (cols != cols_) {
        cols_ = cols;
        ensure_visible();
    }
    float grid_w = cols_ * kCardW + (cols_ - 1) * kGap;
    origin_x_ = x + std::max(kPadX, (width - grid_w) / 2);

    scroll_ += (target_ - scroll_) * 0.3f;
    if (std::fabs(target_ - scroll_) < 0.5f)
        scroll_ = target_;

    nvgSave(vg);
    nvgIntersectScissor(vg, x, y, width, height);
    int first_row = std::max(0, int((scroll_ - kPadY) / (kCardH + kGap)));
    for (int row = first_row; row < rows(); ++row) {
        float cy = y + kPadY + row * (kCardH + kGap) - scroll_;
        if (cy > y + height)
            break;
        for (int col = 0; col < cols_; ++col) {
            int i = row * cols_ + col;
            if (i >= int(cards_.size()))
                break;
            draw_card(vg, cards_[i], origin_x_ + col * (kCardW + kGap), cy, i == selected_);
        }
    }
    nvgRestore(vg);
}

}  // namespace

brls::View* make_rom_list(const std::string& dir) {
    std::vector<DirEntry> entries = list_dir(dir, true);
    if (entries.empty()) {
        auto* box = new brls::Box(brls::Axis::COLUMN);
        box->setPadding(40, 60, 40, 60);
        auto* label = new brls::Label();
        label->setText("No .nds files or folders in " + dir);
        label->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        box->addView(label);
        return box;
    }
    std::vector<Card> cards;
    for (const DirEntry& e : entries)
        cards.push_back({e.name, join_path(dir, e.name), e.is_dir});
    return new RomGrid(std::move(cards));
}

}  // namespace ui
