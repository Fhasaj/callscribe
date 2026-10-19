#pragma once
#include <ftxui/component/component.hpp>

#include <functional>
#include <string>
#include <vector>

#include "context.hpp"

namespace callscribe::ui {

// The two things worth catching the moment they happen, from any tab, without
// losing your place:
//   - a win (^W): one line about something that went well, for the year-end
//     review, filed under a project;
//   - a parked thought (^P): whatever just popped into your head mid-focus,
//     out of your head and onto a list the Focus tab shows at the next break.
class QuickCapture {
public:
    enum class Kind { Win, Park };

    // `current_tag` suggests the project for a win (the focus round under way,
    // if any); `on_saved` reports what was saved, for the status bar.
    QuickCapture(AppContext& ctx, std::function<std::string()> current_tag,
                 std::function<void(const std::string&)> on_saved);

    void open(Kind kind);
    bool* visible() { return &visible_; }
    ftxui::Component component() { return root_; }

private:
    AppContext& ctx_;
    std::function<std::string()> current_tag_;
    std::function<void(const std::string&)> on_saved_;

    Kind kind_ = Kind::Win;
    bool visible_ = false;
    std::string text_;
    std::vector<std::string> tags_;
    int tag_selected_ = 0;
    std::string error_;

    ftxui::Component input_;
    ftxui::Component root_;

    void save();
};

}  // namespace callscribe::ui
