#include "ui/record_screen.hpp"

#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <cstdio>

#include "audio/devices.hpp"
#include "config.hpp"
#include "llm/summarizer.hpp"
#include "ui/level_meter.hpp"
#include "ui/scroll_pane.hpp"
#include "ui/styled_input.hpp"
#include "ui/task_meta.hpp"
#include "ui/theme.hpp"
#include "ui/wrapped_input.hpp"
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
    if (is_recording()) stop_recording();
    if (finalize_worker_.joinable()) finalize_worker_.join();
    if (mic_worker_.joinable()) mic_worker_.join();
    if (monitor_worker_.joinable()) monitor_worker_.join();
}

void RecordScreen::build_ui() {
    using namespace ftxui;

    InputOption single_line = styled_input_option();
    single_line.multiline = false;

    auto meeting_input = WrappedInput(&meeting_name_, "Meeting name", single_line);
    auto customer_input = WrappedInput(&customer_name_, "Customer", single_line);
    auto participants_input = WrappedInput(&participants_, "Participants (comma-separated)", single_line);

    auto start_stop_button = Button(
        "Start / Stop Recording",
        [this] {
            if (state_.load() == State::Idle)
                start_recording();
            else if (is_recording())
                stop_recording();
        },
        ButtonOption::Animated(theme::kAccent));

    // Label follows the state so the button says what it will do next.
    ButtonOption pause_opt = ButtonOption::Animated(theme::kWarning);
    pause_opt.transform = [this](const EntryState& s) {
        Element e = text(state_.load() == State::Paused ? " ▶ Resume " : " ❚❚ Pause ") | bold;
        e |= state_.load() == State::Paused ? color(theme::kSuccess) : color(theme::kWarning);
        if (s.focused) e |= inverted;
        return e;
    };
    auto pause_button = Button("Pause", [this] { toggle_pause(); }, pause_opt);
    auto pause_slot = Maybe(pause_button, [this] { return is_recording(); });

    auto copy_summary_btn = Button("Copy Summary", [this] {
        std::lock_guard<std::mutex> lock(text_mutex_);
        util::copy_to_clipboard(summary_text_);
    });
    auto copy_email_btn = Button("Copy Email", [this] {
        std::lock_guard<std::mutex> lock(text_mutex_);
        util::copy_to_clipboard(email_draft_);
    });

    auto copy_transcript_btn = Button("Copy Transcript", [this] {
        std::lock_guard<std::mutex> lock(text_mutex_);
        util::copy_to_clipboard(live_transcript_);
    });
    auto transcript_pane = ScrollableText(
        [this] {
            std::lock_guard<std::mutex> lock(text_mutex_);
            return live_transcript_;
        },
        "(nothing yet)");
    auto live_page = Renderer(Container::Vertical({transcript_pane, copy_transcript_btn}),
                              [transcript_pane, copy_transcript_btn] {
                                  return vbox({transcript_pane->Render() | flex, copy_transcript_btn->Render()}) | flex;
                              });

    InputOption notes_opt;
    notes_opt.multiline = true;
    notes_opt.on_change = [this] {
        if (call_id_ >= 0) ctx_.repo->update_manual_notes(call_id_, manual_notes_);
    };
    auto notes_input = WrappedInput(&manual_notes_, "Type your own notes for this call here...", notes_opt);
    // An editable Input consumes clicks to place its cursor, so its text can't
    // be drag-selected the way the read-only panes can -- hence a copy button.
    auto copy_notes_btn = Button("Copy Notes", [this] { util::copy_to_clipboard(manual_notes_); });
    auto notes_page = Renderer(Container::Vertical({notes_input, copy_notes_btn}),
                               [notes_input, copy_notes_btn] {
                                   return vbox({
                                              notes_input->Render() | flex | borderStyled(ROUNDED, theme::kMuted),
                                              copy_notes_btn->Render(),
                                          }) |
                                          flex;
                               });

    InputOption new_task_opt = styled_input_option();
    new_task_opt.multiline = false;
    new_task_opt.on_enter = [this] { add_task_for_call(); };
    auto new_task_input = WrappedInput(&new_task_text_, "Add a task for this call...", new_task_opt);
    auto add_task_btn = Button("Add Task", [this] { add_task_for_call(); }, ButtonOption::Animated(theme::kAccent));

    auto summary_pane = ScrollableText(
        [this] {
            std::lock_guard<std::mutex> lock(text_mutex_);
            return summary_text_;
        },
        "(no summary yet)");

    auto summary_page =
        Renderer(Container::Vertical({summary_pane, copy_summary_btn, new_task_input, add_task_btn}),
                 [this, summary_pane, copy_summary_btn, new_task_input, add_task_btn] {
                     if (linked_tasks_dirty_.load() || ctx_.repo->revision() != seen_revision_)
                         refresh_linked_tasks();

                     Elements task_lines;
                     for (auto& t : linked_tasks_) {
                         bool done = t.status == "done";
                         std::string line = (done ? "[x] " : "[ ] ") + t.description;
                         if (t.source == "call") line += "  [auto]";
                         Element e = text(line);
                         if (done) e |= color(theme::kMuted);
                         task_lines.push_back(e);
                         task_lines.push_back(text("    " + task_meta_line(t)) | color(theme::kMuted));
                     }
                     if (task_lines.empty()) task_lines.push_back(text("(no tasks linked to this call yet)") | dim);

                     return vbox({
                                summary_pane->Render() | flex,
                                copy_summary_btn->Render(),
                                separator() | color(theme::kMuted),
                                text("Tasks for this call") | bold | color(theme::kAccent),
                                vbox(task_lines),
                                hbox({new_task_input->Render() | flex, text(" "), add_task_btn->Render()}),
                            }) |
                            flex;
                 });

    auto email_pane = ScrollableText(
        [this] {
            std::lock_guard<std::mutex> lock(text_mutex_);
            return email_draft_;
        },
        "(no email draft yet)");
    auto email_page = Renderer(Container::Vertical({email_pane, copy_email_btn}), [email_pane, copy_email_btn] {
        return vbox({
                   email_pane->Render() | flex,
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
        Container::Horizontal({start_stop_button, pause_slot}),
        inner_menu,
        inner_tab_container,
    });

    root_ = Renderer(form, [this, meeting_input, customer_input, participants_input, start_stop_button, pause_slot,
                             inner_menu, inner_tab_container] {
        State state = state_.load();
        std::string status_text = state == State::Idle    ? "Idle"
                                   : state == State::Recording ? "Recording..."
                                   : state == State::Paused    ? "Paused -- audio is not being captured"
                                                                : "Transcribing & summarizing...";
        Color status_color = state == State::Recording  ? theme::kDanger
                              : state == State::Paused     ? theme::kWarning
                              : state == State::Processing ? theme::kWarning
                                                            : theme::kSuccess;

        std::string elapsed_text = state == State::Idle ? "00:00" : format_mmss(elapsed_seconds());

        std::string error_copy, detail_copy;
        {
            std::lock_guard<std::mutex> lock(text_mutex_);
            error_copy = error_message_;
            detail_copy = status_detail_;
        }

        bool editable = state == State::Idle;

        Elements children;
        children.push_back(text("CallScribe -- Record a call") | bold | color(theme::kAccent));
        children.push_back(separator() | color(theme::kMuted));
        children.push_back(hbox({text("Meeting:      "), meeting_input->Render() | flex}));
        children.push_back(hbox({text("Customer:     "), customer_input->Render() | flex}));
        children.push_back(hbox({text("Participants: "), participants_input->Render() | flex}));
        Elements status_row = {start_stop_button->Render()};
        if (is_recording()) {
            status_row.push_back(text(" "));
            status_row.push_back(pause_slot->Render());
        }
        status_row.push_back(text("   "));
        if (state == State::Recording) status_row.push_back(text("● REC ") | bold | color(theme::kDanger));
        if (state == State::Paused) status_row.push_back(text("❚❚ PAUSED ") | bold | color(theme::kWarning));
        status_row.push_back(text(status_text) | color(status_color));
        status_row.push_back(filler());
        status_row.push_back(text(elapsed_text) | color(status_color) | (state == State::Recording ? bold : nothing));
        children.push_back(hbox(status_row));
        if (state == State::Processing && !detail_copy.empty())
            children.push_back(text(detail_copy) | color(theme::kWarning));
        // Model-loading problems used to be recorded and never shown.
        if (std::string we = ctx_.whisper_error(); !we.empty())
            children.push_back(paragraph("Whisper: " + we) | color(theme::kDanger));
        if (std::string wn = ctx_.whisper_notice(); !wn.empty())
            children.push_back(paragraph(wn) | color(theme::kWarning));

        if (is_recording()) {
            children.push_back(hbox({text("Mic:  ") | dim,
                                      gauge(level_to_progress(mic_capture_->current_level())) |
                                          color(theme::kDanger) | flex}));
            children.push_back(hbox({text("Them: ") | dim,
                                      gauge(level_to_progress(monitor_capture_->current_level())) |
                                          color(theme::kAccent) | flex}));
        }
        if (!editable) {
            children.push_back(text("(fields locked while recording/processing)") | dim);
        }
        if (!error_copy.empty()) {
            children.push_back(text(error_copy) | color(theme::kDanger));
        }
        children.push_back(separator() | color(theme::kMuted));
        children.push_back(inner_menu->Render());
        children.push_back(separator() | color(theme::kMuted));
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
    linked_tasks_dirty_ = true;

    // parec's own default (when --device is omitted) is the default SOURCE
    // (mic), not a monitor -- so an unset monitor_source must be resolved
    // explicitly here, or the "monitor" stream would silently capture the
    // mic too instead of speaker output.
    std::string mic_source = ctx_.settings.mic_source.empty() ? audio::default_mic_source() : ctx_.settings.mic_source;
    std::string monitor_source =
        ctx_.settings.monitor_source.empty() ? audio::default_monitor_source() : ctx_.settings.monitor_source;

    // Echo cancellation, when enabled, swaps both streams for the virtual pair:
    // the mic comes back with speaker bleed subtracted, and the far end comes
    // from the virtual sink's monitor (which carries only what was played into
    // it). Session lives until stop_recording, which restores the audio setup.
    echo_cancel_.reset();
    if (ctx_.settings.echo_cancel) {
        auto session = std::make_unique<audio::EchoCancelSession>(mic_source);
        if (session->active()) {
            mic_source = session->mic_source();
            monitor_source = session->monitor_source();
            echo_cancel_ = std::move(session);
            std::lock_guard<std::mutex> lock(text_mutex_);
            status_detail_ = "Echo cancellation on -- audio is routed through CallScribe while recording.";
        } else {
            std::lock_guard<std::mutex> lock(text_mutex_);
            error_message_ = session->error();
        }
    }

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
    recorded_seconds_ = 0;
    state_.store(State::Recording);
}

long long RecordScreen::elapsed_seconds() const {
    long long live = 0;
    if (state_.load() == State::Recording) {
        live = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start_time_)
                   .count();
    }
    return recorded_seconds_ + live;
}

void RecordScreen::toggle_pause() {
    State state = state_.load();
    if (state == State::Recording) {
        // Bank the stretch just recorded, then let the capture threads drop
        // whatever arrives until the resume.
        recorded_seconds_ = elapsed_seconds();
        if (mic_capture_) mic_capture_->set_paused(true);
        if (monitor_capture_) monitor_capture_->set_paused(true);
        state_.store(State::Paused);
    } else if (state == State::Paused) {
        start_time_ = std::chrono::steady_clock::now();
        if (mic_capture_) mic_capture_->set_paused(false);
        if (monitor_capture_) monitor_capture_->set_paused(false);
        state_.store(State::Recording);
    }
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

void RecordScreen::refresh_linked_tasks() {
    seen_revision_ = ctx_.repo->revision();
    linked_tasks_ = call_id_ >= 0 ? ctx_.repo->list_tasks_for_call(call_id_) : std::vector<db::Task>{};
    linked_tasks_dirty_ = false;
}

void RecordScreen::add_task_for_call() {
    if (new_task_text_.empty() || call_id_ < 0) return;
    ctx_.repo->add_task(new_task_text_, call_id_, customer_id_, "", "manual");
    new_task_text_.clear();
    linked_tasks_dirty_ = true;
}

void RecordScreen::append_transcript_line(const std::string& speaker, const std::string& text) {
    std::lock_guard<std::mutex> lock(text_mutex_);
    if (!live_transcript_.empty()) live_transcript_ += "\n";
    live_transcript_ += speaker + ": " + text;
}

void RecordScreen::stop_recording() {
    if (!is_recording()) return;
    // Freeze the clock before the state flips, so a call stopped while paused
    // still reports the time that was actually recorded.
    recorded_seconds_ = elapsed_seconds();
    if (mic_capture_) mic_capture_->set_paused(false);
    if (monitor_capture_) monitor_capture_->set_paused(false);
    state_.store(State::Processing);
    if (finalize_worker_.joinable()) finalize_worker_.join();
    finalize_worker_ = std::thread(&RecordScreen::finalize, this);
}

void RecordScreen::finalize() {
    auto mic_audio = mic_capture_->stop();
    auto monitor_audio = monitor_capture_->stop();
    if (mic_worker_.joinable()) mic_worker_.join();
    if (monitor_worker_.joinable()) monitor_worker_.join();

    // Give the audio setup back as soon as capture is done -- transcribing and
    // summarizing take minutes, and the user shouldn't be routed through our
    // virtual sink for all of it.
    echo_cancel_.reset();

    long long duration = recorded_seconds_;  // paused stretches excluded

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
                                           meeting_name_, participants_, util::iso_date_to_display(util::today_iso()),
                                           [this](const std::string& phase) {
                                               std::lock_guard<std::mutex> lock(text_mutex_);
                                               status_detail_ = phase;
                                           },
                                           llm::PromptPresets{ctx_.settings.summary_prompt,
                                                              ctx_.settings.email_prompt,
                                                              ctx_.settings.tasks_prompt});
        for (auto& t : result.tasks) {
            // The model can attribute a task to a customer other than the
            // call's own (a task raised about a different account); that name
            // becomes a real customer row so the task still files correctly.
            std::optional<int64_t> task_customer = customer_id_;
            if (!t.customer.empty() && t.customer != customer_name_)
                task_customer = ctx_.repo->get_or_create_customer(t.customer);
            ctx_.repo->add_task(t.description, call_id_, task_customer, t.due_date, "call", t.priority);
        }
        linked_tasks_dirty_ = true;
        ctx_.repo->add_note(call_id_, result.summary_text, result.email_draft, "[]");
        ctx_.repo->set_call_status(call_id_, "done");
        std::lock_guard<std::mutex> lock(text_mutex_);
        summary_text_ = result.summary_text;
        email_draft_ = result.email_draft;
        status_detail_.clear();
        if (result.summary_text.empty())
            error_message_ = "The model returned no summary points for this call (the transcript may be too sparse).";
    } catch (const std::exception& e) {
        ctx_.repo->set_call_status(call_id_, "error");
        std::lock_guard<std::mutex> lock(text_mutex_);
        error_message_ = std::string("Summarization failed: ") + e.what();
    }

    state_.store(State::Idle);
}

}  // namespace callscribe::ui
