#include "ui/record_screen.hpp"

#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <cstdio>

#include "audio/devices.hpp"
#include "config.hpp"
#include "llm/summarizer.hpp"
#include "util/clipboard.hpp"
#include "util/date_format.hpp"
#include "util/time.hpp"
#include "util/wav_writer.hpp"

namespace callscribe::ui {

namespace {
std::string format_mmss(long long total_seconds) {
    long long m = total_seconds / 60;
    long long s = total_seconds % 60;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02lld:%02lld", m, s);
    return buf;
}
}  // namespace

RecordScreen::RecordScreen(AppContext& ctx) : ctx_(ctx) { build_ui(); }

RecordScreen::~RecordScreen() {
    if (state_.load() == State::Recording) stop_recording();
    if (finalize_worker_.joinable()) finalize_worker_.join();
    if (mic_worker_.joinable()) mic_worker_.join();
    if (monitor_worker_.joinable()) monitor_worker_.join();
}

void RecordScreen::build_ui() {
    using namespace ftxui;

    InputOption single_line;
    single_line.multiline = false;

    auto meeting_input = Input(&meeting_name_, "Meeting name", single_line);
    auto customer_input = Input(&customer_name_, "Customer", single_line);
    auto participants_input = Input(&participants_, "Participants (comma-separated)", single_line);

    auto start_stop_button = Button("Start / Stop Recording", [this] {
        if (state_.load() == State::Idle)
            start_recording();
        else if (state_.load() == State::Recording)
            stop_recording();
    });

    auto copy_summary_btn = Button("Copy Summary", [this] {
        std::lock_guard<std::mutex> lock(text_mutex_);
        util::copy_to_clipboard(summary_text_);
    });
    auto copy_email_btn = Button("Copy Email", [this] {
        std::lock_guard<std::mutex> lock(text_mutex_);
        util::copy_to_clipboard(email_draft_);
    });

    auto live_page = Renderer([this] {
        std::string transcript_copy;
        {
            std::lock_guard<std::mutex> lock(text_mutex_);
            transcript_copy = live_transcript_;
        }
        return frame(paragraph(transcript_copy.empty() ? "(nothing yet)" : transcript_copy)) | flex | border;
    });

    InputOption notes_opt;
    notes_opt.multiline = true;
    notes_opt.on_change = [this] {
        if (call_id_ >= 0) ctx_.repo->update_manual_notes(call_id_, manual_notes_);
    };
    auto notes_input = Input(&manual_notes_, "Type your own notes for this call here...", notes_opt);
    auto notes_page = Renderer(notes_input, [notes_input] { return notes_input->Render() | flex | border; });

    auto summary_page = Renderer(copy_summary_btn, [this, copy_summary_btn] {
        std::string summary_copy;
        {
            std::lock_guard<std::mutex> lock(text_mutex_);
            summary_copy = summary_text_;
        }
        return vbox({
                   paragraph(summary_copy.empty() ? "(no summary yet)" : summary_copy) | flex | border,
                   copy_summary_btn->Render(),
               }) |
               flex;
    });

    auto email_page = Renderer(copy_email_btn, [this, copy_email_btn] {
        std::string email_copy;
        {
            std::lock_guard<std::mutex> lock(text_mutex_);
            email_copy = email_draft_;
        }
        return vbox({
                   paragraph(email_copy.empty() ? "(no email draft yet)" : email_copy) | flex | border,
                   copy_email_btn->Render(),
               }) |
               flex;
    });

    auto inner_menu = Menu(&inner_tab_titles_, &inner_tab_index_, MenuOption::HorizontalAnimated());
    auto inner_tab_container = Container::Tab({live_page, notes_page, summary_page, email_page}, &inner_tab_index_);

    auto form = Container::Vertical({
        meeting_input,
        customer_input,
        participants_input,
        start_stop_button,
        inner_menu,
        inner_tab_container,
    });

    root_ = Renderer(form, [this, meeting_input, customer_input, participants_input, start_stop_button, inner_menu,
                             inner_tab_container] {
        State state = state_.load();
        std::string status_text = state == State::Idle    ? "Idle"
                                   : state == State::Recording ? "Recording..."
                                                                : "Transcribing & summarizing...";
        Color status_color = state == State::Recording ? Color(Color::Red) : Color(Color::Default);

        std::string elapsed_text = "00:00";
        if (state == State::Recording) {
            auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() -
                                                                           start_time_)
                            .count();
            elapsed_text = format_mmss(secs);
        }

        std::string error_copy;
        {
            std::lock_guard<std::mutex> lock(text_mutex_);
            error_copy = error_message_;
        }

        bool editable = state == State::Idle;

        Elements children;
        children.push_back(text("CallScribe -- Record a call") | bold);
        children.push_back(separator());
        children.push_back(hbox({text("Meeting:      "), meeting_input->Render() | flex}));
        children.push_back(hbox({text("Customer:     "), customer_input->Render() | flex}));
        children.push_back(hbox({text("Participants: "), participants_input->Render() | flex}));
        children.push_back(hbox({
            start_stop_button->Render(),
            text("   "),
            text(status_text) | color(status_color),
            filler(),
            text(elapsed_text),
        }));
        if (!editable) {
            children.push_back(text("(fields locked while recording/processing)") | dim);
        }
        if (!error_copy.empty()) {
            children.push_back(text(error_copy) | color(Color::Red));
        }
        children.push_back(separator());
        children.push_back(inner_menu->Render());
        children.push_back(separator());
        children.push_back(inner_tab_container->Render() | flex);

        return vbox(children) | flex;
    });
}

void RecordScreen::start_recording() {
    if (!ctx_.whisper_ready()) {
        std::lock_guard<std::mutex> lock(text_mutex_);
        error_message_ =
            ctx_.whisper_loading() ? "Whisper model still loading -- try again shortly." : "Whisper model not loaded (check Settings).";
        return;
    }
    {
        std::lock_guard<std::mutex> lock(text_mutex_);
        error_message_.clear();
        live_transcript_.clear();
        summary_text_.clear();
        email_draft_.clear();
    }
    manual_notes_.clear();

    customer_id_ =
        customer_name_.empty() ? std::nullopt : std::optional<int64_t>(ctx_.repo->get_or_create_customer(customer_name_));
    call_id_ = ctx_.repo->create_call(customer_id_, meeting_name_, participants_);

    // parec's own default (when --device is omitted) is the default SOURCE
    // (mic), not a monitor -- so an unset monitor_source must be resolved
    // explicitly here, or the "monitor" stream would silently capture the
    // mic too instead of speaker output.
    std::string mic_source = ctx_.settings.mic_source.empty() ? audio::default_mic_source() : ctx_.settings.mic_source;
    std::string monitor_source =
        ctx_.settings.monitor_source.empty() ? audio::default_monitor_source() : ctx_.settings.monitor_source;
    mic_capture_ = std::make_unique<audio::StreamCapture>(mic_source);
    monitor_capture_ = std::make_unique<audio::StreamCapture>(monitor_source);
    try {
        mic_capture_->start();
        monitor_capture_->start();
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(text_mutex_);
        error_message_ = std::string("Failed to start audio capture: ") + e.what();
        mic_capture_.reset();
        monitor_capture_.reset();
        return;
    }

    if (mic_worker_.joinable()) mic_worker_.join();
    if (monitor_worker_.joinable()) monitor_worker_.join();
    mic_worker_ = std::thread(&RecordScreen::mic_worker_loop, this);
    monitor_worker_ = std::thread(&RecordScreen::monitor_worker_loop, this);

    start_time_ = std::chrono::steady_clock::now();
    state_.store(State::Recording);
}

void RecordScreen::mic_worker_loop() {
    auto* engine = ctx_.mic_engine();
    if (!engine) return;
    while (auto chunk = mic_capture_->chunks().pop()) {
        std::string text = engine->transcribe(*chunk);
        if (!text.empty()) {
            ctx_.repo->append_segment(call_id_, ctx_.settings.user_display_name, text);
            append_transcript_line(ctx_.settings.user_display_name, text);
        }
    }
}

void RecordScreen::monitor_worker_loop() {
    auto* engine = ctx_.monitor_engine();
    if (!engine) return;
    std::string label = participants_.empty() ? kDefaultOtherSpeakerLabel : participants_;
    while (auto chunk = monitor_capture_->chunks().pop()) {
        std::string text = engine->transcribe(*chunk);
        if (!text.empty()) {
            ctx_.repo->append_segment(call_id_, label, text);
            append_transcript_line(label, text);
        }
    }
}

void RecordScreen::append_transcript_line(const std::string& speaker, const std::string& text) {
    std::lock_guard<std::mutex> lock(text_mutex_);
    if (!live_transcript_.empty()) live_transcript_ += "\n";
    live_transcript_ += speaker + ": " + text;
}

void RecordScreen::stop_recording() {
    if (state_.load() != State::Recording) return;
    state_.store(State::Processing);
    if (finalize_worker_.joinable()) finalize_worker_.join();
    finalize_worker_ = std::thread(&RecordScreen::finalize, this);
}

void RecordScreen::finalize() {
    auto mic_audio = mic_capture_->stop();
    auto monitor_audio = monitor_capture_->stop();
    if (mic_worker_.joinable()) mic_worker_.join();
    if (monitor_worker_.joinable()) monitor_worker_.join();

    long long duration =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start_time_).count();

    size_t n = std::max(mic_audio.size(), monitor_audio.size());
    std::vector<float> mixed(n, 0.0f);
    for (size_t i = 0; i < mic_audio.size(); ++i) mixed[i] += mic_audio[i];
    for (size_t i = 0; i < monitor_audio.size(); ++i) mixed[i] += monitor_audio[i];
    for (auto& s : mixed) s = std::max(-1.0f, std::min(1.0f, s));

    std::string wav_path = (recordings_dir() / ("call_" + std::to_string(call_id_) + ".wav")).string();
    try {
        util::write_wav_mono16(wav_path, mixed, audio::kSampleRate);
    } catch (const std::exception&) {
        wav_path.clear();
    }

    ctx_.repo->finish_call(call_id_, static_cast<int>(duration), wav_path, "summarizing");

    std::string transcript = ctx_.repo->render_transcript(call_id_);
    std::string model = ctx_.settings.ollama_model;
    if (model.empty()) {
        std::lock_guard<std::mutex> lock(text_mutex_);
        error_message_ = "No Ollama model selected -- pick one in Settings.";
        ctx_.repo->set_call_status(call_id_, "error");
        state_.store(State::Idle);
        return;
    }

    try {
        auto result = llm::summarize_call(ctx_.settings.ollama_host, model, transcript, customer_name_,
                                           meeting_name_, participants_, util::iso_date_to_display(util::today_iso()));
        for (auto& t : result.tasks) {
            ctx_.repo->add_task(t, call_id_, customer_id_, "", "call");
        }
        ctx_.repo->add_note(call_id_, result.summary_text, result.email_draft, "[]");
        ctx_.repo->set_call_status(call_id_, "done");
        std::lock_guard<std::mutex> lock(text_mutex_);
        summary_text_ = result.summary_text;
        email_draft_ = result.email_draft;
    } catch (const std::exception& e) {
        ctx_.repo->set_call_status(call_id_, "error");
        std::lock_guard<std::mutex> lock(text_mutex_);
        error_message_ = std::string("Summarization failed: ") + e.what();
    }

    state_.store(State::Idle);
}

}  // namespace callscribe::ui
