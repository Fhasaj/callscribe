#include "context.hpp"

#include "llm/ollama_client.hpp"
#include "util/keyring.hpp"

namespace callscribe {

AppContext::AppContext() {
    ensure_data_dirs();
    settings = load_settings();
    llm::set_backend(settings.uses_llamacpp() ? llm::Backend::LlamaCpp : llm::Backend::Ollama);
    llm::set_keep_alive(settings.ollama_keep_alive);
    llm::set_unload_after_use(settings.ollama_unload_after_use);
    // Only when it'll be used: a keyring lookup can mean a wallet-unlock prompt.
    if (settings.uses_llamacpp()) load_llamacpp_api_key();
    database = std::make_unique<db::Database>(settings.db_path);
    repo = std::make_unique<db::Repository>(*database);
    reminder = std::make_unique<tasks::ReminderScheduler>(*repo, settings.reminder_time, settings.reminder_enabled);
    reminder->set_end_of_day(settings.end_of_day_time, settings.end_of_day_enabled);
    meeting_reminder = std::make_unique<tasks::MeetingReminder>();
    meeting_reminder->set_lead_minutes(settings.meeting_reminder_minutes);
    meeting_reminder->start();
    spotify = std::make_unique<spotify::SpotifyClient>();
    spotify->configure(settings.spotify_client_id, settings.spotify_redirect_port);
    // Off the UI thread like everything else it does: unlocking the keyring
    // may be waiting on a KWallet / GNOME Keyring prompt.
    spotify->check();
}

AppContext::~AppContext() {
    if (whisper_loader_thread_.joinable()) whisper_loader_thread_.join();
    // Quitting should hand the memory back too, not leave the last model
    // sitting out its keep_alive with nothing left to serve.
    if (settings.ollama_unload_after_use) llm::unload_model(settings.llm_host(), settings.llm_model());
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

void AppContext::release_whisper_states() {
    std::lock_guard<std::mutex> lock(whisper_mutex_);
    if (mic_engine_) mic_engine_->release_state();
    if (monitor_engine_) monitor_engine_->release_state();
}

void AppContext::save_settings() {
    callscribe::save_settings(settings);
    llm::set_backend(settings.uses_llamacpp() ? llm::Backend::LlamaCpp : llm::Backend::Ollama);
    llm::set_keep_alive(settings.ollama_keep_alive);
    llm::set_unload_after_use(settings.ollama_unload_after_use);
    spotify->configure(settings.spotify_client_id, settings.spotify_redirect_port);
}

void AppContext::load_llamacpp_api_key() {
    std::lock_guard<std::mutex> lock(api_key_mutex_);
    if (api_key_loaded_) return;
    api_key_loaded_ = true;
    std::string error;
    llamacpp_api_key_ = util::lookup_secret(kLlamaCppApiKeySecret, error).value_or("");
    llm::set_api_key(llamacpp_api_key_);
}

std::string AppContext::llamacpp_api_key() const {
    std::lock_guard<std::mutex> lock(api_key_mutex_);
    return llamacpp_api_key_;
}

std::string AppContext::set_llamacpp_api_key(const std::string& key) {
    std::lock_guard<std::mutex> lock(api_key_mutex_);
    if (api_key_loaded_ && key == llamacpp_api_key_) return "";
    std::string error;
    bool ok = key.empty() ? util::clear_secret(kLlamaCppApiKeySecret, error)
                          : util::store_secret(kLlamaCppApiKeySecret, key, error);
    // Used for this run even if the keyring refused it.
    api_key_loaded_ = true;
    llamacpp_api_key_ = key;
    llm::set_api_key(key);
    return ok ? "" : error;
}

}  // namespace callscribe
