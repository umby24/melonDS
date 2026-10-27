#include "ui/cheats_view.h"

#include <borealis.hpp>

#include <memory>
#include <unordered_map>
#include <vector>

#include "frontend/cheats.h"

namespace ui {

namespace {

// Databases have thousands of codes for some games (2000+ for Pokémon HG/SS). A plain
// Box of cells draws every cell each frame and overflows deko3d's per-frame command
// buffer, so the list is a RecyclerFrame: only on-screen cells exist and get reused.

struct Section {
    std::string title;
    std::vector<size_t> codes;  // indices into cheats::list()
};

void style_cell(brls::DetailCell* cell, const cheats::Cheat& c) {
    brls::Theme theme = brls::Application::getTheme();
    cell->setText(c.name);
    cell->setDetailText(c.enabled ? "On" : "Off");
    cell->setDetailTextColor(c.enabled ? theme["brls/list/listItem_value_color"] : theme["brls/text_disabled"]);
}

class CheatSource : public brls::RecyclerDataSource {
  public:
    CheatSource() {
        const auto& codes = cheats::list();
        const auto& cats = cheats::categories();
        Section loose{"Codes", {}};
        std::vector<Section> by_cat(cats.size());
        for (size_t c = 0; c < cats.size(); ++c)
            by_cat[c].title = cats[c].exclusive ? cats[c].name + " (pick one)" : cats[c].name;
        for (size_t i = 0; i < codes.size(); ++i) {
            if (codes[i].category < 0)
                loose.codes.push_back(i);
            else
                by_cat[codes[i].category].codes.push_back(i);
        }
        if (!loose.codes.empty())
            sections_.push_back(std::move(loose));
        for (auto& s : by_cat)
            if (!s.codes.empty())
                sections_.push_back(std::move(s));
    }

    int numberOfSections(brls::RecyclerFrame*) override {
        return int(sections_.size());
    }

    int numberOfRows(brls::RecyclerFrame*, int section) override {
        return int(sections_[section].codes.size());
    }

    std::string titleForHeader(brls::RecyclerFrame*, int section) override {
        return sections_[section].title;
    }

    brls::RecyclerCell* cellForRow(brls::RecyclerFrame* recycler, brls::IndexPath index) override {
        auto* cell = static_cast<brls::DetailCell*>(recycler->dequeueReusableCell("Cell"));
        size_t code = sections_[index.section].codes[index.row];
        style_cell(cell, cheats::list()[code]);
        bound_[cell] = code;
        return cell;
    }

    void didSelectRowAt(brls::RecyclerFrame*, brls::IndexPath index) override {
        size_t code = sections_[index.section].codes[index.row];
        cheats::set_enabled(code, !cheats::list()[code].enabled);
        // Exclusive categories may have switched siblings off; refresh what's on screen.
        for (auto& [cell, bound] : bound_)
            style_cell(cell, cheats::list()[bound]);
    }

  private:
    std::vector<Section> sections_;
    std::unordered_map<brls::DetailCell*, size_t> bound_;  // cell -> code it shows now
};

class CheatsActivity : public brls::Activity {
  public:
    brls::View* createContentView() override {
        brls::View* content = cheats::list().empty() ? make_empty() : make_list();
        auto* frame = new brls::AppletFrame(content);
        frame->setTitle(cheats::imported_from().empty() ? "Cheats" : "Cheats - " + cheats::imported_from());
        if (cheats::database_available() && !cheats::list().empty()) {
            frame->registerAction("Re-import", brls::BUTTON_X, [this](brls::View*) {
                auto* dialog = new brls::Dialog("Replace this game's cheat list with the codes from usrcheat.dat?");
                dialog->addButton("Cancel", [] {});
                dialog->addButton("Replace", [this] { import(); });
                dialog->open();
                return true;
            });
        }
        return frame;
    }

  private:
    brls::View* make_list() {
        auto* recycler = new brls::RecyclerFrame();
        recycler->setPadding(30, 60, 30, 60);
        recycler->estimatedRowHeight = 70;
        recycler->registerCell("Header", [] { return brls::RecyclerHeader::create(); });
        recycler->registerCell("Cell", [] { return new brls::DetailCell(); });
        recycler->setDataSource(new CheatSource());
        recycler->setGrow(1.0f);
        return recycler;
    }

    brls::View* make_empty() {
        auto* box = new brls::Box(brls::Axis::COLUMN);
        box->setPadding(30, 60, 30, 60);
        auto* l1 = new brls::Label();
        l1->setText("No cheats for this game yet.");
        box->addView(l1);
        auto* l2 = new brls::Label();
        l2->setText("Put a melonDS cheat file at " + cheats::file_path() +
                    (cheats::database_available() ? ", or import this game's codes:"
                                                  : ", or put usrcheat.dat next to your BIOS files (system/) to import codes."));
        l2->setMargins(10, 0, 20, 0);
        box->addView(l2);
        if (cheats::database_available()) {
            auto* cell = new brls::DetailCell();
            cell->setText("Import cheats");
            cell->setDetailText("usrcheat.dat");
            cell->registerClickAction([this](brls::View*) {
                import();
                return true;
            });
            box->addView(cell);
        }
        return box;
    }

    // Picks the usrcheat.dat entry: the one whose checksum matches the ROM if there is
    // exactly one, otherwise the player chooses (other entries are other regions or
    // revisions, whose codes may not fit this ROM).
    void import() {
        std::string message;
        std::vector<cheats::DatabaseEntry> entries = cheats::database_entries(message);
        if (entries.empty()) {
            brls::Application::notify(message);
            return;
        }
        std::vector<size_t> matches;
        for (size_t i = 0; i < entries.size(); ++i)
            if (entries[i].checksum_match)
                matches.push_back(i);
        if (matches.size() == 1) {
            import_entry(matches[0], true);
            return;
        }
        if (entries.size() == 1) {
            import_entry(0, false);
            return;
        }
        std::vector<std::string> labels;
        for (const cheats::DatabaseEntry& e : entries)
            labels.push_back(e.name + " (" + std::to_string(e.codes) + " codes)" +
                             (e.checksum_match ? " - matches your ROM" : ""));
        auto choice = std::make_shared<int>(-1);
        auto* dropdown = new brls::Dropdown(
            "Which version is your game?", labels, [choice](int i) { *choice = i; },
            matches.empty() ? 0 : int(matches[0]),
            [this, choice, entries](int) {
                if (*choice >= 0)
                    import_entry(size_t(*choice), entries[*choice].checksum_match);
            });
        brls::Application::pushActivity(new brls::Activity(dropdown));
    }

    // Importing replaces the whole list; reopen the screen rather than rebuilding views
    // under the focused one (which would leave borealis with a dangling focus).
    void import_entry(size_t index, bool checksum_match) {
        std::string message;
        if (!cheats::import_from_database(index, message)) {
            brls::Application::notify(message);
            return;
        }
        brls::Application::notify(std::to_string(cheats::list().size()) + " codes imported" +
                                  (checksum_match ? "" : " (not an exact match for your ROM; some may not work)"));
        brls::sync([] {
            brls::Application::popActivity(brls::TransitionAnimation::NONE, [] { open_cheats(); });
        });
    }
};

}  // namespace

void open_cheats() {
    brls::Application::pushActivity(new CheatsActivity());
}

}  // namespace ui
