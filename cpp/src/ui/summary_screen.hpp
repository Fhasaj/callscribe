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
    std::string narrative_;
    std::string status_message_;
    bool generating_ = false;
    std::thread worker_;

    ftxui::Component root_;

    void load_stats();
    void generate_narrative();
    void build_ui();
};

}  // namespace callscribe::ui
