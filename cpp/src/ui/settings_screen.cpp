#include "ui/settings_screen.hpp"

#include <ftxui/dom/elements.hpp>

#include "audio/devices.hpp"
#include "config.hpp"
#include "llm/ollama_client.hpp"
#include "transcription/whisper_engine.hpp"
#include "ui/level_meter.hpp"
#include "ui/styled_input.hpp"
#include "ui/theme.hpp"
#include "ui/wrapped_input.hpp"

namespace callscribe::ui {

SettingsScreen::SettingsScreen(AppContext& ctx) : ctx_(ctx) {
    user_display_name_ = ctx_.settings.user_display_name;
    db_path_ = ctx_.settings.db_path;
    reminder_time_ = ctx_.settings.reminder_time;
    reminder_enabled_ = ctx_.settings.reminder_enabled;
    echo_cancel_ = ctx_.settings.echo_cancel;
    summary_prompt_ = ctx_.settings.summary_prompt;
    email_prompt_ = ctx_.settings.email_prompt;
    tasks_prompt_ = ctx_.settings.tasks_prompt;

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
    stop_test();
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

void SettingsScreen::stop_test() {
    if (test_capture_) {
        test_capture_->stop();
        test_capture_.reset();
    }
    testing_ = false;
}

void SettingsScreen::start_mic_test() {
    stop_test();
    std::string source = (mic_selected_ >= 0 && mic_selected_ < static_cast<int>(mic_names_.size()))
                              ? mic_names_[mic_selected_]
                              : "";
    if (source.empty()) source = audio::default_mic_source();
    test_capture_ = std::make_unique<audio::StreamCapture>(source);
    try {
        test_capture_->start();
        testing_label_ = "Mic";
        testing_ = true;
        test_started_at_ = std::chrono::steady_clock::now();
    } catch (const std::exception& e) {
        test_capture_.reset();
        std::lock_guard<std::mutex> lock(status_mutex_);
        ollama_status_ = std::string("Mic test failed: ") + e.what();
    }
}

void SettingsScreen::start_monitor_test() {
    stop_test();
    std::string source = (monitor_selected_ >= 0 && monitor_selected_ < static_cast<int>(monitor_names_.size()))
                              ? monitor_names_[monitor_selected_]
                              : "";
    if (source.empty()) source = audio::default_monitor_source();
    test_capture_ = std::make_unique<audio::StreamCapture>(source);
    try {
        test_capture_->start();
        testing_label_ = "Speaker";
        testing_ = true;
        test_started_at_ = std::chrono::steady_clock::now();
    } catch (const std::exception& e) {
        test_capture_.reset();
        std::lock_guard<std::mutex> lock(status_mutex_);
        ollama_status_ = std::string("Speaker test failed: ") + e.what();
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
    ctx_.settings.echo_cancel = echo_cancel_;
    // Blank means "use the built-in wording" rather than "ask for nothing".
    ctx_.settings.summary_prompt = summary_prompt_.empty() ? kDefaultSummaryPrompt : summary_prompt_;
    ctx_.settings.email_prompt = email_prompt_.empty() ? kDefaultEmailPrompt : email_prompt_;
    ctx_.settings.tasks_prompt = tasks_prompt_.empty() ? kDefaultTasksPrompt : tasks_prompt_;

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
    auto test_mic_btn = Button(
        "Test Mic",
        [this] {
            if (testing_ && testing_label_ == "Mic")
                stop_test();
            else
                start_mic_test();
        },
        ButtonOption::Animated(theme::kAccent));
    auto test_speaker_btn = Button(
        "Test Speaker",
        [this] {
            if (testing_ && testing_label_ == "Speaker")
                stop_test();
            else
                start_monitor_test();
        },
        ButtonOption::Animated(theme::kAccent));

    InputOption name_opt = styled_input_option();
    name_opt.multiline = false;
    auto name_input = WrappedInput(&user_display_name_, "Your name", name_opt);

    InputOption db_opt = styled_input_option();
    db_opt.multiline = false;
    auto db_input = WrappedInput(&db_path_, "Database file path", db_opt);

    auto reminder_checkbox = Checkbox("Daily reminder enabled", &reminder_enabled_);
    auto echo_cancel_checkbox = Checkbox("Echo cancellation (stop the mic hearing the speakers)", &echo_cancel_);
    InputOption time_opt = styled_input_option();
    time_opt.multiline = false;
    auto time_input = WrappedInput(&reminder_time_, "HH:MM", time_opt);

    // Prompt presets. Multiline so a longer instruction stays readable, and
    // wrapped in their own box below the rest of the settings.
    InputOption prompt_opt;
    prompt_opt.multiline = true;
    auto summary_prompt_input = WrappedInput(&summary_prompt_, kDefaultSummaryPrompt, prompt_opt);
    auto email_prompt_input = WrappedInput(&email_prompt_, kDefaultEmailPrompt, prompt_opt);
    auto tasks_prompt_input = WrappedInput(&tasks_prompt_, kDefaultTasksPrompt, prompt_opt);
    auto reset_prompts_btn = Button("Reset prompts to defaults", [this] {
        summary_prompt_ = kDefaultSummaryPrompt;
        email_prompt_ = kDefaultEmailPrompt;
        tasks_prompt_ = kDefaultTasksPrompt;
    });

    auto save_btn = Button("Save Settings", [this] { save(); }, ButtonOption::Animated(theme::kAccent));

    auto layout = Container::Vertical({
        Container::Horizontal({model_dropdown, refresh_models_btn, pull_btn}),
        Container::Horizontal({whisper_size_dropdown, download_model_btn}),
        Container::Horizontal({mic_dropdown, monitor_dropdown}),
        Container::Horizontal({refresh_devices_btn, test_mic_btn, test_speaker_btn}),
        name_input,
        db_input,
        Container::Horizontal({reminder_checkbox, time_input}),
        echo_cancel_checkbox,
        summary_prompt_input,
        email_prompt_input,
        tasks_prompt_input,
        reset_prompts_btn,
        save_btn,
    });

    root_ = Renderer(layout, [this, model_dropdown, refresh_models_btn, pull_btn, whisper_size_dropdown,
                               download_model_btn, mic_dropdown, monitor_dropdown, refresh_devices_btn, test_mic_btn,
                               test_speaker_btn, name_input, db_input, reminder_checkbox, time_input, echo_cancel_checkbox,
                               summary_prompt_input, email_prompt_input, tasks_prompt_input, reset_prompts_btn,
                               save_btn] {
        if (models_need_refresh_.exchange(false)) refresh_models();

        if (testing_.load() &&
            std::chrono::steady_clock::now() - test_started_at_ > std::chrono::seconds(10)) {
            stop_test();  // safety timeout so a forgotten test doesn't keep capturing forever
        }

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
        children.push_back(text("CallScribe -- Settings") | bold | color(theme::kAccent));
        children.push_back(separator() | color(theme::kMuted));

        children.push_back(text("Ollama model:") | bold | color(theme::kAccent));
        children.push_back(hbox({
            model_dropdown->Render() | flex,
            text(" "),
            refresh_models_btn->Render(),
            text(" "),
            pull_btn->Render(),
        }));
        if (!ollama_status_copy.empty()) children.push_back(text(ollama_status_copy) | dim);
        if (pulling_now || !pull_status_copy.empty()) children.push_back(text(pull_status_copy) | dim);

        children.push_back(separator() | color(theme::kMuted));
        children.push_back(text("Whisper model size (" + cuda_note + "):") | bold | color(theme::kAccent));
        children.push_back(hbox({whisper_size_dropdown->Render(), text(" "), download_model_btn->Render()}));
        if (downloading_now || !whisper_status_copy.empty()) children.push_back(text(whisper_status_copy) | dim);

        children.push_back(separator() | color(theme::kMuted));
        children.push_back(text("Audio sources:") | bold | color(theme::kAccent));
        children.push_back(hbox({text("Mic:     "), mic_dropdown->Render() | flex}));
        children.push_back(hbox({text("Monitor: "), monitor_dropdown->Render() | flex}));
        children.push_back(hbox({refresh_devices_btn->Render(), text(" "), test_mic_btn->Render(), text(" "),
                                  test_speaker_btn->Render()}));

        bool testing_now = testing_.load();
        if (testing_now) {
            std::string hint = testing_label_ == "Mic" ? "speak into your mic now..."
                                                         : "play some audio (e.g. a video) now to test capture...";
            children.push_back(hbox({text("Testing " + testing_label_ + ": ") | bold | color(theme::kAccent),
                                      text(hint) | dim}));
            children.push_back(gauge(level_to_progress(test_capture_->current_level())) | color(theme::kDanger) |
                                flex);
        }

        children.push_back(separator() | color(theme::kMuted));
        children.push_back(hbox({text("Your name (labels your side of the call): "), name_input->Render() | flex}));
        children.push_back(hbox({text("Database file: "), db_input->Render() | flex}));
        children.push_back(hbox({reminder_checkbox->Render(), text("   Time: "),
                                  time_input->Render() | size(WIDTH, EQUAL, 8)}));

        children.push_back(echo_cancel_checkbox->Render());
        children.push_back(text("  While recording, audio is routed through a CallScribe device so the") | dim);
        children.push_back(text("  mic can be cleaned. Your normal output is restored when it stops.") | dim);

        children.push_back(separator() | color(theme::kMuted));
        children.push_back(text("AI prompts") | bold | color(theme::kAccent));
        children.push_back(text("What to ask the model for. The reply format is fixed; this is the wording.") | dim);
        children.push_back(text("Summary:"));
        children.push_back(summary_prompt_input->Render() | size(HEIGHT, LESS_THAN, 4) |
                            borderStyled(ROUNDED, theme::kMuted));
        children.push_back(text("Follow-up email:"));
        children.push_back(email_prompt_input->Render() | size(HEIGHT, LESS_THAN, 4) |
                            borderStyled(ROUNDED, theme::kMuted));
        children.push_back(text("Tasks:"));
        children.push_back(tasks_prompt_input->Render() | size(HEIGHT, LESS_THAN, 4) |
                            borderStyled(ROUNDED, theme::kMuted));
        children.push_back(reset_prompts_btn->Render());

        children.push_back(separator() | color(theme::kMuted));
        children.push_back(save_btn->Render());

        return vbox(children) | flex | frame;
    });
}

}  // namespace callscribe::ui
