#pragma once
#include <ftxui/component/component.hpp>

#include <deque>
#include <string>
#include <vector>

#include "context.hpp"
#include "db/repository.hpp"

namespace callscribe::ui {

enum class TaskRowState { Normal, DueToday, Overdue, Done };

class TasksScreen {
public:
    explicit TasksScreen(AppContext& ctx);
    ftxui::Component component() { return root_; }

private:
    AppContext& ctx_;

    std::string new_task_text_;
    std::string new_task_due_;  // typed as DD MM YYYY / today / tomorrow, converted to ISO on add
    std::string status_message_;
    bool show_completed_ = false;
    bool needs_refresh_ = false;

    std::vector<db::Task> tasks_;
    std::vector<TaskRowState> row_state_;
    std::deque<bool> task_checked_;  // NOT vector<bool> -- its bit-packed specialization
                                      // has no real bool* addresses for Checkbox to bind to

    int open_count_ = 0;
    int due_today_count_ = 0;
    int overdue_count_ = 0;

    ftxui::Component task_list_container_;
    ftxui::Component root_;

    void refresh();
    void toggle_task(int index);
    void delete_task_at(int index);
    void add_task();
    void build_ui();
};

}  // namespace callscribe::ui
