#pragma once
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "context.hpp"
#include "focus/pomodoro.hpp"

namespace callscribe::ui {

// Pomodoro focus timer: focus / short break / long break (after every fourth
// round), with a note per round -- what you are about to work on when it
// starts, where you got to when it ends -- saved to focus_sessions. Optionally
// starts a Spotify playlist for the focus rounds and pauses it for the breaks.
class FocusScreen {
public:
    // `is_busy` says whether something else is holding your attention right
    // now (a call being recorded) -- it counts towards the hyperfocus check-in
    // the same as a focus round does.
    FocusScreen(AppContext& ctx, ftxui::ScreenInteractive& screen, std::function<bool()> is_busy = nullptr);
    ~FocusScreen();
    FocusScreen(const FocusScreen&) = delete;
    FocusScreen& operator=(const FocusScreen&) = delete;

    ftxui::Component component() { return root_; }

    // For the tab label: the time left while a phase is under way, so the
    // timer stays visible from every tab. Empty when idle.
    struct Badge {
        std::string text;
        ftxui::Color color;
    };
    Badge tab_badge() const;

    // For the title bar, on every tab: a hyperfocus check-in for a couple of
    // minutes after it fires, otherwise what the current round is for ("Now:
    // Indica · finish the parser"). Empty when there's nothing to say.
    Badge title_note() const;
    // The project of the round under way, "" if none -- the default for a win.
    std::string current_tag() const { return session_ ? session_->tag : ""; }

    // s = start/pause, k = skip, r = reset. Offered by the app shell, ahead of
    // the tab menu, so they work whether or not focus is inside this tab.
    // Declines everything while a prompt is open: the keys are typing then.
    bool handle_shortcut(const ftxui::Event& event);

private:
    struct Session {
        std::string tag;
        std::string next_step;
        std::string started_at;
    };

    AppContext& ctx_;
    ftxui::ScreenInteractive& screen_;

    // Guards timer_ and stopping_: the ticker thread advances the timer, the
    // UI thread starts/pauses/skips it and reads it to render.
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    focus::PomodoroTimer timer_;
    bool stopping_ = false;
    std::thread ticker_;

    // UI thread only from here down.
    std::optional<Session> session_;               // the focus round under way
    std::optional<db::FocusSession> pending_note_;  // ended round awaiting its note
    bool start_prompt_open_ = false;
    bool end_prompt_open_ = false;
    bool warmup_prompt_open_ = false;  // "5 minutes done -- keep going?"
    bool prompt_open() const { return start_prompt_open_ || end_prompt_open_ || warmup_prompt_open_; }
    std::vector<std::string> tags_ = focus::project_tags();
    int tag_selected_ = 0;
    std::string next_step_input_;
    std::string progress_input_;
    std::string start_label_ = "Start";
    std::string play_label_ = "Play";
    std::string error_;
    std::string info_;
    std::chrono::steady_clock::time_point info_at_{};
    std::vector<db::FocusSession> recent_;

    // Parking lot, rebuilt when the repository changes.
    std::vector<db::ParkedItem> parked_;
    uint64_t parked_revision_ = ~uint64_t{0};
    ftxui::Component parked_container_;

    // Focused time per project this week (Monday onwards), refreshed with the
    // session list.
    std::vector<std::pair<std::string, int>> week_by_tag_;

    // Hyperfocus check-in. The counters belong to the ticker thread; the
    // threshold is copied in from Settings by the renderer.
    std::function<bool()> is_busy_;
    std::atomic<int> nudge_minutes_{90};
    int engaged_seconds_ = 0;
    int idle_seconds_ = 0;
    int nudges_ = 0;
    // UI thread: when the last check-in fired, and for how long you'd been going.
    std::chrono::steady_clock::time_point nudged_at_{};
    int nudged_after_minutes_ = 0;

    void toggle();
    void skip();
    void reset();
    void begin_focus(bool warmup = false);
    void keep_going();
    void stop_after_warmup();
    // Ticker thread: counts a second of engaged-or-not, true when a check-in
    // is due.
    bool track_engagement(bool engaged);
    void on_hyperfocus(int minutes);
    void rebuild_parked();
    std::string week_timesheet() const;
    void flash(std::string message);
    void on_transition(const focus::Transition& t);
    void end_session(std::chrono::milliseconds focused);
    void save_pending(const std::string& note);
    void reload_recent();
    void tick_loop();
    void build_ui();
    // The Spotify mini player, shown once an account is connected.
    ftxui::Element render_player(const ftxui::Component& prev_btn, const ftxui::Component& play_btn,
                                 const ftxui::Component& next_btn);

    // What each prompt puts the keyboard on when it opens. The prompts live
    // inside this tab, so without this a prompt opened by a shortcut typed on
    // the tab menu would leave every keystroke going to the menu.
    ftxui::Component start_prompt_focus_;
    ftxui::Component end_prompt_focus_;
    ftxui::Component warmup_prompt_focus_;

    ftxui::Component root_;
};

}  // namespace callscribe::ui
