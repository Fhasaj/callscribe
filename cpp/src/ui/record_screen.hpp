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
#include "ui/proposed_tasks.hpp"

namespace callscribe::ui {

class RecordScreen {
public:
    explicit RecordScreen(AppContext& ctx);
    ~RecordScreen();
    RecordScreen(const RecordScreen&) = delete;
    RecordScreen& operator=(const RecordScreen&) = delete;

    ftxui::Component component() { return root_; }

    // Fills the form in from somewhere else in the app -- today, the "record
    // this" button on a calendar meeting. Ignored while a call is in flight,
    // since the fields are locked then and the running call already has its
    // own name. Blank arguments leave that field alone.
    void prefill(const std::string& meeting_name, const std::string& customer_name,
                  const std::vector<std::string>& participants);
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
    std::string manual_notes_;  // your own free-form notes for this call, saved as you type

    // Participants are edited one at a time in a popup (Add / rename / remove)
    // rather than as one comma-separated string, which was easy to typo and
    // impossible to correct without retyping the lot. `participants_` is still
    // the joined form -- it is what the DB column and the LLM prompt take, and
    // what monitor_worker_loop labels far-end speech with -- and is rebuilt
    // from the list by sync_participants(). The list is the source of truth.
    std::vector<std::string> participant_list_;
    std::string participants_;
    std::string new_participant_;
    bool participants_popup_ = false;
    ftxui::Component participant_rows_;

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
    // Confirmed tasks only. What the summarizer extracted lands as "proposed"
    // and is reviewed in proposed_tasks_ below before it becomes one of these.
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

    // Snapshots taken on the GUI thread when recording starts, so the monitor
    // worker can name speakers without reaching into participant_list_ (which
    // the popup owns) from another thread. The form is locked for the length
    // of a call, so these can't go stale mid-recording.
    std::vector<std::string> far_end_names_;
    bool detect_speaker_turns_ = false;

    // Review panel for the summarizer's suggested action items -- nothing it
    // extracts becomes a real task until it is confirmed here.
    ProposedTasksPanel proposed_tasks_;

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
    // Names the far-end speaker for turn `index`. With tinydiarize on, each
    // detected turn advances the index, and the participants supply the names;
    // past the end of the list (or with no list at all) it falls back to
    // "Speaker N", because a guessed name is worse than an honest number.
    std::string far_end_label(size_t turn_index) const;
    // Runs offline speaker diarization over the far-end recording and rewrites
    // the speaker on every far-end transcript line from the result. Called on
    // the finalize thread, before the transcript is handed to the summarizer,
    // so the summary sees the corrected names too. `monitor_audio` is consumed.
    void diarize_far_end(std::vector<int16_t> monitor_audio);
    // Rebuilds live_transcript_ from the database -- used after diarization has
    // renamed speakers, so the pane on screen matches what was stored.
    void reload_transcript_from_db();
    void finalize();
    void append_transcript_line(const std::string& speaker, const std::string& text);
    void refresh_linked_tasks();
    void add_task_for_call();
    // Rebuilds participants_ (the joined form) from participant_list_.
    void sync_participants();
    void add_participant();
    void rebuild_participant_rows();
    ftxui::Component build_participants_popup();
    void build_ui();
};

}  // namespace callscribe::ui
