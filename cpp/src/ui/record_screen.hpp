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
#include "context.hpp"

namespace callscribe::ui {

class RecordScreen {
public:
    explicit RecordScreen(AppContext& ctx);
    ~RecordScreen();
    RecordScreen(const RecordScreen&) = delete;
    RecordScreen& operator=(const RecordScreen&) = delete;

    ftxui::Component component() { return root_; }

private:
    enum class State { Idle, Recording, Processing };

    AppContext& ctx_;

    // GUI-thread-only (Input components own these directly).
    std::string meeting_name_;
    std::string customer_name_;
    std::string participants_;

    std::atomic<State> state_{State::Idle};
    std::chrono::steady_clock::time_point start_time_;

    std::mutex text_mutex_;
    std::string live_transcript_;
    std::string summary_text_;
    std::string email_draft_;
    std::string error_message_;

    // Live/Summary/Email inner tab bar, below the meeting/customer form.
    std::vector<std::string> inner_tab_titles_ = {"Live", "Summary", "Email"};
    int inner_tab_index_ = 0;

    int64_t call_id_ = -1;
    std::optional<int64_t> customer_id_;

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
    void build_ui();
};

}  // namespace callscribe::ui
