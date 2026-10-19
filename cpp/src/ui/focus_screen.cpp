#include "ui/focus_screen.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include <array>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <map>

#include "tasks/reminder.hpp"
#include "ui/scroll_pane.hpp"
#include "ui/styled_input.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"
#include "ui/wrapped_input.hpp"
#include "util/clipboard.hpp"
#include "util/date_format.hpp"
#include "util/time.hpp"

namespace callscribe::ui {

namespace {

using Clock = focus::PomodoroTimer::Clock;

// The "just 5 minutes" round, for when starting at all is the hard part.
constexpr int kWarmupMinutes = 5;

std::tm parse_iso_day(const std::string& iso) {
    std::tm tm{};
    tm.tm_year = std::stoi(iso.substr(0, 4)) - 1900;
    tm.tm_mon = std::stoi(iso.substr(5, 2)) - 1;
    tm.tm_mday = std::stoi(iso.substr(8, 2));
    tm.tm_hour = 12;  // midday, so a DST shift can't roll the date
    tm.tm_isdst = -1;
    std::mktime(&tm);
    return tm;
}

std::string offset_days_from(const std::string& iso, int days) {
    std::tm tm = parse_iso_day(iso);
    tm.tm_mday += days;
    std::mktime(&tm);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

// Monday of the current week.
std::string week_start_iso() {
    const std::string today = util::today_iso();
    return offset_days_from(today, -((parse_iso_day(today).tm_wday + 6) % 7));
}

// "2h 30m", "45m".
std::string format_hm(int seconds) {
    const int minutes = (seconds + 30) / 60;
    if (minutes < 60) return std::to_string(minutes) + "m";
    return std::to_string(minutes / 60) + "h" + (minutes % 60 ? " " + std::to_string(minutes % 60) + "m" : "");
}

ftxui::Color phase_color(focus::Phase phase) {
    switch (phase) {
        case focus::Phase::Focus: return theme::kDanger;
        case focus::Phase::ShortBreak: return theme::kSuccess;
        case focus::Phase::LongBreak: return theme::kAccent;
    }
    return theme::kMuted;
}

// Rounded up, so a fresh 25-minute round reads 25:00 and 00:00 only shows
// once it is actually over.
std::string format_mm_ss(std::chrono::milliseconds ms) {
    long long total = (ms.count() + 999) / 1000;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02lld:%02lld", total / 60, total % 60);
    return buf;
}

std::string format_minutes(int seconds) {
    return seconds < 60 ? "<1 min" : std::to_string((seconds + 30) / 60) + " min";
}

std::string trim(const std::string& s) {
    size_t begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

// Five rows of block glyphs, so the countdown reads from across the room.
ftxui::Element big_clock(const std::string& mm_ss) {
    using Glyph = std::array<const char*, 5>;
    static const Glyph kDigits[10] = {
        Glyph{"████", "█  █", "█  █", "█  █", "████"}, Glyph{" ██ ", "  █ ", "  █ ", "  █ ", " ███"},
        Glyph{"████", "   █", "████", "█   ", "████"}, Glyph{"████", "   █", " ███", "   █", "████"},
        Glyph{"█  █", "█  █", "████", "   █", "   █"}, Glyph{"████", "█   ", "████", "   █", "████"},
        Glyph{"████", "█   ", "████", "█  █", "████"}, Glyph{"████", "   █", "   █", "   █", "   █"},
        Glyph{"████", "█  █", "████", "█  █", "████"}, Glyph{"████", "█  █", "████", "   █", "████"},
    };
    static const Glyph kColon = {" ", "█", " ", "█", " "};

    ftxui::Elements rows;
    for (int row = 0; row < 5; ++row) {
        std::string line;
        for (char c : mm_ss) {
            if (!line.empty()) line += ' ';
            line += (c == ':') ? kColon[row] : kDigits[c - '0'][row];
        }
        rows.push_back(ftxui::text(line));
    }
    return ftxui::vbox(std::move(rows));
}

// Fire-and-forget: notify-send is waited on, and that shouldn't hold up the UI.
void notify(std::string title, std::string body) {
    std::thread([title = std::move(title), body = std::move(body)] { tasks::send_notification(title, body); })
        .detach();
}

void ring_bell() { std::cout << '\a' << std::flush; }

// m:ss, for track positions.
std::string format_track_time(int ms) {
    int total = std::max(0, ms) / 1000;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d:%02d", total / 60, total % 60);
    return buf;
}

}  // namespace

FocusScreen::FocusScreen(AppContext& ctx, ftxui::ScreenInteractive& screen, std::function<bool()> is_busy)
    : ctx_(ctx), screen_(screen), is_busy_(std::move(is_busy)) {
    nudge_minutes_.store(ctx_.settings.hyperfocus_nudge_minutes);
    timer_.set_durations({ctx_.settings.focus_minutes, ctx_.settings.short_break_minutes,
                          ctx_.settings.long_break_minutes});
    reload_recent();
    build_ui();
    ticker_ = std::thread([this] { tick_loop(); });
}

FocusScreen::~FocusScreen() {
    focus::Phase phase;
    std::chrono::milliseconds elapsed{0};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        phase = timer_.phase();
        elapsed = timer_.elapsed(Clock::now());
    }
    cv_.notify_all();
    if (ticker_.joinable()) ticker_.join();

    // Quitting mid-round still keeps the round, with whatever note was typed.
    if (session_ && phase == focus::Phase::Focus && elapsed.count() > 0) end_session(elapsed);
    if (pending_note_) save_pending(progress_input_);
}

void FocusScreen::tick_loop() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
        cv_.wait_for(lock, std::chrono::seconds(1), [this] { return stopping_; });
        if (stopping_) break;
        const bool running = timer_.running();
        const bool focusing = running && timer_.phase() == focus::Phase::Focus;
        std::optional<focus::Transition> transition;
        if (running) transition = timer_.advance(Clock::now());
        lock.unlock();

        const bool nudge = track_engagement(focusing || (is_busy_ && is_busy_()));
        // Handled on the UI thread: they open prompts and touch state that
        // only the UI thread owns.
        if (transition) screen_.Post([this, t = *transition] { on_transition(t); });
        if (nudge) screen_.Post([this, minutes = engaged_seconds_ / 60] { on_hyperfocus(minutes); });
        if (running || nudge) screen_.PostEvent(ftxui::Event::Custom);
        lock.lock();
    }
}

bool FocusScreen::track_engagement(bool engaged) {
    // Five minutes away -- a real break -- starts the count again.
    constexpr int kResetAfterIdle = 5 * 60;
    // Once you've been told, the next nudge comes this much later, not every
    // second from then on.
    constexpr int kRepeatEvery = 45 * 60;

    if (!engaged) {
        if (++idle_seconds_ >= kResetAfterIdle) {
            engaged_seconds_ = 0;
            nudges_ = 0;
        }
        return false;
    }
    idle_seconds_ = 0;
    ++engaged_seconds_;
    const int threshold_minutes = nudge_minutes_.load();
    if (threshold_minutes <= 0) return false;
    if (engaged_seconds_ < threshold_minutes * 60 + nudges_ * kRepeatEvery) return false;
    ++nudges_;
    return true;
}

void FocusScreen::on_hyperfocus(int minutes) {
    nudged_at_ = std::chrono::steady_clock::now();
    nudged_after_minutes_ = minutes;
    const std::string span = minutes >= 60 ? std::to_string(minutes / 60) + "h" +
                                                 (minutes % 60 ? " " + std::to_string(minutes % 60) + "m" : "")
                                           : std::to_string(minutes) + " min";
    notify("You've been going " + span + " without a break",
           "Stand up, drink some water, check the time. Five minutes away is enough.");
    ring_bell();
}

FocusScreen::Badge FocusScreen::title_note() const {
    const auto now = std::chrono::steady_clock::now();
    if (nudged_after_minutes_ > 0 && now - nudged_at_ < std::chrono::minutes(3)) {
        return {"Going " + std::to_string(nudged_after_minutes_) + " min straight -- stand up, water, check the time",
                theme::kWarning};
    }
    if (session_) {
        std::string line = "Now: " + session_->tag;
        if (!session_->next_step.empty()) line += " · " + session_->next_step;
        return {line, theme::kAccent};
    }
    return {"", theme::kMuted};
}

FocusScreen::Badge FocusScreen::tab_badge() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = Clock::now();
    if (!timer_.running() && timer_.elapsed(now).count() == 0) return {"", theme::kMuted};
    return {" " + format_mm_ss(timer_.remaining(now)), phase_color(timer_.phase())};
}

bool FocusScreen::handle_shortcut(const ftxui::Event& event) {
    if (prompt_open()) return false;
    if (event == ftxui::Event::Character('s')) {
        toggle();
        return true;
    }
    if (event == ftxui::Event::Character('k')) {
        skip();
        return true;
    }
    if (event == ftxui::Event::Character('r')) {
        reset();
        return true;
    }
    // Music keys only mean something once Spotify is connected; until then
    // they fall through like any other letter.
    if (!ctx_.spotify->connected()) return false;
    if (event == ftxui::Event::Character('p')) {
        ctx_.spotify->toggle_playback(ctx_.settings.spotify_playlist_uri);
        return true;
    }
    if (event == ftxui::Event::Character('n')) {
        ctx_.spotify->next_track();
        return true;
    }
    if (event == ftxui::Event::Character('b')) {
        ctx_.spotify->previous_track();
        return true;
    }
    return false;
}

void FocusScreen::toggle() {
    if (prompt_open()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = Clock::now();
    if (timer_.running()) {
        timer_.pause(now);
        return;
    }
    // A fresh focus round asks what it's for first; a paused one just resumes.
    if (timer_.phase() == focus::Phase::Focus && !session_) {
        start_prompt_open_ = true;
        start_prompt_focus_->TakeFocus();
        return;
    }
    timer_.start(now);
}

void FocusScreen::begin_focus(bool warmup) {
    session_ = Session{tags_[tag_selected_], trim(next_step_input_), util::now_iso()};
    next_step_input_.clear();
    start_prompt_open_ = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (warmup)
            timer_.start_warmup(Clock::now(), kWarmupMinutes);
        else
            timer_.start(Clock::now());
    }
    ctx_.spotify->play_playlist(ctx_.settings.spotify_playlist_uri);
}

void FocusScreen::keep_going() {
    warmup_prompt_open_ = false;
    std::lock_guard<std::mutex> lock(mutex_);
    timer_.start(Clock::now());
}

void FocusScreen::stop_after_warmup() {
    warmup_prompt_open_ = false;
    std::chrono::milliseconds elapsed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        elapsed = timer_.elapsed(Clock::now());
        timer_.discard_round();
    }
    // Five minutes on something you couldn't start is still worth a note.
    end_session(elapsed);
    ctx_.spotify->pause();
}

void FocusScreen::skip() {
    if (prompt_open()) return;
    focus::Transition t;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        t = timer_.skip(Clock::now());
    }
    on_transition(t);
}

void FocusScreen::reset() {
    if (prompt_open()) return;
    focus::Phase phase;
    std::chrono::milliseconds elapsed{0};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        phase = timer_.phase();
        elapsed = timer_.elapsed(Clock::now());
        timer_.reset();
    }
    // Abandoning a round part-way still counts the time spent on it.
    if (phase == focus::Phase::Focus && session_) {
        end_session(elapsed);
        ctx_.spotify->pause();
    }
}

void FocusScreen::on_transition(const focus::Transition& t) {
    if (t.warmup_over) {
        warmup_prompt_open_ = true;
        if (root_ && root_->Active()) warmup_prompt_focus_->TakeFocus();
        notify(std::to_string(kWarmupMinutes) + " minutes done",
               "That was the hard part. Keep going, or stop here -- either is fine.");
        ring_bell();
        return;
    }
    if (t.from == focus::Phase::Focus) {
        end_session(t.elapsed);
        ctx_.spotify->pause();
    }

    if (t.to == focus::Phase::ShortBreak) {
        notify("Focus round done", "Take a " + std::to_string(ctx_.settings.short_break_minutes) + "-minute break.");
    } else if (t.to == focus::Phase::LongBreak) {
        notify(std::to_string(focus::kRoundsBeforeLongBreak) + " focus rounds done",
               "Take a " + std::to_string(ctx_.settings.long_break_minutes) + "-minute long break.");
    } else {
        notify("Break's over", "Press s in CallScribe's Focus tab to start the next round.");
    }
    ring_bell();
}

void FocusScreen::end_session(std::chrono::milliseconds focused) {
    if (!session_) return;
    // Shouldn't happen -- a round can't start while the last one's prompt is
    // up -- but never drop a note that's already been typed.
    if (pending_note_) save_pending(progress_input_);

    pending_note_ = db::FocusSession{0,
                                     session_->tag,
                                     session_->next_step,
                                     "",
                                     session_->started_at,
                                     util::now_iso(),
                                     static_cast<int>(focused.count() / 1000)};
    session_.reset();
    progress_input_.clear();
    end_prompt_open_ = true;
    // TakeFocus() would also switch tabs, so only when this one is showing --
    // a round ending mid-call mustn't pull you out of the Record tab. From
    // another tab, the prompt is waiting here when you come back.
    if (root_ && root_->Active()) end_prompt_focus_->TakeFocus();
}

void FocusScreen::save_pending(const std::string& note) {
    if (!pending_note_) return;
    pending_note_->progress_note = trim(note);
    try {
        ctx_.repo->add_focus_session(*pending_note_);
        error_.clear();
    } catch (const std::exception& e) {
        error_ = std::string("Couldn't save the session: ") + e.what();
    }
    pending_note_.reset();
    end_prompt_open_ = false;
    reload_recent();
}

void FocusScreen::rebuild_parked() {
    using namespace ftxui;
    parked_revision_ = ctx_.repo->revision();
    parked_ = ctx_.repo->list_parked();
    parked_container_->DetachAllChildren();

    const ButtonOption inline_opt = link_style();
    for (const auto& item : parked_) {
        const int64_t id = item.id;
        const std::string text_copy = item.text;
        auto to_task = Button(
            "[→ task]",
            [this, id, text_copy] {
                ctx_.repo->add_task(text_copy);
                ctx_.repo->resolve_parked(id);
                flash("Added to Tasks.");
            },
            inline_opt);
        auto done = Button("[done]", [this, id] { ctx_.repo->resolve_parked(id); }, inline_opt);
        auto row = Container::Horizontal({to_task, done});
        parked_container_->Add(Renderer(row, [text_copy, to_task, done] {
            return hbox({text("• "), paragraph(text_copy) | flex, text(" "), to_task->Render(), text(" "),
                         done->Render()});
        }));
    }
}

std::string FocusScreen::week_timesheet() const {
    const std::string from = week_start_iso();
    auto sessions = ctx_.repo->list_focus_sessions_between(from, offset_days_from(from, 7));

    // day -> tag -> seconds, both in order.
    std::map<std::string, std::map<std::string, int>> by_day;
    std::map<std::string, int> totals;
    for (const auto& s : sessions) {
        by_day[s.started_at.substr(0, 10)][s.tag] += s.duration_seconds;
        totals[s.tag] += s.duration_seconds;
    }

    std::string out = "Focus time, week of " + util::iso_date_to_display(from) + "\n";
    for (const auto& [day, tags] : by_day) {
        char weekday[8];
        std::tm tm = parse_iso_day(day);
        std::strftime(weekday, sizeof(weekday), "%a", &tm);
        out += std::string(weekday) + " " + util::iso_date_to_display(day) + ":";
        for (const auto& [tag, secs] : tags) out += "  " + tag + " " + format_hm(secs);
        out += "\n";
    }
    out += "Total:";
    for (const auto& [tag, secs] : totals) out += "  " + tag + " " + format_hm(secs);
    return out + "\n";
}

void FocusScreen::flash(std::string message) {
    info_ = std::move(message);
    info_at_ = std::chrono::steady_clock::now();
}

void FocusScreen::reload_recent() {
    try {
        week_by_tag_ = ctx_.repo->focus_seconds_by_tag(week_start_iso(), offset_days_from(week_start_iso(), 7));
        recent_ = ctx_.repo->list_focus_sessions();
    } catch (const std::exception& e) {
        error_ = std::string("Couldn't load past sessions: ") + e.what();
    }
}

ftxui::Element FocusScreen::render_player(const ftxui::Component& prev_btn, const ftxui::Component& play_btn,
                                         const ftxui::Component& next_btn) {
    using namespace ftxui;
    const auto now = std::chrono::steady_clock::now();
    spotify::NowPlaying np = ctx_.spotify->now_playing();
    const int position = np.progress_now_ms(now);
    // Rendering is what drives the polling, so it only happens while this tab
    // is on screen. A track that has run out is checked on sooner, to pick up
    // the next one without a 5-second stale gap.
    const bool track_over = np.playing && np.duration_ms > 0 && position >= np.duration_ms;
    ctx_.spotify->refresh_now_playing(track_over ? std::chrono::seconds(1) : std::chrono::seconds(5));

    play_label_ = np.playing ? "Pause" : "Play";

    Element info;
    if (!np.active) {
        info = text("Nothing playing -- p starts your focus playlist.") | dim;
    } else if (np.track.empty()) {
        info = text(np.playing ? "Playing" : "Paused") | dim;
    } else {
        info = hbox({
            text(np.playing ? "▶ " : "❚❚ ") | color(theme::kSuccess),
            text(np.track) | bold,
            text(np.artists.empty() ? "" : "  " + np.artists) | dim,
        });
    }

    Elements progress_row;
    if (np.active && np.duration_ms > 0) {
        float fraction = std::min(1.0f, static_cast<float>(position) / static_cast<float>(np.duration_ms));
        progress_row = {
            text(format_track_time(position) + " ") | dim,
            gauge(fraction) | color(theme::kSuccess) | flex,
            text(" " + format_track_time(np.duration_ms)) | dim,
        };
    } else {
        progress_row = {filler()};
    }

    std::string footer = np.device.empty() ? "" : "on " + np.device + " · ";
    footer += "p play/pause · n next · b back";

    Elements rows = {
        info,
        hbox({prev_btn->Render(), play_btn->Render(), next_btn->Render(), text("  "),
              hbox(std::move(progress_row)) | vcenter | flex}),
        text(footer) | dim,
    };
    // Status only when it says something went wrong; "Playing your focus
    // playlist" is already obvious from the player itself.
    std::string status = ctx_.spotify->status();
    if (status.rfind("Couldn't", 0) == 0 || status.rfind("No Spotify", 0) == 0 || status.rfind("Not ", 0) == 0)
        rows.push_back(paragraph(status) | color(theme::kWarning));
    return section("Spotify", vbox(std::move(rows)), theme::kSuccess);
}

void FocusScreen::build_ui() {
    using namespace ftxui;

    // -- start prompt -------------------------------------------------------
    // Horizontal, so Up/Down are free to move between the two fields: a
    // vertical Radiobox keeps them (and Tab) for itself.
    InputOption next_opt = styled_input_option();
    next_opt.multiline = false;
    next_opt.on_enter = [this] { begin_focus(); };
    auto next_input = WrappedInput(&next_step_input_, "What's the next step?", next_opt);
    MenuOption tag_opt = MenuOption::Horizontal();
    tag_opt.on_enter = [next_input] { next_input->TakeFocus(); };
    auto tag_toggle = Menu(&tags_, &tag_selected_, tag_opt);
    auto go_btn = Button("Start focus", [this] { begin_focus(); }, button_style(theme::kDanger));
    auto warmup_btn = Button(
        "Just " + std::to_string(kWarmupMinutes) + " min", [this] { begin_focus(/*warmup=*/true); },
        button_style(theme::kWarning));
    auto cancel_btn = Button("Cancel", [this] { start_prompt_open_ = false; }, button_style(theme::kMuted));
    auto start_container = Container::Vertical({tag_toggle, next_input, Container::Horizontal({go_btn, warmup_btn, cancel_btn})});
    start_prompt_focus_ = tag_toggle;
    auto start_modal = Renderer(start_container, [this, tag_toggle, next_input, go_btn, warmup_btn, cancel_btn] {
                           return vbox({
                                      text("New focus round") | bold | color(theme::kDanger),
                                      text(std::to_string(ctx_.settings.focus_minutes) +
                                           (ctx_.settings.focus_minutes == 1 ? " minute" : " minutes")) |
                                          dim,
                                      separator() | color(theme::kMuted),
                                      hbox({text("Project:   "), tag_toggle->Render()}),
                                      text(""),
                                      text("Next step:"),
                                      next_input->Render(),
                                      text("←/→ project · ↓ next step · Enter start · Esc cancel") | dim,
                                      separator() | color(theme::kMuted),
                                      hbox({go_btn->Render(), text("  "), warmup_btn->Render(), text("  "),
                                            cancel_btn->Render()}) |
                                          hcenter,
                                      text("Can't get going? \"Just " + std::to_string(kWarmupMinutes) +
                                           " min\" asks if you want to carry on after that.") |
                                          dim,
                                  }) |
                                  size(WIDTH, EQUAL, 60) | borderStyled(ROUNDED, theme::kDanger) |
                                  bgcolor(Color::Black) | clear_under | center;
                       }) |
                       CatchEvent([this](const Event& e) {
                           if (e != Event::Escape) return false;
                           start_prompt_open_ = false;
                           return true;
                       });

    // -- end prompt ---------------------------------------------------------
    InputOption note_opt;
    note_opt.multiline = true;
    auto note_input = WrappedInput(&progress_input_, "e.g. parser handles RRULE, EXDATE still to do", note_opt);
    auto save_note_btn =
        Button("Save note", [this] { save_pending(progress_input_); }, button_style(theme::kAccent));
    auto no_note_btn =
        Button("Save without a note", [this] { save_pending(""); }, button_style(theme::kMuted));
    auto end_container = Container::Vertical({note_input, Container::Horizontal({save_note_btn, no_note_btn})});
    end_prompt_focus_ = note_input;
    auto end_modal = Renderer(end_container, [this, note_input, save_note_btn, no_note_btn] {
                         Elements rows = {text("Focus round over") | bold | color(theme::kSuccess)};
                         if (pending_note_) {
                             rows.push_back(text(pending_note_->tag + " · " +
                                                 format_minutes(pending_note_->duration_seconds) + " focused") |
                                            dim);
                             if (!pending_note_->next_step.empty())
                                 rows.push_back(paragraph("You planned: " + pending_note_->next_step) | dim);
                         }
                         rows.push_back(separator() | color(theme::kMuted));
                         rows.push_back(text("Where did you get to?"));
                         rows.push_back(note_input->Render() | size(HEIGHT, GREATER_THAN, 3) |
                                        size(HEIGHT, LESS_THAN, 8) | borderStyled(ROUNDED, theme::kMuted));
                         rows.push_back(separator() | color(theme::kMuted));
                         rows.push_back(hbox({save_note_btn->Render(), text("  "), no_note_btn->Render()}) | hcenter);
                         return vbox(std::move(rows)) | size(WIDTH, EQUAL, 64) |
                                borderStyled(ROUNDED, theme::kSuccess) | bgcolor(Color::Black) | clear_under | center;
                     }) |
                     CatchEvent([this](const Event& e) {
                         // Esc keeps whatever was typed rather than throwing it away.
                         if (e != Event::Escape) return false;
                         save_pending(progress_input_);
                         return true;
                     });

    // -- warm-up over ---------------------------------------------------------
    auto keep_btn = Button("Keep going", [this] { keep_going(); }, button_style(theme::kDanger));
    auto stop_btn = Button("Stop here", [this] { stop_after_warmup(); }, button_style(theme::kMuted));
    auto warmup_container = Container::Horizontal({keep_btn, stop_btn});
    warmup_prompt_focus_ = keep_btn;
    auto warmup_modal = Renderer(warmup_container, [this, keep_btn, stop_btn] {
        return vbox({
                   text(std::to_string(kWarmupMinutes) + " minutes done") | bold | color(theme::kSuccess),
                   text("That was the hard part. Keep going for the rest of the round?"),
                   text("Stopping is fine too -- the time still gets its note.") | dim,
                   separator() | color(theme::kMuted),
                   hbox({keep_btn->Render(), text("  "), stop_btn->Render()}) | hcenter,
               }) |
               size(WIDTH, EQUAL, 60) | borderStyled(ROUNDED, theme::kSuccess) | bgcolor(Color::Black) | clear_under |
               center;
    });

    // -- main view ----------------------------------------------------------
    parked_container_ = Container::Vertical({});
    auto copy_week_btn = Button(
        "[copy timesheet]",
        [this] { flash(util::copy_to_clipboard(week_timesheet()) ? "Timesheet copied." : "No clipboard tool found."); },
        link_style());
    auto start_btn = Button(&start_label_, [this] { toggle(); }, button_style(theme::kAccent));
    auto skip_btn = Button("Skip", [this] { skip(); }, button_style(theme::kMuted));
    auto reset_btn = Button("Reset", [this] { reset(); }, button_style(theme::kMuted));

    auto prev_btn = Button("« Prev", [this] { ctx_.spotify->previous_track(); }, button_style(theme::kMuted));
    auto play_btn = Button(
        &play_label_, [this] { ctx_.spotify->toggle_playback(ctx_.settings.spotify_playlist_uri); },
        button_style(theme::kSuccess));
    auto next_btn = Button("Next »", [this] { ctx_.spotify->next_track(); }, button_style(theme::kMuted));
    auto player_buttons = Container::Horizontal({prev_btn, play_btn, next_btn});

    auto history = ScrollableText(
        [this] {
            std::string out;
            for (const auto& s : recent_) {
                std::string date = s.started_at.substr(0, 10);
                std::string time = s.started_at.size() >= 16 ? s.started_at.substr(11, 5) : "";
                out += util::iso_date_to_display(date) + " " + time + " · " + format_minutes(s.duration_seconds) +
                       " · " + s.tag + "\n";
                if (!s.next_step.empty()) out += "  Planned: " + s.next_step + "\n";
                if (!s.progress_note.empty()) out += "  Got to:  " + s.progress_note + "\n";
                out += "\n";
            }
            return out;
        },
        "No focus rounds yet.", "Recent focus rounds");

    auto layout = Container::Vertical({
        Container::Horizontal({start_btn, skip_btn, reset_btn}),
        Maybe(player_buttons, [this] { return ctx_.spotify->connected(); }),
        parked_container_,
        copy_week_btn,
        history,
    });

    root_ = Renderer(layout, [this, start_btn, skip_btn, reset_btn, prev_btn, play_btn, next_btn, copy_week_btn,
                              history] {
        if (ctx_.repo->revision() != parked_revision_) rebuild_parked();
        nudge_minutes_.store(ctx_.settings.hyperfocus_nudge_minutes);
        const focus::Durations durations{ctx_.settings.focus_minutes, ctx_.settings.short_break_minutes,
                                         ctx_.settings.long_break_minutes};
        focus::Phase phase;
        bool running;
        int rounds;
        float progress;
        std::chrono::milliseconds remaining, elapsed;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // Picked up live, so a change saved in Settings applies straight away.
            timer_.set_durations(durations);
            const auto now = Clock::now();
            phase = timer_.phase();
            running = timer_.running();
            rounds = timer_.completed_rounds();
            progress = timer_.progress(now);
            remaining = timer_.remaining(now);
            elapsed = timer_.elapsed(now);
        }
        const bool started = running || elapsed.count() > 0;
        start_label_ = running ? "Pause" : (started ? "Resume" : "Start");
        const Color c = phase_color(phase);

        std::string round_note =
            phase == focus::Phase::Focus
                ? "round " + std::to_string(rounds + 1) + " of " + std::to_string(focus::kRoundsBeforeLongBreak)
                : std::to_string(rounds) + " of " + std::to_string(focus::kRoundsBeforeLongBreak) + " rounds done";
        std::string phase_badge = " " + std::string(focus::phase_label(phase)) + " ";
        for (auto& ch : phase_badge) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));

        // -- timer ----------------------------------------------------------
        Element clock = big_clock(format_mm_ss(remaining)) | bold | color(c) | hcenter;
        if (!running) clock |= dim;
        Element now_line;
        if (session_) {
            std::string line = session_->tag;
            if (!session_->next_step.empty()) line += " · " + session_->next_step;
            now_line = paragraph(line) | color(theme::kAccent) | hcenter;
        } else {
            now_line = text("Press s to start -- you'll be asked what it's for") | dim | hcenter;
        }
        Element timer = vbox({
            hbox({
                badge(focus::phase_label(phase), c),
                text("  " + round_note) | dim,
                filler(),
                text(running ? "" : (started ? "paused " : "ready ")) | color(theme::kWarning),
            }),
            text(""),
            clock,
            text(""),
            gauge(progress) | color(c),
            now_line,
            hbox({start_btn->Render(), text(" "), skip_btn->Render(), text(" "), reset_btn->Render(), filler(),
                  text("s start/pause · k skip · r reset ") | dim | vcenter}),
            text(std::to_string(durations.focus_minutes) + " min focus · " +
                 std::to_string(durations.short_break_minutes) + " min break · " +
                 std::to_string(durations.long_break_minutes) + " min long break") |
                dim,
        });
        Elements left = {section("Timer", timer, c)};
        if (!info_.empty() && std::chrono::steady_clock::now() - info_at_ < std::chrono::seconds(3))
            left.push_back(text(" " + info_) | color(theme::kSuccess));
        if (!error_.empty()) left.push_back(paragraph(error_) | color(theme::kDanger));

        // -- side panels ----------------------------------------------------
        Elements right;
        if (ctx_.spotify->connected()) {
            right.push_back(render_player(prev_btn, play_btn, next_btn));
        } else if (ctx_.spotify->configured()) {
            std::string spotify = ctx_.spotify->status();
            if (spotify.empty()) spotify = "Not connected -- Connect in Settings.";
            right.push_back(section("Spotify", paragraph(spotify) | dim, theme::kSuccess));
        }

        const bool on_break = phase != focus::Phase::Focus;
        Element parked;
        if (parked_.empty()) {
            parked = text("^P from any tab parks a stray thought here.") | dim;
        } else {
            // Kept small while focusing: they're parked so they stop pulling
            // at you, not to sit in full view.
            parked = on_break ? parked_container_->Render()
                              : parked_container_->Render() | size(HEIGHT, LESS_THAN, 4) | dim;
        }
        right.push_back(section(on_break && !parked_.empty() ? "Parked -- deal with these on your break"
                                                             : "Parked thoughts" + (parked_.empty()
                                                                                        ? std::string()
                                                                                        : " (" + std::to_string(
                                                                                                     parked_.size()) +
                                                                                              ")"),
                                parked, on_break && !parked_.empty() ? theme::kWarning : theme::kAccent));

        Elements week;
        if (week_by_tag_.empty()) week.push_back(text("Nothing yet") | dim);
        for (size_t i = 0; i < week_by_tag_.size(); ++i) {
            if (i) week.push_back(text(" · ") | dim);
            week.push_back(text(week_by_tag_[i].first + " ") | bold);
            week.push_back(text(format_hm(week_by_tag_[i].second)));
        }
        week.push_back(filler());
        week.push_back(copy_week_btn->Render());
        right.push_back(section("This week", hbox(std::move(week))));

        Elements children;
        // Side by side when there's room for both; stacked otherwise.
        if (ftxui::Terminal::Size().dimx >= 110) {
            children.push_back(hbox({vbox(std::move(left)) | flex, text(" "),
                                     vbox(std::move(right)) | size(WIDTH, EQUAL, 52)}));
        } else {
            for (auto& e : left) children.push_back(std::move(e));
            for (auto& e : right) children.push_back(std::move(e));
        }
        children.push_back(history->Render() | flex);
        return vbox(children) | flex;
    });

    root_ |= Modal(start_modal, &start_prompt_open_);
    root_ |= Modal(end_modal, &end_prompt_open_);
    root_ |= Modal(warmup_modal, &warmup_prompt_open_);
}

}  // namespace callscribe::ui
