#include "ui/settings_screen.hpp"

#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <cstdio>

#include "audio/devices.hpp"
#include "config.hpp"
#include "llm/ollama_client.hpp"
#include "transcription/diarizer.hpp"
#include "transcription/whisper_engine.hpp"
#include "ui/level_meter.hpp"
#include "ui/styled_input.hpp"
#include "ui/theme.hpp"
#include "ui/wrapped_input.hpp"

namespace callscribe::ui {

namespace {

std::string format_bytes(int64_t bytes) {
    char buf[32];
    if (bytes >= 1024 * 1024)
        std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    else
        std::snprintf(buf, sizeof(buf), "%.0f KB", static_cast<double>(bytes) / 1024.0);
    return buf;
}

// A progress bar for one transfer. Servers do not always send a content
// length, so "how far along" is not always answerable -- in that case the
// bytes so far are shown on their own rather than a bar pretending to know.
ftxui::Element download_progress(int64_t done, int64_t total) {
    using namespace ftxui;
    if (total <= 0) return text("  " + format_bytes(done) + " downloaded...") | color(theme::kAccent);
    float fraction = std::min(1.0f, static_cast<float>(done) / static_cast<float>(total));
    return hbox({
        text("  "),
        gauge(fraction) | color(theme::kAccent) | size(WIDTH, EQUAL, 30),
        text(" " + std::to_string(static_cast<int>(fraction * 100)) + "%  ") | color(theme::kAccent),
        text(format_bytes(done) + " / " + format_bytes(total)) | dim,
    });
}

}  // namespace

SettingsScreen::SettingsScreen(AppContext& ctx) : ctx_(ctx) {
    ollama_keep_alive_ = ctx_.settings.ollama_keep_alive;
    ollama_unload_after_use_ = ctx_.settings.ollama_unload_after_use;
    user_display_name_ = ctx_.settings.user_display_name;
    db_path_ = ctx_.settings.db_path;
    reminder_time_ = ctx_.settings.reminder_time;
    reminder_enabled_ = ctx_.settings.reminder_enabled;
    echo_cancel_ = ctx_.settings.echo_cancel;
    detect_speaker_turns_ = ctx_.settings.detect_speaker_turns;
    diarize_far_end_ = ctx_.settings.diarize_far_end;
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
    if (diarization_download_thread_.joinable()) diarization_download_thread_.join();
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

void SettingsScreen::unload_now() {
    llm::unload_model(ctx_.settings.ollama_host, ctx_.settings.ollama_model);
    std::lock_guard<std::mutex> lock(status_mutex_);
    ollama_status_ = ctx_.settings.ollama_model.empty()
                          ? "No model selected to unload."
                          : ("Asked Ollama to unload " + ctx_.settings.ollama_model + ".");
}

void SettingsScreen::download_model() {
    if (downloading_.load()) return;
    std::string size = whisper_sizes_[whisper_size_selected_];
    if (transcription::model_present(size)) return;
    if (download_thread_.joinable()) download_thread_.join();

    downloading_ = true;
    whisper_bytes_ = 0;
    whisper_bytes_total_ = 0;
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        whisper_status_ = "Downloading ggml-" + size + ".bin...";
    }
    download_thread_ = std::thread([this, size] {
        std::string status;
        try {
            // The lock is taken only to publish the final message. Holding it
            // across the transfer would block every render for the length of
            // the download -- the renderer takes this same mutex -- which is
            // exactly the freeze a progress bar is supposed to rule out.
            transcription::ensure_model_downloaded(size, [this](int64_t done, int64_t total) {
                whisper_bytes_.store(done);
                whisper_bytes_total_.store(total);
            });
            status = "Download complete.";
        } catch (const std::exception& e) {
            status = std::string("Download failed: ") + e.what();
        }
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            whisper_status_ = status;
        }
        downloading_ = false;
    });
}

void SettingsScreen::download_diarization_models() {
    if (downloading_diarization_.load()) return;
    if (transcription::diarization_models_present()) return;
    if (diarization_download_thread_.joinable()) diarization_download_thread_.join();

    downloading_diarization_ = true;
    diarization_bytes_ = 0;
    diarization_bytes_total_ = 0;
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        diarization_status_ = "Downloading speaker models (" +
                               transcription::diarization_models_size_note() + ")...";
        diarization_file_label_.clear();
    }
    diarization_download_thread_ = std::thread([this] {
        std::string status;
        try {
            transcription::ensure_diarization_models_downloaded(
                [this](const std::string& label, int index, int count, int64_t done, int64_t total) {
                    diarization_bytes_.store(done);
                    diarization_bytes_total_.store(total);
                    // The label only changes when the file does, so the string
                    // (and its lock) is touched once per file rather than on
                    // every progress tick.
                    std::string next = label + " (" + std::to_string(index) + " of " +
                                        std::to_string(count) + ")";
                    std::lock_guard<std::mutex> lock(status_mutex_);
                    if (diarization_file_label_ != next) diarization_file_label_ = next;
                });
            status = "Speaker models ready.";
        } catch (const std::exception& e) {
            status = std::string("Download failed: ") + e.what();
        }
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            diarization_status_ = status;
            diarization_file_label_.clear();
        }
        downloading_diarization_ = false;
    });
}

void SettingsScreen::save() {
    ctx_.settings.ollama_model = (ollama_model_selected_ >= 0 && ollama_model_selected_ < static_cast<int>(ollama_models_.size()))
                                      ? ollama_models_[ollama_model_selected_]
                                      : "";
    ctx_.settings.whisper_model_size = whisper_sizes_[whisper_size_selected_];
    ctx_.settings.detect_speaker_turns = detect_speaker_turns_;
    ctx_.settings.diarize_far_end = diarize_far_end_;
    ctx_.settings.mic_source =
        (mic_selected_ >= 0 && mic_selected_ < static_cast<int>(mic_names_.size())) ? mic_names_[mic_selected_] : "";
    ctx_.settings.monitor_source = (monitor_selected_ >= 0 && monitor_selected_ < static_cast<int>(monitor_names_.size()))
                                        ? monitor_names_[monitor_selected_]
                                        : "";
    // Blank means "leave it to the server's OLLAMA_KEEP_ALIVE", which is the
    // behaviour this setting exists to override -- so fall back to the default.
    ctx_.settings.ollama_keep_alive = ollama_keep_alive_.empty() ? kDefaultOllamaKeepAlive : ollama_keep_alive_;
    ctx_.settings.ollama_unload_after_use = ollama_unload_after_use_;
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

    InputOption keep_alive_opt = styled_input_option();
    keep_alive_opt.multiline = false;
    auto keep_alive_input = WrappedInput(&ollama_keep_alive_, kDefaultOllamaKeepAlive, keep_alive_opt);
    auto unload_btn = Button("Unload now", [this] { unload_now(); });
    auto unload_after_use_checkbox =
        Checkbox("Unload the model as soon as each summary or reply is done", &ollama_unload_after_use_);

    auto whisper_size_dropdown = Dropdown(&whisper_sizes_, &whisper_size_selected_);
    auto download_model_btn = Button("Download model", [this] { download_model(); });
    auto speaker_turns_checkbox =
        Checkbox("Split the far end into speaker turns (tinydiarize)", &detect_speaker_turns_);
    auto diarize_checkbox =
        Checkbox("Identify far-end speakers after the call (recommended)", &diarize_far_end_);
    auto download_diarization_btn =
        Button("Download speaker models", [this] { download_diarization_models(); });

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
        Container::Horizontal({keep_alive_input, unload_btn}),
        unload_after_use_checkbox,
        // Maybe, so a button that isn't drawn (already installed, or a
        // download in flight) also stops being a Tab stop.
        Container::Horizontal({whisper_size_dropdown, Maybe(download_model_btn, [this] {
                                    return !downloading_.load() &&
                                            !transcription::model_present(
                                                whisper_sizes_[whisper_size_selected_]);
                                })}),
        speaker_turns_checkbox,
        Container::Horizontal({diarize_checkbox, Maybe(download_diarization_btn, [this] {
                                    return !downloading_diarization_.load() &&
                                            !transcription::diarization_models_present();
                                })}),
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

    root_ = Renderer(layout, [this, model_dropdown, refresh_models_btn, pull_btn, keep_alive_input, unload_btn,
                              unload_after_use_checkbox,
                               whisper_size_dropdown,
                               download_model_btn, speaker_turns_checkbox, diarize_checkbox,
                               download_diarization_btn, mic_dropdown, monitor_dropdown, refresh_devices_btn, test_mic_btn,
                               test_speaker_btn, name_input, db_input, reminder_checkbox, time_input, echo_cancel_checkbox,
                               summary_prompt_input, email_prompt_input, tasks_prompt_input, reset_prompts_btn,
                               save_btn] {
        if (models_need_refresh_.exchange(false)) refresh_models();

        if (testing_.load() &&
            std::chrono::steady_clock::now() - test_started_at_ > std::chrono::seconds(10)) {
            stop_test();  // safety timeout so a forgotten test doesn't keep capturing forever
        }

        // Read once per frame: diarization_models_present() and model_present()
        // both stat the filesystem, and each is asked about more than once below.
        const bool diarization_installed = transcription::diarization_models_present();
        const bool downloading_diarization_now = downloading_diarization_.load();
        const bool whisper_model_installed =
            transcription::model_present(whisper_sizes_[whisper_size_selected_]);

        std::string ollama_status_copy, pull_status_copy, whisper_status_copy, diarization_status_copy;
        std::string diarization_file_copy;
        bool pulling_now, downloading_now;
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            ollama_status_copy = ollama_status_;
            pull_status_copy = pull_status_;
            whisper_status_copy = whisper_status_;
            diarization_status_copy = diarization_status_;
            diarization_file_copy = diarization_file_label_;
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

        children.push_back(hbox({text("Keep model loaded for: "), keep_alive_input->Render() | size(WIDTH, EQUAL, 10),
                                  text(" "), unload_btn->Render()}));
        children.push_back(text("  How long Ollama holds the model in memory after a call (10m, 1h, 0, -1).") | dim);
        children.push_back(text("  Shorter frees VRAM for Whisper, which otherwise transcribes on CPU.") | dim);
        children.push_back(unload_after_use_checkbox->Render());
        children.push_back(text("  On: the model is loaded for the job and dropped straight after, so it never") | dim);
        children.push_back(text("  sits alongside Whisper or another Ollama client. Costs a reload each time.") | dim);

        children.push_back(separator() | color(theme::kMuted));
        children.push_back(text("Whisper model size (" + cuda_note + "):") | bold | color(theme::kAccent));
        Elements whisper_row = {whisper_size_dropdown->Render(), text(" ")};
        if (downloading_now)
            whisper_row.push_back(text(" Downloading... ") | color(theme::kWarning));
        else if (whisper_model_installed)
            whisper_row.push_back(text(" ✓ Installed ") | color(theme::kSuccess));
        else
            whisper_row.push_back(download_model_btn->Render());
        children.push_back(hbox(std::move(whisper_row)));
        if (downloading_now)
            children.push_back(download_progress(whisper_bytes_.load(), whisper_bytes_total_.load()));
        if (!whisper_status_copy.empty()) children.push_back(text(whisper_status_copy) | dim);

        // Diarization first: it is the better answer for "who said that", works
        // with any Whisper model, and the tinydiarize option below is the
        // fallback for wanting it live rather than after the call.
        children.push_back(separator() | color(theme::kMuted));
        children.push_back(text("Speaker identification (far end):") | bold | color(theme::kAccent));
        Elements diarize_row = {diarize_checkbox->Render(), text("  ")};
        if (downloading_diarization_now)
            diarize_row.push_back(text(" Downloading... ") | color(theme::kWarning));
        else if (diarization_installed)
            diarize_row.push_back(text(" ✓ Installed ") | color(theme::kSuccess));
        else
            diarize_row.push_back(download_diarization_btn->Render());
        children.push_back(hbox(std::move(diarize_row)));
        if (downloading_diarization_now) {
            if (!diarization_file_copy.empty())
                children.push_back(text("  " + diarization_file_copy) | color(theme::kAccent));
            children.push_back(download_progress(diarization_bytes_.load(), diarization_bytes_total_.load()));
        }
        children.push_back(text("  Runs two small models over the call recording when it ends and rewrites") | dim);
        children.push_back(text("  the far-end names. Local, no account, nothing uploaded. Participants are") | dim);
        children.push_back(text("  matched in the order they first speak; extra voices become Speaker 2, 3...") | dim);
        if (!diarization_installed && !downloading_diarization_now)
            children.push_back(text("  Models not downloaded yet (" +
                                     transcription::diarization_models_size_note() + ").") |
                                color(theme::kWarning));
        if (!diarization_status_copy.empty()) children.push_back(text("  " + diarization_status_copy) | dim);

        children.push_back(separator() | color(theme::kMuted));
        children.push_back(speaker_turns_checkbox->Render());
        // Said plainly, because the feature is easy to over-read: it marks that
        // the voice changed, and the participant list supplies names in the
        // order people first speak. It cannot recognise who is talking.
        if (transcription::model_supports_speaker_turns(whisper_sizes_[whisper_size_selected_])) {
            children.push_back(text("  Marks where the far-end speaker changes; participants are named in the") | dim);
            children.push_back(text("  order they first speak, then Speaker 2, Speaker 3, ... It does not") | dim);
            children.push_back(text("  recognise voices, and small.en-tdrz is English-only.") | dim);
        } else {
            children.push_back(text("  Needs the small.en-tdrz model above -- no other model can do this.") |
                                color(theme::kWarning));
            children.push_back(text("  That model is an English-only 'small', so it transcribes less accurately") | dim);
            children.push_back(text("  than medium/large. Your own mic is a separate stream either way.") | dim);
        }

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
