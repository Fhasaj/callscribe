#pragma once
#include <ftxui/component/component.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "context.hpp"

namespace callscribe::ui {

// End-of-day wrap-up: what today held (built from what was recorded, nothing
// to fill in), a line of your own about it, and the wins so far. Saving it
// writes the daily log, which the year-end review reads back.
class TodayScreen {
public:
    explicit TodayScreen(AppContext& ctx);
    ftxui::Component component() { return root_; }

private:
    AppContext& ctx_;
    std::string day_;
    std::string digest_;
    std::string note_;
    std::string saved_at_;  // "" until today's log exists
    std::string status_;
    std::vector<db::Win> wins_;
    uint64_t seen_revision_ = ~uint64_t{0};
    ftxui::Component wins_container_;
    ftxui::Component root_;

    void refresh();
    void save();
    void build_ui();
};

}  // namespace callscribe::ui
