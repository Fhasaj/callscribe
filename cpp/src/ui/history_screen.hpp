#pragma once
#include <ftxui/component/component.hpp>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "context.hpp"
#include "db/repository.hpp"
#include "ui/proposed_tasks.hpp"

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
    std::string notes_;  // GUI-thread-only, bound directly to the notes Input
    // Last repository revision the list was built from; when it moves (a call
    // finished summarizing on the Record tab, say) the list and the open call
    // are reloaded, with the selection kept on the same call id.
    uint64_t seen_revision_ = 0;

    std::mutex detail_mutex_;
    std::string transcript_;
    std::string summary_;
    std::string email_;
    std::string status_message_;
    bool regenerating_ = false;
    std::thread regen_thread_;

    std::vector<std::string> inner_tab_titles_ = {"Transcript", "Notes", "Summary", "Email"};
    int inner_tab_index_ = 0;

    // Quick "add task" box on the Summary inner tab, tied to the selected
    // call -- lets you capture something the LLM's auto-extracted task list
    // missed without leaving the summary. linked_tasks_dirty_ is touched
    // from the regenerate() background thread too, so it's atomic.
    std::string new_task_text_;
    // Confirmed tasks only; anything the summarizer suggested waits in
    // proposed_tasks_ until it is reviewed.
    std::vector<db::Task> linked_tasks_;
    std::atomic<bool> linked_tasks_dirty_{true};

    // Same review panel the Record tab uses, pointed at the selected call --
    // so re-summarizing an old call here produces suggestions you can act on
    // rather than tasks that appeared behind your back.
    ProposedTasksPanel proposed_tasks_;

    ftxui::Component root_;

    void refresh_list();
    void load_detail();
    void save_current_notes();
    void regenerate();
    void refresh_linked_tasks();
    void add_task_for_selected();
    void build_ui();
};

}  // namespace callscribe::ui
