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
    // A running download animates a progress bar, so it wants the fast tick
    // for the same reason a live level meter does.
    bool needs_fast_redraw() const {
        return testing_.load() || downloading_.load() || downloading_diarization_.load();
    }

private:
    AppContext& ctx_;

    // Which AI server: 0 = Ollama, 1 = llama.cpp (llama-server).
    std::vector<std::string> llm_backends_ = {"Ollama", "llama.cpp"};
    int llm_backend_index_ = 0;
    std::string llamacpp_host_;
    std::string llamacpp_status_;
    std::string llamacpp_api_key_;

    std::vector<std::string> ollama_models_;
    int ollama_model_selected_ = 0;
    std::atomic<bool> models_need_refresh_{false};

    // small.en-tdrz is the odd one out: an English-only `small` that is the
    // only model whisper.cpp can detect speaker turns with (see
    // detect_speaker_turns_ below).
    std::vector<std::string> whisper_sizes_ = {"tiny", "base", "small", "medium", "large-v3", "small.en-tdrz"};
    int whisper_size_selected_ = 2;
    bool detect_speaker_turns_ = false;
    bool diarize_far_end_ = false;

    std::vector<std::string> mic_labels_;
    std::vector<std::string> mic_names_;
    int mic_selected_ = 0;
    std::vector<std::string> monitor_labels_;
    std::vector<std::string> monitor_names_;
    int monitor_selected_ = 0;

    std::string ollama_keep_alive_;
    bool ollama_unload_after_use_ = true;
    std::string user_display_name_;
    std::string db_path_;
    std::string reminder_time_;
    bool reminder_enabled_ = true;
    bool echo_cancel_ = true;

    // Focus timer lengths, as typed; parsed and clamped on Save.
    std::string focus_minutes_;
    std::string short_break_minutes_;
    std::string long_break_minutes_;

    std::string hyperfocus_minutes_;
    std::string meeting_reminder_minutes_;
    std::string end_of_day_time_;
    bool end_of_day_enabled_ = true;

    std::string spotify_client_id_;
    std::string spotify_port_;
    std::string spotify_playlist_;
    std::string spotify_note_;  // validation feedback from Save/Connect

    // Editable AI prompt presets (what to ask for; the JSON envelope is fixed).
    std::string summary_prompt_;
    std::string email_prompt_;
    std::string tasks_prompt_;

    std::mutex status_mutex_;
    std::string ollama_status_;
    std::string save_status_;  // footer line after Save
    std::string pull_status_;
    std::string whisper_status_;
    bool pulling_ = false;
    // Atomic because needs_fast_redraw() is called from the ticker thread.
    std::atomic<bool> downloading_{false};
    std::thread pull_thread_;
    std::thread download_thread_;
    std::thread diarization_download_thread_;
    std::atomic<bool> downloading_diarization_{false};
    std::string diarization_status_;

    // Live transfer counters, written from the download threads and read by
    // the renderer -- atomics rather than status_mutex_ so a progress tick
    // (which fires many times a second) never contends with the status text.
    // `total` is 0 until the server reports a content length, and stays 0 if
    // it never does; the renderer shows bytes-so-far in that case rather than
    // a meaningless percentage.
    std::atomic<int64_t> whisper_bytes_{0};
    std::atomic<int64_t> whisper_bytes_total_{0};
    std::atomic<int64_t> diarization_bytes_{0};
    std::atomic<int64_t> diarization_bytes_total_{0};
    // "speaker embeddings (2 of 2)" -- which file of the pair is in flight.
    std::string diarization_file_label_;

    // Mic/Speaker test: a throwaway StreamCapture on the currently selected
    // source, read for its live level only (nothing transcribed/saved).
    std::unique_ptr<audio::StreamCapture> test_capture_;
    std::atomic<bool> testing_{false};
    std::string testing_label_;  // "Mic" or "Speaker"
    std::chrono::steady_clock::time_point test_started_at_;

    ftxui::Component root_;

    void refresh_models();
    // Saves the llama.cpp choice and asks the server what it has loaded and
    // how big its context window is.
    void check_llamacpp();
    void refresh_audio_devices();
    void pull_recommended();
    void unload_now();
    void download_model();
    void download_diarization_models();
    void start_mic_test();
    void start_monitor_test();
    void stop_test();
    void save();
    // Copies the focus-timer and Spotify fields into ctx_.settings, without
    // writing the file -- shared by Save and by Connect, which mustn't pay for
    // the Whisper reload a full Save triggers.
    void apply_focus_and_spotify_fields();
    void build_ui();
};

}  // namespace callscribe::ui
