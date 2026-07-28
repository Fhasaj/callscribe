#include "ui/settings_screen.hpp"

#include <ftxui/dom/elements.hpp>

#include "audio/devices.hpp"
#include "config.hpp"
#include "llm/ollama_client.hpp"
#include "transcription/whisper_engine.hpp"

namespace callscribe::ui {

SettingsScreen::SettingsScreen(AppContext& ctx) : ctx_(ctx) {
    user_display_name_ = ctx_.settings.user_display_name;
    db_path_ = ctx_.settings.db_path;
    reminder_time_ = ctx_.settings.reminder_time;
    reminder_enabled_ = ctx_.settings.reminder_enabled;

    std::string initial_size =
        ctx_.settings.whisper_model_size.empty() ? transcription::suggest_model_size() : ctx_.settings.whisper_model_size;
    for (size_t i = 0; i < whisper_sizes_.size(); ++i) {
        if (whisper_sizes_[i] == initial_size) whisper_size_selected_ = static_cast<int>(i);
    }

    build_ui();
    refresh_models();
    refresh_audio_devices();
}

SettingsScreen::~SettingsScreen() {
    if (pull_thread_.joinable()) pull_thread_.join();
    if (download_thread_.joinable()) download_thread_.join();
}

void SettingsScreen::refresh_models() {
    std::string status;
    try {
        ollama_models_ = llm::list_models(ctx_.settings.ollama_host);
        status = ollama_models_.empty() ? "No models installed yet. Use 'Pull recommended model'."
                                         : (std::to_string(ollama_models_.size()) + " model(s) installed.");
    } catch (const std::exception& e) {
        ollama_models_.clear();
        status = std::string("Ollama not reachable: ") + e.what();
    }
    ollama_model_selected_ = 0;
    for (size_t i = 0; i < ollama_models_.size(); ++i) {
        if (ollama_models_[i] == ctx_.settings.ollama_model) ollama_model_selected_ = static_cast<int>(i);
    }
    std::lock_guard<std::mutex> lock(status_mutex_);
    ollama_status_ = status;
}

void SettingsScreen::refresh_audio_devices() {
    mic_labels_.clear();
    mic_names_.clear();
    for (auto& s : audio::list_mic_sources()) {
        mic_labels_.push_back(s.description);
        mic_names_.push_back(s.name);
    }
    monitor_labels_.clear();
    monitor_names_.clear();
    for (auto& s : audio::list_monitor_sources()) {
        monitor_labels_.push_back(s.description);
        monitor_names_.push_back(s.name);
    }

    mic_selected_ = 0;
    for (size_t i = 0; i < mic_names_.size(); ++i) {
        if (mic_names_[i] == ctx_.settings.mic_source) mic_selected_ = static_cast<int>(i);
    }
    monitor_selected_ = 0;
    for (size_t i = 0; i < monitor_names_.size(); ++i) {
        if (monitor_names_[i] == ctx_.settings.monitor_source) monitor_selected_ = static_cast<int>(i);
    }
}

void SettingsScreen::pull_recommended() {
    if (pull_thread_.joinable()) pull_thread_.join();
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        pulling_ = true;
        pull_status_ = std::string("Pulling ") + kRecommendedOllamaModel + "...";
    }
    pull_thread_ = std::thread([this] {
        try {
            llm::pull_model(ctx_.settings.ollama_host, kRecommendedOllamaModel, [this](const std::string& status) {
                std::lock_guard<std::mutex> lock(status_mutex_);
                pull_status_ = status;
            });
            std::lock_guard<std::mutex> lock(status_mutex_);
            pull_status_ = "Pull complete.";
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(status_mutex_);
            pull_status_ = std::string("Pull failed: ") + e.what();
        }
        pulling_ = false;
        models_need_refresh_.store(true);
    });
}

void SettingsScreen::download_model() {
    std::string size = whisper_sizes_[whisper_size_selected_];
    if (download_thread_.joinable()) download_thread_.join();
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        downloading_ = true;
        whisper_status_ = "Downloading ggml-" + size + ".bin...";
    }
    download_thread_ = std::thread([this, size] {
        std::lock_guard<std::mutex> lock(status_mutex_);
        try {
            transcription::ensure_model_downloaded(size);
            whisper_status_ = "Download complete.";
        } catch (const std::exception& e) {
            whisper_status_ = std::string("Download failed: ") + e.what();
        }
        downloading_ = false;
    });
}

void SettingsScreen::save() {
    ctx_.settings.ollama_model = (ollama_model_selected_ >= 0 && ollama_model_selected_ < static_cast<int>(ollama_models_.size()))
                                      ? ollama_models_[ollama_model_selected_]
                                      : "";
    ctx_.settings.whisper_model_size = whisper_sizes_[whisper_size_selected_];
    ctx_.settings.mic_source =
        (mic_selected_ >= 0 && mic_selected_ < static_cast<int>(mic_names_.size())) ? mic_names_[mic_selected_] : "";
    ctx_.settings.monitor_source = (monitor_selected_ >= 0 && monitor_selected_ < static_cast<int>(monitor_names_.size()))
                                        ? monitor_names_[monitor_selected_]
                                        : "";
    ctx_.settings.user_display_name = user_display_name_.empty() ? kDefaultUserDisplayName : user_display_name_;
    bool db_path_changed = ctx_.settings.db_path != db_path_;
    ctx_.settings.db_path = db_path_;
    ctx_.settings.reminder_time = reminder_time_.empty() ? "09:00" : reminder_time_;
    ctx_.settings.reminder_enabled = reminder_enabled_;

    ctx_.save_settings();
    ctx_.reload_whisper();
    ctx_.reminder->set_reminder_time(ctx_.settings.reminder_time);
    ctx_.reminder->set_enabled(ctx_.settings.reminder_enabled);

    std::lock_guard<std::mutex> lock(status_mutex_);
    ollama_status_ =
        "Settings saved." + std::string(db_path_changed ? " Restart CallScribe for the new database path." : "") +
        " Whisper model reloading in background.";
}

void SettingsScreen::build_ui() {
    using namespace ftxui;

    auto model_dropdown = Dropdown(&ollama_models_, &ollama_model_selected_);
    auto refresh_models_btn = Button("Refresh", [this] { refresh_models(); });
    auto pull_btn = Button(std::string("Pull ") + kRecommendedOllamaModel, [this] { pull_recommended(); });

    auto whisper_size_dropdown = Dropdown(&whisper_sizes_, &whisper_size_selected_);
    auto download_model_btn = Button("Download model", [this] { download_model(); });

    auto mic_dropdown = Dropdown(&mic_labels_, &mic_selected_);
    auto monitor_dropdown = Dropdown(&monitor_labels_, &monitor_selected_);
    auto refresh_devices_btn = Button("Refresh audio devices", [this] { refresh_audio_devices(); });

    InputOption name_opt;
    name_opt.multiline = false;
    auto name_input = Input(&user_display_name_, "Your name", name_opt);

    InputOption db_opt;
    db_opt.multiline = false;
    auto db_input = Input(&db_path_, "Database file path", db_opt);

    auto reminder_checkbox = Checkbox("Daily reminder enabled", &reminder_enabled_);
    InputOption time_opt;
    time_opt.multiline = false;
    auto time_input = Input(&reminder_time_, "HH:MM", time_opt);

    auto save_btn = Button("Save Settings", [this] { save(); });

    auto layout = Container::Vertical({
        Container::Horizontal({model_dropdown, refresh_models_btn, pull_btn}),
        Container::Horizontal({whisper_size_dropdown, download_model_btn}),
        Container::Horizontal({mic_dropdown, monitor_dropdown}),
        refresh_devices_btn,
        name_input,
        db_input,
        Container::Horizontal({reminder_checkbox, time_input}),
        save_btn,
    });

    root_ = Renderer(layout, [this, model_dropdown, refresh_models_btn, pull_btn, whisper_size_dropdown,
                               download_model_btn, mic_dropdown, monitor_dropdown, refresh_devices_btn, name_input,
                               db_input, reminder_checkbox, time_input, save_btn] {
        if (models_need_refresh_.exchange(false)) refresh_models();

        std::string ollama_status_copy, pull_status_copy, whisper_status_copy;
        bool pulling_now, downloading_now;
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            ollama_status_copy = ollama_status_;
            pull_status_copy = pull_status_;
            whisper_status_copy = whisper_status_;
            pulling_now = pulling_;
            downloading_now = downloading_;
        }
        std::string cuda_note =
            transcription::cuda_build_available() ? "CUDA build detected" : "No CUDA in this build (CPU)";

        Elements children;
        children.push_back(text("CallScribe -- Settings") | bold);
        children.push_back(separator());

        children.push_back(text("Ollama model:") | bold);
        children.push_back(hbox({
            model_dropdown->Render() | flex,
            text(" "),
            refresh_models_btn->Render(),
            text(" "),
            pull_btn->Render(),
        }));
        if (!ollama_status_copy.empty()) children.push_back(text(ollama_status_copy) | dim);
        if (pulling_now || !pull_status_copy.empty()) children.push_back(text(pull_status_copy) | dim);

        children.push_back(separator());
        children.push_back(text("Whisper model size (" + cuda_note + "):") | bold);
        children.push_back(hbox({whisper_size_dropdown->Render(), text(" "), download_model_btn->Render()}));
        if (downloading_now || !whisper_status_copy.empty()) children.push_back(text(whisper_status_copy) | dim);

        children.push_back(separator());
        children.push_back(text("Audio sources:") | bold);
        children.push_back(hbox({text("Mic:     "), mic_dropdown->Render() | flex}));
        children.push_back(hbox({text("Monitor: "), monitor_dropdown->Render() | flex}));
        children.push_back(refresh_devices_btn->Render());

        children.push_back(separator());
        children.push_back(hbox({text("Your name (labels your side of the call): "), name_input->Render() | flex}));
        children.push_back(hbox({text("Database file: "), db_input->Render() | flex}));
        children.push_back(hbox({reminder_checkbox->Render(), text("   Time: "),
                                  time_input->Render() | size(WIDTH, EQUAL, 8)}));

        children.push_back(separator());
        children.push_back(save_btn->Render());

        return vbox(children) | flex | frame;
    });
}

}  // namespace callscribe::ui
