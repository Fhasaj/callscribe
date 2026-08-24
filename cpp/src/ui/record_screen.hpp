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
    // Paused counts as recording: the call is still open, and quitting would
    // still lose it.
    bool is_recording() const {
        State s = state_.load();
        return s == State::Recording || s == State::Paused;
    }

private:
    enum class State { Idle, Recording, Paused, Processing };

    AppContext& ctx_;

    // GUI-thread-only (Input components own these directly).
    std::string meeting_name_;
    std::string customer_name_;
    std::string participants_;
    std::string manual_notes_;  // your own free-form notes for this call, saved as you type

    std::atomic<State> state_{State::Idle};
    // Elapsed time is the sum of the finished recording stretches plus the one
    // currently running, so a pause doesn't inflate the call's duration.
    std::chrono::steady_clock::time_point start_time_;
    long long recorded_seconds_ = 0;

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
    // Also reloaded when any other screen writes (editing or completing one of
    // these tasks over on the Tasks tab, for instance).
    uint64_t seen_revision_ = 0;

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
    void toggle_pause();
    long long elapsed_seconds() const;
    // Combined across both streams; non-zero once transcription falls far
    // enough behind that queued audio had to be discarded.
    size_t dropped_chunks() const {
        return (mic_capture_ ? mic_capture_->dropped_chunks() : 0) +
               (monitor_capture_ ? monitor_capture_->dropped_chunks() : 0);
    }
    void mic_worker_loop();
    void monitor_worker_loop();
    void finalize();
    void append_transcript_line(const std::string& speaker, const std::string& text);
    void refresh_linked_tasks();
    void add_task_for_call();
    void build_ui();
};

}  // namespace callscribe::ui
