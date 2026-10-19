#include "ui/quick_capture.hpp"

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>

#include "focus/pomodoro.hpp"
#include "ui/styled_input.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"
#include "ui/wrapped_input.hpp"

namespace callscribe::ui {

QuickCapture::QuickCapture(AppContext& ctx, std::function<std::string()> current_tag,
                           std::function<void(const std::string&)> on_saved)
    : ctx_(ctx), current_tag_(std::move(current_tag)), on_saved_(std::move(on_saved)) {
    using namespace ftxui;

    tags_ = focus::project_tags();
    tags_.push_back("None");

    InputOption opt = styled_input_option();
    opt.multiline = false;
    opt.on_enter = [this] { save(); };
    input_ = WrappedInput(&text_, "", opt);
    auto tag_menu = Menu(&tags_, &tag_selected_, MenuOption::Horizontal());
    auto save_btn = Button("Save", [this] { save(); }, button_style(theme::kSuccess));
    auto cancel_btn = Button("Cancel", [this] { visible_ = false; }, button_style(theme::kMuted));

    auto container = Container::Vertical({
        input_,
        Maybe(tag_menu, [this] { return kind_ == Kind::Win; }),
        Container::Horizontal({save_btn, cancel_btn}),
    });

    root_ = Renderer(container, [this, tag_menu, save_btn, cancel_btn] {
                const bool win = kind_ == Kind::Win;
                const Color c = win ? theme::kSuccess : theme::kWarning;
                Elements rows = {
                    text(win ? "Log a win" : "Park a thought") | bold | color(c),
                    text(win ? "Something that went well -- the year-end review is built from these."
                             : "Get it out of your head. It'll be waiting on the Focus tab at your next break.") |
                        dim,
                    separator() | color(theme::kMuted),
                    input_->Render(),
                };
                if (win) rows.push_back(hbox({text("Project: "), tag_menu->Render()}));
                if (!error_.empty()) rows.push_back(paragraph(error_) | color(theme::kDanger));
                rows.push_back(text("Enter save · Esc cancel") | dim);
                rows.push_back(hbox({save_btn->Render(), text("  "), cancel_btn->Render()}) | hcenter);
                return vbox(std::move(rows)) | size(WIDTH, EQUAL, 70) | borderStyled(ROUNDED, c) |
                       bgcolor(Color::Black) | clear_under | center;
            }) |
            CatchEvent([this](const Event& e) {
                if (e != Event::Escape) return false;
                visible_ = false;
                return true;
            });
}

void QuickCapture::open(Kind kind) {
    kind_ = kind;
    text_.clear();
    error_.clear();
    if (kind == Kind::Win) {
        // Pre-pick the project of the round under way; otherwise keep whatever
        // was used last time.
        std::string tag = current_tag_ ? current_tag_() : "";
        auto it = std::find(tags_.begin(), tags_.end(), tag);
        if (it != tags_.end()) tag_selected_ = static_cast<int>(it - tags_.begin());
    }
    visible_ = true;
    input_->TakeFocus();
}

void QuickCapture::save() {
    std::string text = text_;
    text.erase(0, text.find_first_not_of(" \t"));
    text.erase(text.find_last_not_of(" \t") + 1);
    if (text.empty()) {
        visible_ = false;
        return;
    }
    try {
        if (kind_ == Kind::Win) {
            const std::string& tag = tags_[static_cast<size_t>(tag_selected_)];
            ctx_.repo->add_win(text, tag == "None" ? "" : tag);
            if (on_saved_) on_saved_("Win logged");
        } else {
            ctx_.repo->park(text);
            if (on_saved_) on_saved_("Parked for your next break");
        }
        visible_ = false;
    } catch (const std::exception& e) {
        error_ = std::string("Couldn't save: ") + e.what();
    }
}

}  // namespace callscribe::ui
