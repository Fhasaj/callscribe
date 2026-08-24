#pragma once
#include <ftxui/component/component.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio/capture.hpp"
#include "context.hpp"

namespace callscribe::ui {

class SettingsScreen {
public:
    explicit SettingsScreen(AppContext& ctx);
    ~SettingsScreen();
    SettingsScreen(const SettingsScreen&) = delete;
    SettingsScreen& operator=(const SettingsScreen&) = delete;

    ftxui::Component component() { return root_; }
    // True while a mic/speaker test is running its live level gauge, which
    // needs the fast redraw tick to look smooth.
    bool needs_fast_redraw() const { return testing_.load(); }

private:
    AppContext& ctx_;

    std::vector<std::string> ollama_models_;
    int ollama_model_selected_ = 0;
    std::atomic<bool> models_need_refresh_{false};

    std::vector<std::string> whisper_sizes_ = {"tiny", "base", "small", "medium", "large-v3"};
    int whisper_size_selected_ = 2;

    std::vector<std::string> mic_labels_;
    std::vector<std::string> mic_names_;
    int mic_selected_ = 0;
    std::vector<std::string> monitor_labels_;
    std::vector<std::string> monitor_names_;
    int monitor_selected_ = 0;

    std::string ollama_keep_alive_;
    std::string user_display_name_;
    std::string db_path_;
    std::string reminder_time_;
    bool reminder_enabled_ = true;
    bool echo_cancel_ = true;

    // Editable AI prompt presets (what to ask for; the JSON envelope is fixed).
    std::string summary_prompt_;
    std::string email_prompt_;
    std::string tasks_prompt_;

    std::mutex status_mutex_;
    std::string ollama_status_;
    std::string pull_status_;
    std::string whisper_status_;
    bool pulling_ = false;
    bool downloading_ = false;
    std::thread pull_thread_;
    std::thread download_thread_;

    // Mic/Speaker test: a throwaway StreamCapture on the currently selected
    // source, read for its live level only (nothing transcribed/saved).
    std::unique_ptr<audio::StreamCapture> test_capture_;
    std::atomic<bool> testing_{false};
    std::string testing_label_;  // "Mic" or "Speaker"
    std::chrono::steady_clock::time_point test_started_at_;

    ftxui::Component root_;

    void refresh_models();
    void refresh_audio_devices();
    void pull_recommended();
    void unload_now();
    void download_model();
    void start_mic_test();
    void start_monitor_test();
    void stop_test();
    void save();
    void build_ui();
};

}  // namespace callscribe::ui
