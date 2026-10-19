#pragma once
#include <ftxui/component/component.hpp>

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "context.hpp"
#include "db/repository.hpp"

namespace callscribe::ui {

enum class TaskRowState { Normal, DueToday, Overdue, Done };

class TasksScreen {
public:
    explicit TasksScreen(AppContext& ctx);
    ~TasksScreen();
    TasksScreen(const TasksScreen&) = delete;
    TasksScreen& operator=(const TasksScreen&) = delete;
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

    // "Break it down": the model suggests a few first steps for a task, which
    // come back for review -- ticked ones become tasks of their own.
    std::thread breakdown_worker_;
    std::atomic<bool> breaking_down_{false};
    std::mutex breakdown_mutex_;
    // Worker -> UI handoff, guarded by breakdown_mutex_.
    bool breakdown_ready_ = false;
    std::vector<std::string> breakdown_result_;
    std::string breakdown_error_;
    // UI thread only.
    db::Task breakdown_task_;
    std::vector<std::string> breakdown_steps_;
    std::deque<bool> breakdown_checked_;
    bool breakdown_visible_ = false;
    ftxui::Component breakdown_list_;
    ftxui::Component breakdown_first_;

    ftxui::Component task_list_container_;
    ftxui::Component root_;

    void refresh();
    void toggle_task(int index);
    void delete_task_at(int index);
    void begin_edit(int index);
    void cancel_edit();
    void submit_form();  // adds, or saves the task being edited
    void start_breakdown(int index);
    void take_breakdown_result();  // UI thread, once the worker is done
    void add_breakdown_steps();
    void clear_form();
    void build_ui();
};

}  // namespace callscribe::ui
