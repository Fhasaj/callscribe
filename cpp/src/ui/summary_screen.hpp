#pragma once
#include <ftxui/component/component.hpp>

#include <mutex>
#include <string>
#include <thread>

#include "context.hpp"
#include "db/repository.hpp"

namespace callscribe::ui {

class SummaryScreen {
public:
    explicit SummaryScreen(AppContext& ctx);
    ~SummaryScreen();
    SummaryScreen(const SummaryScreen&) = delete;
    SummaryScreen& operator=(const SummaryScreen&) = delete;

    ftxui::Component component() { return root_; }

private:
    AppContext& ctx_;
    std::string year_text_;
    db::YearlyStats stats_;

    std::mutex mutex_;
    std::string report_;
    std::string status_message_;
    bool generating_ = false;
    std::thread worker_;

    // Review-report wizard: step 0 picks the cycle, step 1 ticks which of that
    // cycle's questions to answer. Selection flags are plain arrays because
    // ftxui::Checkbox binds to a bool* (std::vector<bool> can't provide one).
    static constexpr int kSemiQuestionCount = 2;
    static constexpr int kAnnualQuestionCount = 3;
    bool wizard_visible_ = false;
    int wizard_step_ = 0;
    int cycle_index_ = 0;  // 0 = semi-annual, 1 = annual
    bool semi_selected_[kSemiQuestionCount] = {true, true};
    bool annual_selected_[kAnnualQuestionCount] = {true, true, true};

    ftxui::Component root_;

    void load_stats();
    void generate_review_report();
    void build_ui();
};

}  // namespace callscribe::ui
