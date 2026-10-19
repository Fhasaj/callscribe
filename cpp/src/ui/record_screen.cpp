#include "ui/record_screen.hpp"

#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <cstdio>

#include "audio/devices.hpp"
#include "config.hpp"
#include "llm/summarizer.hpp"
#include "transcription/diarizer.hpp"
#include "ui/level_meter.hpp"
#include "ui/scroll_pane.hpp"
#include "ui/styled_input.hpp"
#include "ui/task_meta.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"
#include "ui/wrapped_input.hpp"
#include "util/clipboard.hpp"
#include "util/date_format.hpp"
#include "util/time.hpp"
#include "util/wav_writer.hpp"

namespace callscribe::ui {

namespace {

// The DB column and the LLM prompt both want participants as one line.
std::string join_participants(const std::vector<std::string>& names) {
    std::string joined;
    for (const std::string& name : names) {
        if (name.empty()) continue;
        if (!joined.empty()) joined += ", ";
        joined += name;
    }
    return joined;
}

std::string format_mmss(long long total_seconds) {
    long long m = total_seconds / 60;
    long long s = total_seconds % 60;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02lld:%02lld", m, s);
    return buf;
}
}  // namespace

RecordScreen::RecordScreen(AppContext& ctx)
    : ctx_(ctx), proposed_tasks_(ctx, [this] { return call_id_; }) {
    build_ui();
}

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

    // Participants get a popup rather than an inline field: names are added one
    // at a time and each can be removed on its own, which a single
    // comma-separated box made needlessly fiddly.
    ButtonOption participants_opt = button_style(theme::kAccent);
    participants_opt.transform = [this](const EntryState& s) {
        std::string label = participant_list_.empty()
                                ? " + Add participants "
                                : " " + std::to_string(participant_list_.size()) + " participant" +
                                      (participant_list_.size() == 1 ? "" : "s") + " -- edit ";
        Element e = text(label) | bold | color(theme::kAccent);
        if (s.focused) e |= inverted;
        return e;
    };
    auto participants_button = Button(
        "Participants",
        [this] {
            if (state_.load() == State::Idle) participants_popup_ = true;
        },
        participants_opt);

    auto start_stop_button = Button(
        "Start / Stop Recording",
        [this] {
            if (state_.load() == State::Idle)
                start_recording();
            else if (is_recording())
                stop_recording();
        },
        [this] {
            // Says what it will do next, in the colour of that: green to
            // start, red to stop.
            ButtonOption opt;
            opt.transform = [this](const EntryState& s) {
                EntryState labelled = s;
                const bool recording = is_recording();
                labelled.label = recording ? "■ Stop recording" : "● Start recording";
                return button_style(recording ? theme::kDanger : theme::kSuccess).transform(labelled);
            };
            return opt;
        }());

    // Label follows the state so the button says what it will do next.
    ButtonOption pause_opt;
    pause_opt.transform = [this](const EntryState& s) {
        EntryState labelled = s;
        const bool paused = state_.load() == State::Paused;
        labelled.label = paused ? "▶ Resume" : "❚❚ Pause";
        return button_style(paused ? theme::kSuccess : theme::kWarning).transform(labelled);
    };
    auto pause_button = Button("Pause", [this] { toggle_pause(); }, pause_opt);
    auto pause_slot = Maybe(pause_button, [this] { return is_recording(); });

    auto copy_summary_btn = Button("Copy Summary", [this] {
        std::lock_guard<std::mutex> lock(text_mutex_);
        util::copy_to_clipboard(summary_text_);
    }, button_style(theme::kMuted));
    auto copy_email_btn = Button("Copy Email", [this] {
        std::lock_guard<std::mutex> lock(text_mutex_);
        util::copy_to_clipboard(email_draft_);
    }, button_style(theme::kMuted));

    auto copy_transcript_btn = Button("Copy Transcript", [this] {
        std::lock_guard<std::mutex> lock(text_mutex_);
        util::copy_to_clipboard(live_transcript_);
    }, button_style(theme::kMuted));
    auto transcript_pane = ScrollableText(
        [this] {
            std::lock_guard<std::mutex> lock(text_mutex_);
            return live_transcript_;
        },
        "(nothing yet)");
    auto live_page = Renderer(Container::Vertical({transcript_pane, copy_transcript_btn}),
                              [transcript_pane, copy_transcript_btn] {
                                  return vbox({transcript_pane->Render() | flex, hbox({filler(), copy_transcript_btn->Render()})}) | flex;
                              });

    InputOption notes_opt;
    notes_opt.multiline = true;
    notes_opt.on_change = [this] {
        if (call_id_ >= 0) ctx_.repo->update_manual_notes(call_id_, manual_notes_);
    };
    auto notes_input = WrappedInput(&manual_notes_, "Type your own notes for this call here...", notes_opt);
    // An editable Input consumes clicks to place its cursor, so its text can't
    // be drag-selected the way the read-only panes can -- hence a copy button.
    auto copy_notes_btn = Button("Copy Notes", [this] { util::copy_to_clipboard(manual_notes_); }, button_style(theme::kMuted));
    auto notes_page = Renderer(Container::Vertical({notes_input, copy_notes_btn}),
                               [notes_input, copy_notes_btn] {
                                   return vbox({
                                              notes_input->Render() | flex | borderStyled(ROUNDED, theme::kMuted),
                                              hbox({filler(), copy_notes_btn->Render()}),
                                          }) |
                                          flex;
                               });

    InputOption new_task_opt = styled_input_option();
    new_task_opt.multiline = false;
    new_task_opt.on_enter = [this] { add_task_for_call(); };
    auto new_task_input = WrappedInput(&new_task_text_, "Add a task for this call...", new_task_opt);
    auto add_task_btn = Button("Add Task", [this] { add_task_for_call(); }, button_style(theme::kAccent));

    auto summary_pane = ScrollableText(
        [this] {
            std::lock_guard<std::mutex> lock(text_mutex_);
            return summary_text_;
        },
        "(no summary yet)");

    auto proposed_panel = proposed_tasks_.component();
    auto summary_page = Renderer(
        Container::Vertical({summary_pane, copy_summary_btn, proposed_panel, new_task_input, add_task_btn}),
        [this, summary_pane, copy_summary_btn, proposed_panel, new_task_input, add_task_btn] {
            if (linked_tasks_dirty_.load() || ctx_.repo->revision() != seen_revision_) refresh_linked_tasks();

            Elements task_lines;
            for (auto& t : linked_tasks_) {
                bool done = t.status == db::kStatusDone;
                std::string line = (done ? "[x] " : "[ ] ") + t.description;
                if (t.source == "call") line += "  [from summary]";
                Element e = text(line);
                if (done) e |= color(theme::kMuted);
                task_lines.push_back(e);
                task_lines.push_back(text("    " + task_meta_line(t)) | color(theme::kMuted));
            }
            if (task_lines.empty()) task_lines.push_back(text("(no tasks linked to this call yet)") | dim);

            Elements children = {summary_pane->Render() | flex, hbox({filler(), copy_summary_btn->Render()})};
            // The review panel renders itself only when there is something to
            // review, so the section is asked for its own emptiness first.
            if (!proposed_tasks_.empty()) {
                children.push_back(separator() | color(theme::kMuted));
                children.push_back(proposed_panel->Render());
            }
            children.push_back(separator() | color(theme::kMuted));
            children.push_back(text("Tasks for this call") | bold | color(theme::kAccent));
            children.push_back(vbox(std::move(task_lines)));
            children.push_back(hbox({new_task_input->Render() | flex, text(" "), add_task_btn->Render()}));
            return vbox(std::move(children)) | flex;
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
                   hbox({filler(), copy_email_btn->Render()}),
               }) |
               flex;
    });

    auto inner_menu = Menu(&inner_tab_titles_, &inner_tab_index_, MenuOption::HorizontalAnimated());
    auto inner_tab_container = Container::Tab({live_page, notes_page, summary_page, email_page}, &inner_tab_index_);

    auto form = Container::Vertical({
        meeting_input,
        customer_input,
        participants_button,
        Container::Horizontal({start_stop_button, pause_slot}),
        inner_menu,
        inner_tab_container,
    });

    auto page = Renderer(form, [this, meeting_input, customer_input, participants_button, start_stop_button,
                                  pause_slot, inner_menu, inner_tab_container] {
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
        Element details = vbox({
            hbox({
                field_row("Meeting", meeting_input->Render(), 10) | flex,
                text("  "),
                field_row("Customer", customer_input->Render(), 10) | flex,
            }),
            hbox({
                text("Participants") | color(Color::GrayLight) | size(WIDTH, EQUAL, 14),
                participants_button->Render(),
                text("  "),
                participant_list_.empty() ? text("none yet") | dim : paragraph(participants_) | color(theme::kMuted),
            }),
        });
        children.push_back(section("Call", details));

        Elements status_row = {start_stop_button->Render()};
        if (is_recording()) {
            status_row.push_back(text(" "));
            status_row.push_back(pause_slot->Render());
        }
        status_row.push_back(text("   "));
        if (state == State::Recording) status_row.push_back(badge("REC", theme::kDanger) | vcenter);
        if (state == State::Paused) status_row.push_back(badge("PAUSED", theme::kWarning) | vcenter);
        status_row.push_back(text(" " + status_text) | color(status_color) | vcenter);
        status_row.push_back(filler());
        status_row.push_back(text(elapsed_text) | color(status_color) | (state == State::Recording ? bold : nothing) |
                             vcenter);
        status_row.push_back(text(" "));
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
        // Only reachable when Whisper is running well behind realtime -- worth
        // saying plainly, since the transcript is what the summary is built from.
        if (size_t dropped = dropped_chunks(); dropped > 0) {
            children.push_back(paragraph("Transcription is behind: " + std::to_string(dropped) +
                                          " chunk(s) skipped in the live transcript. The full recording is "
                                          "still saved. A smaller Whisper model, or freeing GPU memory in "
                                          "Settings, will keep up.") |
                                color(theme::kWarning));
        }
        if (!error_copy.empty()) {
            children.push_back(text(error_copy) | color(theme::kDanger));
        }
        children.push_back(text(""));
        children.push_back(inner_menu->Render());
        children.push_back(inner_tab_container->Render() | flex);

        return vbox(children) | flex;
    });

    // Modal rather than an inline expander: the participant list is a short,
    // focused edit, and a popup keeps the recording controls from shifting
    // down the screen every time a name is added.
    root_ = Modal(page, build_participants_popup(), &participants_popup_);
}

ftxui::Component RecordScreen::build_participants_popup() {
    using namespace ftxui;

    participant_rows_ = Container::Vertical({});
    rebuild_participant_rows();

    InputOption add_opt = styled_input_option();
    add_opt.multiline = false;
    add_opt.on_enter = [this] { add_participant(); };
    auto add_input = WrappedInput(&new_participant_, "Name", add_opt);
    auto add_btn = Button(" Add ", [this] { add_participant(); }, button_style(theme::kAccent));
    auto done_btn = Button(
        " Save ", [this] { participants_popup_ = false; }, button_style(theme::kSuccess));

    auto layout = Container::Vertical({
        Container::Horizontal({add_input, add_btn}),
        participant_rows_,
        done_btn,
    });

    return Renderer(layout, [this, add_input, add_btn, done_btn] {
        Elements children = {
            text("Participants") | bold | color(theme::kAccent),
            text("Everyone on the call besides you. Used to label the far end of the") | dim,
            text("transcript, and given to the model when it writes the summary.") | dim,
            separator() | color(theme::kMuted),
            hbox({add_input->Render() | flex, text(" "), add_btn->Render()}),
        };
        if (participant_list_.empty())
            children.push_back(text("No participants added yet.") | dim);
        else
            children.push_back(participant_rows_->Render());
        children.push_back(separator() | color(theme::kMuted));
        children.push_back(hbox({filler(), done_btn->Render()}));
        return vbox(std::move(children)) | size(WIDTH, GREATER_THAN, 48) |
               borderStyled(ROUNDED, theme::kAccent) | clear_under;
    });
}

void RecordScreen::rebuild_participant_rows() {
    using namespace ftxui;

    participant_rows_->DetachAllChildren();
    for (size_t i = 0; i < participant_list_.size(); ++i) {
        InputOption name_opt = styled_input_option();
        name_opt.multiline = false;
        // Editing a name in place keeps the joined form in step, so the label
        // shown on the Record tab never lags what is in the popup.
        name_opt.on_change = [this] { sync_participants(); };
        auto name_input = WrappedInput(&participant_list_[i], "Name", name_opt);

        ButtonOption remove_opt;
        remove_opt.transform = [](const EntryState& s) {
            Element e = text(s.label) | color(theme::kDanger);
            if (s.focused) e |= inverted;
            return e;
        };
        auto remove_btn = Button(
            "[x]",
            [this, i] {
                if (i >= participant_list_.size()) return;
                participant_list_.erase(participant_list_.begin() + static_cast<long>(i));
                sync_participants();
                rebuild_participant_rows();
            },
            remove_opt);

        auto row = Container::Horizontal({name_input, remove_btn});
        participant_rows_->Add(Renderer(row, [name_input, remove_btn] {
            return hbox({name_input->Render() | flex, text(" "), remove_btn->Render()});
        }));
    }
}

void RecordScreen::add_participant() {
    size_t begin = new_participant_.find_first_not_of(" \t");
    size_t end = new_participant_.find_last_not_of(" \t");
    if (begin == std::string::npos) return;
    participant_list_.push_back(new_participant_.substr(begin, end - begin + 1));
    new_participant_.clear();
    sync_participants();
    rebuild_participant_rows();
}

void RecordScreen::sync_participants() { participants_ = join_participants(participant_list_); }

void RecordScreen::prefill(const std::string& meeting_name, const std::string& customer_name,
                            const std::vector<std::string>& participants) {
    if (state_.load() != State::Idle) return;
    if (!meeting_name.empty()) meeting_name_ = meeting_name;
    if (!customer_name.empty()) customer_name_ = customer_name;
    if (!participants.empty()) {
        participant_list_ = participants;
        sync_participants();
        rebuild_participant_rows();
    }
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

    sync_participants();
    customer_id_ =
        customer_name_.empty() ? std::nullopt : std::optional<int64_t>(ctx_.repo->get_or_create_customer(customer_name_));
    call_id_ = ctx_.repo->create_call(customer_id_, meeting_name_, participants_);
    linked_tasks_dirty_ = true;

    // Copied for the monitor worker before it starts (see far_end_names_).
    far_end_names_ = participant_list_;
    detect_speaker_turns_ = ctx_.settings.detect_speaker_turns &&
                             transcription::model_supports_speaker_turns(ctx_.settings.whisper_model_size);

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
        int64_t chunk_offset_ms = chunk->start_sample * 1000 / audio::kSampleRate;
        for (const auto& utterance : engine->transcribe_turns(chunk->samples, /*detect_turns=*/false)) {
            if (utterance.text.empty()) continue;
            ctx_.repo->append_segment(call_id_, ctx_.settings.user_display_name, utterance.text,
                                       chunk_offset_ms + utterance.start_ms, chunk_offset_ms + utterance.end_ms);
            append_transcript_line(ctx_.settings.user_display_name, utterance.text);
        }
    }
}

std::string RecordScreen::far_end_label(size_t turn_index) const {
    if (!detect_speaker_turns_) {
        // No turn detection: everything from the far end is one speaker as far
        // as we can tell, so it is labelled with whoever is on the call.
        return far_end_names_.empty() ? kDefaultOtherSpeakerLabel : join_participants(far_end_names_);
    }
    if (turn_index < far_end_names_.size()) return far_end_names_[turn_index];
    // More voices than names -- tinydiarize can tell us the speaker changed but
    // never who it is, so past the end of the list we number rather than guess.
    return "Speaker " + std::to_string(turn_index + 1);
}

void RecordScreen::monitor_worker_loop() {
    auto* engine = ctx_.monitor_engine();
    if (!engine) return;
    // Turn numbering runs across the whole call, not per chunk, so the second
    // person to speak keeps the same label every time they come back.
    size_t turn = 0;
    while (auto chunk = monitor_capture_->chunks().pop()) {
        int64_t chunk_offset_ms = chunk->start_sample * 1000 / audio::kSampleRate;
        for (const auto& utterance : engine->transcribe_turns(chunk->samples, detect_speaker_turns_)) {
            if (utterance.text.empty()) continue;
            std::string label = far_end_label(turn);
            ctx_.repo->append_segment(call_id_, label, utterance.text, chunk_offset_ms + utterance.start_ms,
                                       chunk_offset_ms + utterance.end_ms);
            append_transcript_line(label, utterance.text);
            if (utterance.turn_after) ++turn;
        }
    }
}

void RecordScreen::refresh_linked_tasks() {
    seen_revision_ = ctx_.repo->revision();
    linked_tasks_.clear();
    if (call_id_ >= 0) {
        // Proposed ones are the review panel's business, not this list's.
        for (auto& task : ctx_.repo->list_tasks_for_call(call_id_))
            if (task.status != db::kStatusProposed) linked_tasks_.push_back(std::move(task));
    }
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

void RecordScreen::reload_transcript_from_db() {
    std::string rebuilt;
    for (const auto& segment : ctx_.repo->get_segments(call_id_)) {
        if (!rebuilt.empty()) rebuilt += "\n";
        rebuilt += segment.speaker + ": " + segment.text;
    }
    std::lock_guard<std::mutex> lock(text_mutex_);
    live_transcript_ = std::move(rebuilt);
}

void RecordScreen::diarize_far_end(std::vector<int16_t> monitor_audio) {
    if (monitor_audio.empty()) return;
    {
        std::lock_guard<std::mutex> lock(text_mutex_);
        status_detail_ = "Identifying speakers...";
    }

    try {
        // Only the far-end lines are candidates for relabelling -- your own mic
        // is already a separate stream and needs no guessing.
        std::vector<db::TranscriptSegment> far_end;
        for (auto& segment : ctx_.repo->get_segments(call_id_)) {
            if (segment.speaker == ctx_.settings.user_display_name) continue;
            if (segment.end_ms <= segment.start_ms) continue;  // pre-timestamp row
            far_end.push_back(std::move(segment));
        }
        if (far_end.empty()) return;

        // Telling the clusterer how many voices to look for is the single
        // biggest quality lever, and the participant list is exactly that. One
        // name is not a useful hint (there may still be someone unlisted), so
        // below two it is left to work the number out for itself.
        int expected = static_cast<int>(far_end_names_.size());
        transcription::Diarizer diarizer((models_dir() / transcription::segmentation_model_filename()).string(),
                                          (models_dir() / transcription::embedding_model_filename()).string(),
                                          expected);

        // These models are 16 kHz, the same rate capture runs at -- but the
        // times below are only meaningful if that holds, so it is checked
        // rather than assumed.
        if (diarizer.sample_rate() != audio::kSampleRate) {
            std::lock_guard<std::mutex> lock(text_mutex_);
            error_message_ = "Speaker identification skipped: the models expect " +
                              std::to_string(diarizer.sample_rate()) + " Hz audio.";
            return;
        }

        std::vector<float> samples(monitor_audio.size());
        for (size_t i = 0; i < monitor_audio.size(); ++i) samples[i] = monitor_audio[i] / 32768.0f;
        std::vector<int16_t>().swap(monitor_audio);

        std::vector<transcription::DiarizedSegment> speech = diarizer.process(samples);
        std::vector<float>().swap(samples);
        if (speech.empty()) return;

        std::vector<std::pair<int64_t, int64_t>> ranges;
        ranges.reserve(far_end.size());
        for (const db::TranscriptSegment& line : far_end) ranges.emplace_back(line.start_ms, line.end_ms);

        std::vector<std::string> names = transcription::label_lines(speech, ranges, far_end_names_);
        for (size_t i = 0; i < far_end.size(); ++i) {
            // An empty name means no diarized speech lined up with that line --
            // keep whatever it was labelled with live rather than guessing.
            if (names[i].empty() || names[i] == far_end[i].speaker) continue;
            ctx_.repo->set_segment_speaker(far_end[i].id, names[i]);
        }

        reload_transcript_from_db();
    } catch (const std::exception& e) {
        // A diarization failure must not cost the call its summary: the
        // transcript still has its live labels and is perfectly usable.
        std::lock_guard<std::mutex> lock(text_mutex_);
        error_message_ = std::string("Speaker identification failed: ") + e.what();
    }
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

    // Mix in place. Each capture buffer is ~115 MB per recorded hour, so
    // allocating a third full-length buffer for the mix used to triple peak
    // memory at exactly the point summarization also wants room. Instead the
    // mic buffer is reused as the mix and each source is released the moment
    // it has been folded in. The sum of two int16 samples can overflow, so it
    // is taken in int32 and clamped back.
    std::vector<int16_t> mixed = std::move(mic_audio);
    if (monitor_audio.size() > mixed.size()) mixed.resize(monitor_audio.size(), 0);
    for (size_t i = 0; i < monitor_audio.size(); ++i) {
        int32_t sum = static_cast<int32_t>(mixed[i]) + static_cast<int32_t>(monitor_audio[i]);
        mixed[i] = static_cast<int16_t>(std::max(-32768, std::min(32767, sum)));
    }

    std::string wav_path = (recordings_dir() / ("call_" + std::to_string(call_id_) + ".wav")).string();
    try {
        util::write_wav_mono16(wav_path, mixed, audio::kSampleRate);
    } catch (const std::exception&) {
        wav_path.clear();
    }
    // The mix is on disk now. Freed before diarization rather than after: the
    // far-end buffer has to be widened to float for the ONNX models, and doing
    // that while a third full-length buffer is still alive is the peak this
    // ordering exists to avoid. monitor_audio is deliberately still held.
    std::vector<int16_t>().swap(mixed);

    // Transcription is over for this call. Hand back the per-stream Whisper
    // buffers now rather than at quit: summarizing is the memory-hungriest
    // part of the pipeline and wants the room, and the app may then sit idle
    // for hours before the next recording.
    ctx_.release_whisper_states();

    if (ctx_.settings.diarize_far_end) diarize_far_end(std::move(monitor_audio));
    std::vector<int16_t>().swap(monitor_audio);

    ctx_.repo->finish_call(call_id_, static_cast<int>(duration), wav_path, "summarizing");

    std::string transcript = ctx_.repo->render_transcript(call_id_);
    std::string model = ctx_.settings.llm_model();
    if (model.empty()) {
        std::lock_guard<std::mutex> lock(text_mutex_);
        error_message_ = "No AI model selected -- pick one in Settings.";
        ctx_.repo->set_call_status(call_id_, "error");
        state_.store(State::Idle);
        return;
    }

    try {
        auto result = llm::summarize_call(ctx_.settings.llm_host(), model, transcript, customer_name_,
                                           meeting_name_, participants_, util::iso_date_to_display(util::today_iso()),
                                           [this](const std::string& phase) {
                                               std::lock_guard<std::mutex> lock(text_mutex_);
                                               status_detail_ = phase;
                                           },
                                           llm::PromptPresets{ctx_.settings.summary_prompt,
                                                              ctx_.settings.email_prompt,
                                                              ctx_.settings.tasks_prompt},
                                           ctx_.settings.user_display_name);
        for (auto& t : result.tasks) {
            // The model can attribute a task to a customer other than the
            // call's own (a task raised about a different account); that name
            // becomes a real customer row so the task still files correctly.
            std::optional<int64_t> task_customer = customer_id_;
            if (!t.customer.empty() && t.customer != customer_name_)
                task_customer = ctx_.repo->get_or_create_customer(t.customer);
            // Proposed, not open: these are the model's reading of the call and
            // go under the summary for review. Nothing reaches the Tasks tab
            // or the due-date reminders until it is confirmed there.
            ctx_.repo->add_task(t.description, call_id_, task_customer, t.due_date, "call", t.priority,
                                 db::kStatusProposed);
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
