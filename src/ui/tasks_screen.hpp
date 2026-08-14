#pragma once
#include <ftxui/component/component.hpp>

#include <cstdint>
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

    // The one form at the top does double duty: it adds a new task, or edits
    // the task whose "edit" button was pressed (editing_id_ >= 0).
    std::string form_text_;
    std::string form_due_;  // typed as DD MM YYYY / today / tomorrow, converted to ISO on save
    std::string form_customer_;
    int form_priority_ = 1;  // index into kPriorityLabels: 0 low, 1 medium, 2 high
    int64_t editing_id_ = -1;

    std::string status_message_;
    bool show_completed_ = false;
    bool needs_refresh_ = false;
    // Last repository revision this list was built from -- when the Record
    // screen files the tasks it extracted from a call, this is how they show
    // up here without the tab having to be re-entered.
    uint64_t seen_revision_ = 0;

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
    void begin_edit(int index);
    void cancel_edit();
    void submit_form();  // adds, or saves the task being edited
    void clear_form();
    void build_ui();
};

}  // namespace callscribe::ui
