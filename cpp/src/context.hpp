#pragma once
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "config.hpp"
#include "db/database.hpp"
#include "db/repository.hpp"
#include "spotify/spotify_client.hpp"
#include "tasks/meeting_reminder.hpp"
#include "tasks/reminder.hpp"
#include "transcription/whisper_engine.hpp"

namespace callscribe {

// Shared services handed to every screen -- settings, DB, reminder
// scheduler, and the (asynchronously loaded) Whisper engines. Two
// independent Whisper engines are kept (mic, monitor) since whisper.cpp
// contexts aren't safe for concurrent inference from multiple threads and
// each audio stream is transcribed on its own thread for speaker labeling.
class AppContext {
public:
    AppContext();
    ~AppContext();
    AppContext(const AppContext&) = delete;
    AppContext& operator=(const AppContext&) = delete;

    Settings settings;
    std::unique_ptr<db::Database> database;
    std::unique_ptr<db::Repository> repo;
    std::unique_ptr<tasks::ReminderScheduler> reminder;
    std::unique_ptr<tasks::MeetingReminder> meeting_reminder;
    // Always present; inert until a client ID is configured in Settings.
    std::unique_ptr<callscribe::spotify::SpotifyClient> spotify;

    void preload_whisper();
    void reload_whisper();  // call after a Settings change to model size
    bool whisper_ready() const;
    bool whisper_loading() const;
    std::string whisper_error() const;
    // Non-fatal warning from model loading (e.g. fell back to CPU for lack of VRAM).
    std::string whisper_notice() const;
    transcription::WhisperEngine* mic_engine();
    transcription::WhisperEngine* monitor_engine();
    // Hands back the per-stream inference buffers (KV cache, scratch) while
    // keeping the weights loaded, so an idle app isn't sitting on memory it
    // won't use until the next recording. Call once a call has finished
    // transcribing; the next transcribe() re-allocates transparently.
    void release_whisper_states();

    void save_settings();

    // llama-server's --api-key, from the keyring. Loaded at startup when
    // llama.cpp is the backend in use; set_llamacpp_api_key() stores (or, when
    // empty, removes) it and returns an error message, "" on success.
    std::string llamacpp_api_key() const;
    std::string set_llamacpp_api_key(const std::string& key);
    // Loads it if that hasn't happened yet (llama.cpp just got selected).
    void load_llamacpp_api_key();

private:
    mutable std::mutex api_key_mutex_;
    std::string llamacpp_api_key_;
    bool api_key_loaded_ = false;

    mutable std::mutex whisper_mutex_;
    std::shared_ptr<transcription::WhisperModel> whisper_model_;
    std::unique_ptr<transcription::WhisperEngine> mic_engine_;
    std::unique_ptr<transcription::WhisperEngine> monitor_engine_;
    std::string whisper_error_;
    std::string whisper_notice_;
    bool whisper_loading_ = false;
    std::thread whisper_loader_thread_;
};

}  // namespace callscribe
