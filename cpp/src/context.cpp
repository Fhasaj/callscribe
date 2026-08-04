#include "context.hpp"

namespace callscribe {

AppContext::AppContext() {
    ensure_data_dirs();
    settings = load_settings();
    database = std::make_unique<db::Database>(settings.db_path);
    repo = std::make_unique<db::Repository>(*database);
    reminder = std::make_unique<tasks::ReminderScheduler>(*repo, settings.reminder_time, settings.reminder_enabled);
}

AppContext::~AppContext() {
    if (whisper_loader_thread_.joinable()) whisper_loader_thread_.join();
}

void AppContext::preload_whisper() {
    {
        std::lock_guard<std::mutex> lock(whisper_mutex_);
        if (mic_engine_ || whisper_loading_) return;
        whisper_loading_ = true;
        whisper_error_.clear();
        whisper_notice_.clear();
    }
    if (whisper_loader_thread_.joinable()) whisper_loader_thread_.join();

    std::string size = settings.whisper_model_size.empty() ? transcription::suggest_model_size()
                                                             : settings.whisper_model_size;
    std::string model_path = (models_dir() / transcription::model_filename(size)).string();

    whisper_loader_thread_ = std::thread([this, model_path] {
        try {
            auto mic = std::make_unique<transcription::WhisperEngine>(model_path);
            auto monitor = std::make_unique<transcription::WhisperEngine>(model_path);
            std::string note = mic->backend_note().empty() ? monitor->backend_note() : mic->backend_note();
            std::lock_guard<std::mutex> lock(whisper_mutex_);
            mic_engine_ = std::move(mic);
            monitor_engine_ = std::move(monitor);
            whisper_notice_ = note;
            whisper_loading_ = false;
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(whisper_mutex_);
            whisper_error_ = e.what();
            whisper_loading_ = false;
        }
    });
}

void AppContext::reload_whisper() {
    {
        std::lock_guard<std::mutex> lock(whisper_mutex_);
        mic_engine_.reset();
        monitor_engine_.reset();
    }
    preload_whisper();
}

bool AppContext::whisper_ready() const {
    std::lock_guard<std::mutex> lock(whisper_mutex_);
    return mic_engine_ != nullptr;
}

bool AppContext::whisper_loading() const {
    std::lock_guard<std::mutex> lock(whisper_mutex_);
    return whisper_loading_;
}

std::string AppContext::whisper_error() const {
    std::lock_guard<std::mutex> lock(whisper_mutex_);
    return whisper_error_;
}

std::string AppContext::whisper_notice() const {
    std::lock_guard<std::mutex> lock(whisper_mutex_);
    return whisper_notice_;
}

transcription::WhisperEngine* AppContext::mic_engine() {
    std::lock_guard<std::mutex> lock(whisper_mutex_);
    return mic_engine_.get();
}

transcription::WhisperEngine* AppContext::monitor_engine() {
    std::lock_guard<std::mutex> lock(whisper_mutex_);
    return monitor_engine_.get();
}

void AppContext::save_settings() { callscribe::save_settings(settings); }

}  // namespace callscribe
