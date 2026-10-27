#include "ui/achievements_view.h"

#include <borealis.hpp>

#include <memory>
#include <vector>

#include "frontend/achievements.h"
#include "platform/keyboard.h"
#include "ui/settings_list.h"

namespace ui {

namespace {

// A RecyclerFrame like the cheat list: some games have a few hundred achievements.
class AchievementSource : public brls::RecyclerDataSource {
  public:
    AchievementSource() : list_(achievements::list()) {}

    int numberOfSections(brls::RecyclerFrame*) override {
        return 1;
    }

    int numberOfRows(brls::RecyclerFrame*, int) override {
        return int(list_.size());
    }

    std::string titleForHeader(brls::RecyclerFrame*, int) override {
        achievements::Status s = achievements::status();
        return s.game_title + ": " + std::to_string(s.unlocked) + " of " + std::to_string(s.total) + " unlocked";
    }

    brls::RecyclerCell* cellForRow(brls::RecyclerFrame* recycler, brls::IndexPath index) override {
        auto* cell = static_cast<brls::DetailCell*>(recycler->dequeueReusableCell("Cell"));
        const achievements::Entry& e = list_[index.row];
        brls::Theme theme = brls::Application::getTheme();
        cell->setText(e.title + " (" + std::to_string(e.points) + ")");
        cell->setDetailText(e.state);
        cell->setDetailTextColor(e.unlocked ? theme["brls/list/listItem_value_color"] : theme["brls/text_disabled"]);
        return cell;
    }

    void didSelectRowAt(brls::RecyclerFrame*, brls::IndexPath index) override {
        const achievements::Entry& e = list_[index.row];
        auto* dialog = new brls::Dialog(e.title + "\n\n" + e.description);
        dialog->addButton("OK", [] {});
        dialog->open();
    }

  private:
    std::vector<achievements::Entry> list_;
};

std::string account_text(const achievements::Status& s) {
    if (s.busy)
        return "Logging in...";
    if (!s.logged_in)
        return "Not logged in";
    return s.user + " (" + std::to_string(s.score) + " points)";
}

// Cells are updated in place after a login (rebuilding would delete the focused cell).
class AccountPanel : public brls::Box {
  public:
    AccountPanel() : brls::Box(brls::Axis::COLUMN) {
        setPadding(40, 60, 40, 60);

        auto* header = new brls::Header();
        header->setTitle("Account");
        addView(header);

        account_ = new brls::DetailCell();
        account_->setText("RetroAchievements account");
        addView(account_);

        action_ = new brls::DetailCell();
        action_->registerClickAction([this](brls::View*) {
            if (achievements::status().logged_in)
                logout();
            else
                login();
            return true;
        });
        addView(action_);

        auto* header2 = new brls::Header();
        header2->setTitle("Options");
        addView(header2);
        add_option_cells(this, {"ra_hardcore"});

        auto* hint = new brls::Label();
        hint->setText("Hardcore mode earns full credit, but turns off cheats and loading save states. "
                      "Achievements need an internet connection; unlocks made offline are sent once it's back. "
                      "Your password is only used to log in; a login token is kept on the SD card.");
        hint->setFontSize(16);
        hint->setMargins(16, 0, 0, 0);
        addView(hint);

        refresh();
    }

    ~AccountPanel() override {
        alive_.reset();
    }

  private:
    brls::DetailCell* account_;
    brls::DetailCell* action_;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);

    void refresh() {
        achievements::Status s = achievements::status();
        account_->setDetailText(account_text(s));
        action_->setText(s.logged_in ? "Log out" : "Log in");
    }

    void login() {
        std::string user, password;
        if (!keyboard::prompt("RetroAchievements username", "", false, user))
            return;
        if (!keyboard::prompt("RetroAchievements password", "", true, password))
            return;
        std::weak_ptr<int> alive = alive_;
        achievements::login(user, password, [this, alive](bool, const std::string&) {
            brls::sync([this, alive] {
                if (alive.lock())
                    refresh();
            });
        });
        refresh();
    }

    void logout() {
        achievements::logout();
        refresh();
    }
};

}  // namespace

brls::View* make_achievements_settings() {
    auto* scroll = new brls::ScrollingFrame();
    scroll->setContentView(new AccountPanel());
    scroll->setGrow(1.0f);
    return scroll;
}

void open_achievements() {
    if (achievements::list().empty()) {
        brls::Application::notify("No achievements to show for this game");
        return;
    }
    auto* recycler = new brls::RecyclerFrame();
    recycler->setPadding(30, 60, 30, 60);
    recycler->estimatedRowHeight = 70;
    recycler->registerCell("Header", [] { return brls::RecyclerHeader::create(); });
    recycler->registerCell("Cell", [] { return new brls::DetailCell(); });
    recycler->setDataSource(new AchievementSource());
    recycler->setGrow(1.0f);
    auto* frame = new brls::AppletFrame(recycler);
    frame->setTitle("Achievements");
    brls::Application::pushActivity(new brls::Activity(frame));
}

}  // namespace ui
