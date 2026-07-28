#pragma once
#include <ftxui/component/component.hpp>

#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "context.hpp"
#include "db/repository.hpp"

namespace callscribe::ui {

class HistoryScreen {
public:
    explicit HistoryScreen(AppContext& ctx);
    ~HistoryScreen();
    HistoryScreen(const HistoryScreen&) = delete;
    HistoryScreen& operator=(const HistoryScreen&) = delete;

    ftxui::Component component() { return root_; }

private:
    AppContext& ctx_;

    std::string search_text_;
    std::vector<db::Call> calls_;
    std::vector<std::string> call_labels_;
    int selected_ = 0;

    std::mutex detail_mutex_;
    std::string transcript_;
    std::string summary_;
    std::string email_;
    std::string status_message_;
    bool regenerating_ = false;
    std::thread regen_thread_;

    ftxui::Component root_;

    void refresh_list();
    void load_detail();
    void regenerate();
    void build_ui();
};

}  // namespace callscribe::ui
