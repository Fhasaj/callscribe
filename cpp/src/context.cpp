#include "context.hpp"

#include "llm/ollama_client.hpp"

namespace callscribe {

AppContext::AppContext() {
    ensure_data_dirs();
    settings = load_settings();
    llm::set_keep_alive(settings.ollama_keep_alive);
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
            // One copy of the weights, two inference states -- see WhisperModel.
            auto model = std::make_shared<transcription::WhisperModel>(model_path, /*expected_states=*/2);
            auto mic = std::make_unique<transcription::WhisperEngine>(model);
            auto monitor = std::make_unique<transcription::WhisperEngine>(model);
            std::string note = model->backend_note();
            std::lock_guard<std::mutex> lock(whisper_mutex_);
            whisper_model_ = std::move(model);
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
        // States first: they hold a reference to the model they were made from.
        mic_engine_.reset();
        monitor_engine_.reset();
        whisper_model_.reset();
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

void AppContext::save_settings() {
    callscribe::save_settings(settings);
    llm::set_keep_alive(settings.ollama_keep_alive);
}

}  // namespace callscribe
