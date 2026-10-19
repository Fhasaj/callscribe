#include "ui/today_screen.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include "tasks/day_digest.hpp"
#include "ui/scroll_pane.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"
#include "ui/wrapped_input.hpp"
#include "util/date_format.hpp"
#include "util/time.hpp"

namespace callscribe::ui {

TodayScreen::TodayScreen(AppContext& ctx) : ctx_(ctx) { build_ui(); }

void TodayScreen::refresh() {
    seen_revision_ = ctx_.repo->revision();
    const std::string today = util::today_iso();
    // Past midnight with the app still open: start the new day clean.
    if (today != day_) {
        day_ = today;
        note_.clear();
        saved_at_.clear();
        if (auto log = ctx_.repo->get_daily_log(day_)) {
            note_ = log->note;
            saved_at_ = log->updated_at.size() >= 16 ? log->updated_at.substr(11, 5) : log->updated_at;
        }
    }
    digest_ = tasks::build_day_digest(*ctx_.repo, day_);
    wins_ = ctx_.repo->list_wins(util::offset_days_iso(-30), day_ + "~");

    using namespace ftxui;
    wins_container_->DetachAllChildren();
    const ButtonOption x_opt = link_style(theme::kDanger);
    for (const auto& w : wins_) {
        const int64_t id = w.id;
        auto del = Button("[x]", [this, id] { ctx_.repo->delete_win(id); }, x_opt);
        std::string line = util::iso_date_to_display(w.created_at.substr(0, 10)) + "  " +
                           (w.tag.empty() ? "" : "[" + w.tag + "] ") + w.text;
        wins_container_->Add(Renderer(del, [del, line] { return hbox({paragraph(line) | flex, text(" "), del->Render()}); }));
    }
}

void TodayScreen::save() {
    try {
        ctx_.repo->save_daily_log(day_, tasks::build_day_digest(*ctx_.repo, day_), note_);
        saved_at_ = util::now_time_hhmm();
        status_ = "Saved. The review reads these back at the end of the year.";
    } catch (const std::exception& e) {
        status_ = std::string("Couldn't save: ") + e.what();
    }
}

void TodayScreen::build_ui() {
    using namespace ftxui;

    InputOption note_opt;
    note_opt.multiline = true;
    auto note_input = WrappedInput(&note_, "How did today go? What moved forward, what got in the way, anything you're "
                                           "proud of...",
                                   note_opt);
    auto save_btn = Button("Save today's log", [this] { save(); }, button_style(theme::kAccent));
    wins_container_ = Container::Vertical({});
    auto digest_pane = ScrollableText([this] { return digest_; }, "", "What today held");

    auto layout = Container::Vertical({note_input, save_btn, digest_pane, wins_container_});

    root_ = Renderer(layout, [this, note_input, save_btn, digest_pane] {
        if (ctx_.repo->revision() != seen_revision_ || util::today_iso() != day_) refresh();

        Element note = vbox({
            note_input->Render() | size(HEIGHT, GREATER_THAN, 3) | size(HEIGHT, LESS_THAN, 7) |
                borderStyled(ROUNDED, theme::kMuted),
            hbox({
                save_btn->Render(),
                text("  "),
                (saved_at_.empty() ? badge("not saved yet", theme::kWarning)
                                   : badge("saved at " + saved_at_, theme::kSuccess)) |
                    vcenter,
                filler(),
            }),
            status_.empty() ? text("") : paragraph(status_) | dim,
        });
        Element wins = wins_.empty() ? paragraph("None yet. Fixed something, helped someone, shipped something? "
                                                 "Press ^W from any tab.") |
                                           dim
                                     : wins_container_->Render() | yframe | flex;

        Elements left = {section("How did " + util::iso_date_to_display(day_) + " go?", note),
                         digest_pane->Render() | flex};
        Element wins_section = section("Wins, last 30 days", wins, theme::kSuccess);

        Elements children;
        if (ftxui::Terminal::Size().dimx >= 110) {
            children.push_back(hbox({vbox(std::move(left)) | flex, text(" "), wins_section | size(WIDTH, EQUAL, 50)}) |
                               flex);
        } else {
            for (auto& e : left) children.push_back(std::move(e));
            children.push_back(wins_section | flex);
        }
        return vbox(std::move(children)) | flex;
    });
}

}  // namespace callscribe::ui
