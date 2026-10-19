#include "ui/settings_screen.hpp"

#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <cstdio>

#include "audio/devices.hpp"
#include "config.hpp"
#include "focus/pomodoro.hpp"
#include "llm/ollama_client.hpp"
#include "spotify/pkce.hpp"
#include "transcription/diarizer.hpp"
#include "transcription/whisper_engine.hpp"
#include "ui/level_meter.hpp"
#include "ui/scroll_pane.hpp"
#include "ui/styled_input.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"
#include "ui/wrapped_input.hpp"
#include "util/time.hpp"

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
// Tolerant of stray spaces and of the field being cleared mid-edit; anything
// unparseable keeps `fallback`.
int parse_int(const std::string& text, int fallback) {
    try {
        size_t consumed = 0;
        int value = std::stoi(text, &consumed);
        return consumed == 0 ? fallback : value;
    } catch (const std::exception&) {
        return fallback;
    }
}

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
    llm_backend_index_ = ctx_.settings.uses_llamacpp() ? 1 : 0;
    llamacpp_host_ = ctx_.settings.llamacpp_host;
    llamacpp_api_key_ = ctx_.llamacpp_api_key();
    focus_minutes_ = std::to_string(ctx_.settings.focus_minutes);
    short_break_minutes_ = std::to_string(ctx_.settings.short_break_minutes);
    long_break_minutes_ = std::to_string(ctx_.settings.long_break_minutes);
    hyperfocus_minutes_ = std::to_string(ctx_.settings.hyperfocus_nudge_minutes);
    meeting_reminder_minutes_ = std::to_string(ctx_.settings.meeting_reminder_minutes);
    end_of_day_time_ = ctx_.settings.end_of_day_time;
    end_of_day_enabled_ = ctx_.settings.end_of_day_enabled;
    spotify_client_id_ = ctx_.settings.spotify_client_id;
    spotify_port_ = std::to_string(ctx_.settings.spotify_redirect_port);
    spotify_playlist_ = ctx_.settings.spotify_playlist_uri;
    summary_prompt_ = ctx_.settings.summary_prompt;
    email_prompt_ = ctx_.settings.email_prompt;
    tasks_prompt_ = ctx_.settings.tasks_prompt;

    std::string initial_size =
        ctx_.settings.whisper_model_size.empty() ? transcription::suggest_model_size() : ctx_.settings.whisper_model_size;
    for (size_t i = 0; i < whisper_sizes_.size(); ++i) {
        if (whisper_sizes_[i] == initial_size) whisper_size_selected_ = static_cast<int>(i);
    }

    build_ui();
    if (ctx_.settings.uses_llamacpp())
        check_llamacpp();  // also learns the context size before the first long call
    else
        refresh_models();
    refresh_audio_devices();
}

SettingsScreen::~SettingsScreen() {
    stop_test();
    if (pull_thread_.joinable()) pull_thread_.join();
    if (download_thread_.joinable()) download_thread_.join();
    if (diarization_download_thread_.joinable()) diarization_download_thread_.join();
}

void SettingsScreen::check_llamacpp() {
    ctx_.settings.llm_backend = "llamacpp";
    ctx_.settings.llamacpp_host = llamacpp_host_.empty() ? kLlamaCppHostDefault : llamacpp_host_;
    llamacpp_host_ = ctx_.settings.llamacpp_host;
    ctx_.save_settings();
    // First time llama.cpp is picked this run: the stored key hasn't been read.
    ctx_.load_llamacpp_api_key();
    if (llamacpp_api_key_.empty()) llamacpp_api_key_ = ctx_.llamacpp_api_key();
    if (std::string err = ctx_.set_llamacpp_api_key(llamacpp_api_key_); !err.empty()) {
        llamacpp_status_ = err;
        return;
    }
    try {
        auto models = llm::list_models(ctx_.settings.llamacpp_host);
        long context = llm::refresh_server_context(ctx_.settings.llamacpp_host);
        ctx_.settings.llamacpp_model = models.empty() ? "" : models.front();
        ctx_.save_settings();
        llamacpp_status_ = "Connected" + (models.empty() ? std::string() : " -- " + models.front()) +
                           (context > 0 ? " · " + std::to_string(context) + "-token context" : "");
    } catch (const std::exception& e) {
        llamacpp_status_ = "Can't reach llama-server at " + ctx_.settings.llamacpp_host + ": " + e.what();
    }
}

void SettingsScreen::refresh_models() {
    // The Ollama list is asked of Ollama even while llama.cpp is selected:
    // it's the Ollama section being refreshed.
    if (ctx_.settings.uses_llamacpp()) return;
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

void SettingsScreen::apply_focus_and_spotify_fields() {
    auto minutes = [](std::string& field, int& target) {
        target = std::clamp(parse_int(field, target), focus::kMinMinutes, focus::kMaxMinutes);
        field = std::to_string(target);  // show what will actually run
    };
    minutes(focus_minutes_, ctx_.settings.focus_minutes);
    minutes(short_break_minutes_, ctx_.settings.short_break_minutes);
    minutes(long_break_minutes_, ctx_.settings.long_break_minutes);
    ctx_.settings.hyperfocus_nudge_minutes =
        std::clamp(parse_int(hyperfocus_minutes_, ctx_.settings.hyperfocus_nudge_minutes), 0, 600);
    hyperfocus_minutes_ = std::to_string(ctx_.settings.hyperfocus_nudge_minutes);
    if (!end_of_day_time_.empty()) ctx_.settings.end_of_day_time = end_of_day_time_;
    ctx_.settings.end_of_day_enabled = end_of_day_enabled_;
    ctx_.reminder->set_end_of_day(ctx_.settings.end_of_day_time, ctx_.settings.end_of_day_enabled);
    ctx_.settings.meeting_reminder_minutes =
        std::clamp(parse_int(meeting_reminder_minutes_, ctx_.settings.meeting_reminder_minutes), 0, 120);
    meeting_reminder_minutes_ = std::to_string(ctx_.settings.meeting_reminder_minutes);
    ctx_.meeting_reminder->set_lead_minutes(ctx_.settings.meeting_reminder_minutes);

    auto trimmed = [](const std::string& v) {
        size_t b = v.find_first_not_of(" \t");
        return b == std::string::npos ? std::string() : v.substr(b, v.find_last_not_of(" \t") - b + 1);
    };
    spotify_client_id_ = trimmed(spotify_client_id_);
    ctx_.settings.spotify_client_id = spotify_client_id_;
    ctx_.settings.spotify_redirect_port =
        std::clamp(parse_int(spotify_port_, ctx_.settings.spotify_redirect_port), 1024, 65535);
    spotify_port_ = std::to_string(ctx_.settings.spotify_redirect_port);
    // Stored as the canonical URI, so a pasted share link (?si=...) is tidied
    // up once here rather than on every play.
    std::string playlist = trimmed(spotify_playlist_);
    std::string uri = spotify::normalize_playlist_uri(playlist);
    if (!playlist.empty() && uri.empty()) {
        spotify_note_ = "That doesn't look like a Spotify playlist link -- it was not saved.";
    } else {
        spotify_note_.clear();
        spotify_playlist_ = uri;
        ctx_.settings.spotify_playlist_uri = uri;
    }
}

void SettingsScreen::save() {
    ctx_.settings.ollama_model = (ollama_model_selected_ >= 0 && ollama_model_selected_ < static_cast<int>(ollama_models_.size()))
                                      ? ollama_models_[ollama_model_selected_]
                                      : "";
    ctx_.settings.llm_backend = llm_backend_index_ == 1 ? "llamacpp" : "ollama";
    ctx_.settings.llamacpp_host = llamacpp_host_.empty() ? kLlamaCppHostDefault : llamacpp_host_;
    if (llm_backend_index_ == 1) ctx_.set_llamacpp_api_key(llamacpp_api_key_);
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
    apply_focus_and_spotify_fields();

    // Blank means "use the built-in wording" rather than "ask for nothing".
    ctx_.settings.summary_prompt = summary_prompt_.empty() ? kDefaultSummaryPrompt : summary_prompt_;
    ctx_.settings.email_prompt = email_prompt_.empty() ? kDefaultEmailPrompt : email_prompt_;
    ctx_.settings.tasks_prompt = tasks_prompt_.empty() ? kDefaultTasksPrompt : tasks_prompt_;

    ctx_.save_settings();
    ctx_.reload_whisper();
    ctx_.reminder->set_reminder_time(ctx_.settings.reminder_time);
    ctx_.reminder->set_enabled(ctx_.settings.reminder_enabled);

    std::lock_guard<std::mutex> lock(status_mutex_);
    save_status_ = "Saved at " + util::now_time_hhmm() + "." +
                   std::string(db_path_changed ? " Restart CallScribe for the new database path." : "") +
                   " Whisper model reloading in the background.";
}

void SettingsScreen::build_ui() {
    using namespace ftxui;

    MenuOption backend_opt = MenuOption::Horizontal();
    backend_opt.on_change = [this] {
        // Applied straight away rather than on Save: the sections below are
        // about the server picked here, so they should talk to it.
        if (llm_backend_index_ == 1) {
            check_llamacpp();
        } else {
            ctx_.settings.llm_backend = "ollama";
            ctx_.save_settings();
            refresh_models();
        }
    };
    auto backend_toggle = Menu(&llm_backends_, &llm_backend_index_, backend_opt);
    InputOption llama_host_opt = styled_input_option();
    llama_host_opt.multiline = false;
    llama_host_opt.on_enter = [this] { check_llamacpp(); };
    auto llama_host_input = WrappedInput(&llamacpp_host_, kLlamaCppHostDefault, llama_host_opt);
    InputOption api_key_opt = styled_input_option();
    api_key_opt.multiline = false;
    api_key_opt.password = true;
    api_key_opt.on_enter = [this] { check_llamacpp(); };
    auto llama_key_input = WrappedInput(&llamacpp_api_key_, "only if started with --api-key", api_key_opt);
    auto llama_check_btn = Button("Check", [this] { check_llamacpp(); }, button_style(theme::kAccent));

    auto model_dropdown = Dropdown(&ollama_models_, &ollama_model_selected_);
    auto refresh_models_btn = Button("Refresh", [this] { refresh_models(); }, button_style(theme::kMuted));
    auto pull_btn = Button(std::string("Pull ") + kRecommendedOllamaModel, [this] { pull_recommended(); },
                           button_style(theme::kMuted));

    InputOption keep_alive_opt = styled_input_option();
    keep_alive_opt.multiline = false;
    auto keep_alive_input = WrappedInput(&ollama_keep_alive_, kDefaultOllamaKeepAlive, keep_alive_opt);
    auto unload_btn = Button("Unload now", [this] { unload_now(); }, button_style(theme::kMuted));
    auto unload_after_use_checkbox =
        Checkbox("Unload the model as soon as each summary or reply is done", &ollama_unload_after_use_);

    auto whisper_size_dropdown = Dropdown(&whisper_sizes_, &whisper_size_selected_);
    auto download_model_btn = Button("Download model", [this] { download_model(); }, button_style(theme::kAccent));
    auto speaker_turns_checkbox =
        Checkbox("Split the far end into speaker turns (tinydiarize)", &detect_speaker_turns_);
    auto diarize_checkbox =
        Checkbox("Identify far-end speakers after the call (recommended)", &diarize_far_end_);
    auto download_diarization_btn =
        Button("Download speaker models", [this] { download_diarization_models(); },
               button_style(theme::kAccent));

    auto mic_dropdown = Dropdown(&mic_labels_, &mic_selected_);
    auto monitor_dropdown = Dropdown(&monitor_labels_, &monitor_selected_);
    auto refresh_devices_btn = Button("Refresh audio devices", [this] { refresh_audio_devices(); }, button_style(theme::kMuted));
    auto test_mic_btn = Button(
        "Test Mic",
        [this] {
            if (testing_ && testing_label_ == "Mic")
                stop_test();
            else
                start_mic_test();
        },
        button_style(theme::kAccent));
    auto test_speaker_btn = Button(
        "Test Speaker",
        [this] {
            if (testing_ && testing_label_ == "Speaker")
                stop_test();
            else
                start_monitor_test();
        },
        button_style(theme::kAccent));

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

    InputOption minutes_opt = styled_input_option();
    minutes_opt.multiline = false;
    auto focus_minutes_input = WrappedInput(&focus_minutes_, "25", minutes_opt);
    auto short_break_input = WrappedInput(&short_break_minutes_, "5", minutes_opt);
    auto long_break_input = WrappedInput(&long_break_minutes_, "15", minutes_opt);
    auto hyperfocus_input = WrappedInput(&hyperfocus_minutes_, "90", minutes_opt);
    auto meeting_minutes_input = WrappedInput(&meeting_reminder_minutes_, "15", minutes_opt);
    auto eod_checkbox = Checkbox("Remind me to log the day (skipped once it's saved)", &end_of_day_enabled_);
    auto eod_time_input = WrappedInput(&end_of_day_time_, "17:30", minutes_opt);

    InputOption spotify_opt = styled_input_option();
    spotify_opt.multiline = false;
    auto spotify_client_input = WrappedInput(&spotify_client_id_, "Client ID from developer.spotify.com", spotify_opt);
    auto spotify_port_input = WrappedInput(&spotify_port_, "8888", spotify_opt);
    auto spotify_playlist_input =
        WrappedInput(&spotify_playlist_, "spotify:playlist:... or an open.spotify.com link", spotify_opt);
    auto spotify_connect_btn = Button(
        "Connect",
        [this] {
            // Saved first, so the login uses what's in the fields right now.
            apply_focus_and_spotify_fields();
            ctx_.save_settings();
            if (ctx_.settings.spotify_client_id.empty())
                spotify_note_ = "Enter your Spotify app's Client ID first.";
            else
                ctx_.spotify->connect();
        },
        button_style(theme::kSuccess));
    auto spotify_cancel_btn = Button("Cancel login", [this] { ctx_.spotify->cancel_login(); },
                                     button_style(theme::kMuted));
    auto spotify_disconnect_btn =
        Button("Disconnect", [this] { ctx_.spotify->disconnect(); }, button_style(theme::kMuted));

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
    }, button_style(theme::kMuted));

    auto save_btn = Button("Save Settings", [this] { save(); }, button_style(theme::kAccent));

    auto ollama_section = Container::Vertical({
        Container::Horizontal({model_dropdown, refresh_models_btn, pull_btn}),
        Container::Horizontal({keep_alive_input, unload_btn}),
        unload_after_use_checkbox,
    });
    auto llama_section =
        Container::Vertical({Container::Horizontal({llama_host_input, llama_check_btn}), llama_key_input});

    auto layout = Container::Vertical({
        backend_toggle,
        Maybe(ollama_section, [this] { return llm_backend_index_ == 0; }),
        Maybe(llama_section, [this] { return llm_backend_index_ == 1; }),
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
        meeting_minutes_input,
        echo_cancel_checkbox,
        Container::Horizontal({focus_minutes_input, short_break_input, long_break_input}),
        hyperfocus_input,
        Container::Horizontal({eod_checkbox, eod_time_input}),
        spotify_client_input,
        spotify_port_input,
        spotify_playlist_input,
        // Only what is drawn is a Tab stop: Cancel while a login is waiting,
        // Connect/Disconnect otherwise.
        Container::Horizontal({
            Maybe(spotify_connect_btn, [this] { return !ctx_.spotify->logging_in(); }),
            Maybe(spotify_cancel_btn, [this] { return ctx_.spotify->logging_in(); }),
            Maybe(spotify_disconnect_btn,
                  [this] { return !ctx_.spotify->logging_in() && ctx_.spotify->connected(); }),
        }),
        summary_prompt_input,
        email_prompt_input,
        tasks_prompt_input,
        reset_prompts_btn,
    });

    root_ = Renderer(layout, [this, backend_toggle, llama_host_input, llama_key_input, llama_check_btn, model_dropdown,
                              refresh_models_btn, pull_btn, keep_alive_input, unload_btn, unload_after_use_checkbox,
                               whisper_size_dropdown,
                               download_model_btn, speaker_turns_checkbox, diarize_checkbox,
                               download_diarization_btn, mic_dropdown, monitor_dropdown, refresh_devices_btn, test_mic_btn,
                               test_speaker_btn, name_input, db_input, reminder_checkbox, time_input, echo_cancel_checkbox,
                               focus_minutes_input, short_break_input, long_break_input, hyperfocus_input,
                               eod_checkbox, eod_time_input, meeting_minutes_input, spotify_client_input,
                               spotify_port_input, spotify_playlist_input, spotify_connect_btn, spotify_cancel_btn,
                               spotify_disconnect_btn, summary_prompt_input, email_prompt_input, tasks_prompt_input, reset_prompts_btn,
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
        // Grouped into titled sections as the page is built: begin_section()
        // closes the one in progress and starts the next.
        Elements page;
        std::string section_title;
        auto begin_section = [&](std::string title) {
            if (!children.empty()) page.push_back(section(section_title, vbox(std::move(children))));
            children.clear();
            section_title = std::move(title);
        };
        begin_section("AI");

        children.push_back(hbox({text("Server  ") | color(Color::GrayLight), backend_toggle->Render()}));
        if (llm_backend_index_ == 1) {
            children.push_back(field_row("Server URL", hbox({llama_host_input->Render() | flex, text(" "),
                                                            llama_check_btn->Render()})));
            children.push_back(field_row("API key", llama_key_input->Render()));
            children.push_back(text("  Kept in your desktop keyring, not in the config file.") | dim);
            if (!llamacpp_status_.empty())
                children.push_back(paragraph("  " + llamacpp_status_) |
                                   color(llamacpp_status_.rfind("Connected", 0) == 0 ? theme::kSuccess
                                                                                     : theme::kWarning));
            children.push_back(text("  Uses the model llama-server was started with (-m) and its context size") | dim);
            children.push_back(text("  (-c). Long calls are split to fit that context automatically.") | dim);
        }
        if (llm_backend_index_ == 0) {
            children.push_back(text("Ollama model") | color(Color::GrayLight));
            children.push_back(hbox({
                model_dropdown->Render() | flex,
                text(" "),
                refresh_models_btn->Render(),
                text(" "),
                pull_btn->Render(),
            }));
            if (!ollama_status_copy.empty()) children.push_back(text(ollama_status_copy) | dim);
            if (pulling_now || !pull_status_copy.empty()) children.push_back(text(pull_status_copy) | dim);

            children.push_back(hbox({text("Keep loaded for ") | color(Color::GrayLight) | vcenter,
                                     keep_alive_input->Render() | size(WIDTH, EQUAL, 10), text(" "),
                                     unload_btn->Render()}));
            children.push_back(text("  How long Ollama holds the model in memory after a call (10m, 1h, 0, -1).") | dim);
            children.push_back(text("  Shorter frees VRAM for Whisper, which otherwise transcribes on CPU.") | dim);
            children.push_back(unload_after_use_checkbox->Render());
            children.push_back(text("  On: the model is loaded for the job and dropped straight after, so it never") | dim);
            children.push_back(text("  sits alongside Whisper or another Ollama client. Costs a reload each time.") | dim);
        }

        begin_section("Transcription -- " + cuda_note);
        children.push_back(text("Whisper model size") | color(Color::GrayLight));
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
        begin_section("Speaker identification (far end)");
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

        children.push_back(text(""));
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

        begin_section("Audio");
        children.push_back(field_row("Mic", mic_dropdown->Render()));
        children.push_back(field_row("Speakers", monitor_dropdown->Render()));
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

        begin_section("You");
        children.push_back(field_row("Your name", name_input->Render()));
        children.push_back(field_row("Database file", db_input->Render()));
        children.push_back(hbox({reminder_checkbox->Render() | vcenter, text("   at ") | dim | vcenter,
                                 time_input->Render() | size(WIDTH, EQUAL, 9), filler()}));
        children.push_back(hbox({text("Meeting reminder ") | color(Color::GrayLight) | vcenter,
                                 meeting_minutes_input->Render() | size(WIDTH, EQUAL, 7),
                                 text(" min before each call on your calendar  (0 = off)") | dim | vcenter,
                                 filler()}));

        children.push_back(echo_cancel_checkbox->Render());
        children.push_back(text("  While recording, audio is routed through a CallScribe device so the") | dim);
        children.push_back(text("  mic can be cleaned. Your normal output is restored when it stops.") | dim);

        begin_section("Focus");
        // A label column, a small box, and its unit after it -- each row
        // centred on the three-row box so the text sits level with the value.
        auto label = [](const std::string& t, int width) {
            return text(t) | color(Color::GrayLight) | size(WIDTH, EQUAL, width) | vcenter;
        };
        auto unit = [](const std::string& t) { return text(t) | dim | vcenter; };
        children.push_back(hbox({
            label("Focus round", 16), focus_minutes_input->Render() | size(WIDTH, EQUAL, 7), unit(" min"),
            text("     "),
            label("Short break", 13), short_break_input->Render() | size(WIDTH, EQUAL, 7), unit(" min"),
            text("     "),
            label("Long break", 12), long_break_input->Render() | size(WIDTH, EQUAL, 7), unit(" min"),
            filler(),
        }));
        children.push_back(hbox({text("") | size(WIDTH, EQUAL, 16),
                                 text("The long break comes after every " +
                                      std::to_string(focus::kRoundsBeforeLongBreak) + " focus rounds.") |
                                     dim}));
        children.push_back(hbox({
            label("Check-in after", 16), hyperfocus_input->Render() | size(WIDTH, EQUAL, 7),
            unit(" min of focus or calls with no 5-minute break  (0 = off)"), filler(),
        }));
        children.push_back(hbox({
            label("Wrap-up at", 16), eod_time_input->Render() | size(WIDTH, EQUAL, 9), text("  "),
            eod_checkbox->Render() | vcenter, filler(),
        }));

        begin_section("Spotify (optional)");
        children.push_back(text("  Plays a playlist during focus rounds and pauses it for breaks. Needs Premium.") |
                           dim);
        children.push_back(text("  Create an app at developer.spotify.com/dashboard (Web API), add this exact") | dim);
        children.push_back(text("  Redirect URI to it, and paste its Client ID below:") | dim);
        children.push_back(text("    " + spotify::redirect_uri(parse_int(spotify_port_, 8888))) |
                           color(theme::kWarning));
        children.push_back(field_row("Client ID", spotify_client_input->Render()));
        children.push_back(field_row("Redirect port", hbox({spotify_port_input->Render() | size(WIDTH, EQUAL, 9), filler()})));
        children.push_back(field_row("Focus playlist", spotify_playlist_input->Render()));

        const bool logging_in = ctx_.spotify->logging_in();
        const bool spotify_connected = ctx_.spotify->connected();
        Elements spotify_row;
        if (logging_in) {
            spotify_row.push_back(spotify_cancel_btn->Render());
        } else {
            spotify_row.push_back(spotify_connect_btn->Render());
            if (spotify_connected) {
                spotify_row.push_back(text(" "));
                spotify_row.push_back(spotify_disconnect_btn->Render());
            }
        }
        spotify_row.push_back(text("  "));
        spotify_row.push_back((spotify_connected ? text(" ✓ Connected ") | color(theme::kSuccess)
                                                 : text(" Not connected ") | color(theme::kMuted)) |
                              vcenter);
        children.push_back(hbox(std::move(spotify_row)));
        std::string spotify_status = ctx_.spotify->status();
        if (!spotify_note_.empty()) children.push_back(paragraph("  " + spotify_note_) | color(theme::kWarning));
        if (!spotify_status.empty()) children.push_back(paragraph("  " + spotify_status) | dim);
        std::string manual_url = ctx_.spotify->manual_login_url();
        if (!manual_url.empty()) children.push_back(paragraph(manual_url) | color(theme::kAccent));
        children.push_back(text("  The login token is kept in your desktop keyring (KWallet / GNOME Keyring),") |
                           dim);
        children.push_back(text("  never in a file.") | dim);

        begin_section("AI prompts");
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

        begin_section("");
        return vbox(std::move(page));
    });
    // The page is far taller than a terminal, and without this only moving
    // focus through it from the keyboard ever brought the lower half on screen.
    root_ = ScrollableForm(root_);

    // Save sits in a footer outside the scrolling page, so it's always one
    // click away however far down the change was.
    auto form = root_;
    root_ = Renderer(Container::Vertical({form, save_btn}), [this, form, save_btn] {
        std::string note;
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            note = save_status_;
        }
        return vbox({
            form->Render() | flex,
            hbox({text(" " + note) | dim | vcenter, filler(), save_btn->Render()}),
        });
    });
}

}  // namespace callscribe::ui
