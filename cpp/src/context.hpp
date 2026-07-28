#pragma once
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "config.hpp"
#include "db/database.hpp"
#include "db/repository.hpp"
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

    void preload_whisper();
    void reload_whisper();  // call after a Settings change to model size
    bool whisper_ready() const;
    bool whisper_loading() const;
    std::string whisper_error() const;
    transcription::WhisperEngine* mic_engine();
    transcription::WhisperEngine* monitor_engine();

    void save_settings();

private:
    mutable std::mutex whisper_mutex_;
    std::unique_ptr<transcription::WhisperEngine> mic_engine_;
    std::unique_ptr<transcription::WhisperEngine> monitor_engine_;
    std::string whisper_error_;
    bool whisper_loading_ = false;
    std::thread whisper_loader_thread_;
};

}  // namespace callscribe
