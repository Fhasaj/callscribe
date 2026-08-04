#pragma once
#include <ftxui/component/component.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "audio/capture.hpp"
#include "audio/echo_cancel.hpp"
#include "context.hpp"
#include "db/repository.hpp"

namespace callscribe::ui {

class RecordScreen {
public:
    explicit RecordScreen(AppContext& ctx);
    ~RecordScreen();
    RecordScreen(const RecordScreen&) = delete;
    RecordScreen& operator=(const RecordScreen&) = delete;

    ftxui::Component component() { return root_; }
    bool is_recording() const { return state_.load() == State::Recording; }

private:
    enum class State { Idle, Recording, Processing };

    AppContext& ctx_;

    // GUI-thread-only (Input components own these directly).
    std::string meeting_name_;
    std::string customer_name_;
    std::string participants_;
    std::string manual_notes_;  // your own free-form notes for this call, saved as you type

    std::atomic<State> state_{State::Idle};
    std::chrono::steady_clock::time_point start_time_;

    std::mutex text_mutex_;
    std::string live_transcript_;
    std::string summary_text_;
    std::string email_draft_;
    std::string error_message_;
    // Which phase of transcribe/condense/summarize is running, so a long call
    // shows progress instead of an unexplained wait.
    std::string status_detail_;

    // Live/Notes/Summary/Email inner tab bar, below the meeting/customer form.
    std::vector<std::string> inner_tab_titles_ = {"Live", "Notes", "Summary", "Email"};
    int inner_tab_index_ = 0;

    int64_t call_id_ = -1;
    std::optional<int64_t> customer_id_;

    // Quick "add task" box on the Summary inner tab, tied to this call --
    // lets you capture something the LLM's auto-extracted task list missed
    // without leaving the summary. linked_tasks_dirty_ is touched from the
    // finalize() background thread too, so it's atomic.
    std::string new_task_text_;
    std::vector<db::Task> linked_tasks_;
    std::atomic<bool> linked_tasks_dirty_{true};

    // Held for the length of a recording; its destructor restores the default
    // sink and unloads the echo-cancel module.
    std::unique_ptr<audio::EchoCancelSession> echo_cancel_;
    std::unique_ptr<audio::StreamCapture> mic_capture_;
    std::unique_ptr<audio::StreamCapture> monitor_capture_;
    std::thread mic_worker_;
    std::thread monitor_worker_;
    std::thread finalize_worker_;

    ftxui::Component root_;

    void start_recording();
    void stop_recording();
    void mic_worker_loop();
    void monitor_worker_loop();
    void finalize();
    void append_transcript_line(const std::string& speaker, const std::string& text);
    void refresh_linked_tasks();
    void add_task_for_call();
    void build_ui();
};

}  // namespace callscribe::ui
