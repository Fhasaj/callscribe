#pragma once
#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace callscribe::focus {

enum class Phase { Focus, ShortBreak, LongBreak };

const char* phase_label(Phase phase);

// What focus rounds and wins are filed under.
inline const std::vector<std::string>& project_tags() {
    static const std::vector<std::string> tags = {"Indica", "Freelance", "SkyPin", "Other"};
    return tags;
}

// Minutes per phase. Clamped to [kMinMinutes, kMaxMinutes] wherever they come
// from user input.
struct Durations {
    int focus_minutes = 25;
    int short_break_minutes = 5;
    int long_break_minutes = 15;
};

inline constexpr int kMinMinutes = 1;
inline constexpr int kMaxMinutes = 180;
// Every fourth completed focus round earns the long break.
inline constexpr int kRoundsBeforeLongBreak = 4;

// One phase ending and the next beginning, handed back so the caller can react
// (notify, prompt for a note, start/stop music).
struct Transition {
    Phase from;
    Phase to;
    // Time actually spent in `from`, pauses excluded.
    std::chrono::milliseconds elapsed;
    // A warm-up ran out (see start_warmup): still the same focus round, now
    // paused, and `to` == `from`. Nothing has been counted yet.
    bool warmup_over = false;
};

// The Pomodoro state machine, with no thread and no clock of its own: every
// call that depends on time takes `now`, so tests can drive it with made-up
// time points and the UI drives it with steady_clock.
//
// Breaks start running on their own as soon as a focus round ends -- the point
// of a break is that it happens. A new focus round never does: it waits for
// start(), because the UI asks what you're about to work on first.
//
// Not thread-safe; the owner serializes access.
class PomodoroTimer {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    explicit PomodoroTimer(Durations durations = {});

    Phase phase() const { return phase_; }
    bool running() const { return running_; }
    // Focus rounds completed since the last long break (or reset).
    int completed_rounds() const { return completed_rounds_; }

    // Applies immediately, including to the phase in progress -- shortening it
    // below what has already elapsed ends it on the next advance().
    void set_durations(Durations durations) { durations_ = durations; }
    Durations durations() const { return durations_; }

    std::chrono::milliseconds length() const;
    std::chrono::milliseconds elapsed(TimePoint now) const;
    std::chrono::milliseconds remaining(TimePoint now) const;
    // 0 at the start of the phase, 1 at the end.
    float progress(TimePoint now) const;

    void start(TimePoint now);
    void pause(TimePoint now);

    // "Just N minutes": starts a fresh focus round that stops after `minutes`
    // to ask whether to keep going -- the way past not being able to start at
    // all. Keeping going is start(), which carries on to the full length with
    // the warm-up already counted. No-op unless a fresh focus round is waiting.
    void start_warmup(TimePoint now, int minutes);
    bool in_warmup() const { return warmup_minutes_ > 0; }
    // Drops the focus round in progress without counting it towards the long
    // break: back to a fresh, paused focus round.
    void discard_round();

    // Accounts for the time since the last call while running, and ends the
    // phase if it is up. At most one transition per call.
    std::optional<Transition> advance(TimePoint now);
    // Ends the current phase early, exactly as if it had run out.
    Transition skip(TimePoint now);
    // Back to a fresh, paused focus round with the round count cleared.
    void reset();

private:
    Durations durations_;
    Phase phase_ = Phase::Focus;
    bool running_ = false;
    int completed_rounds_ = 0;
    std::chrono::milliseconds elapsed_{0};
    TimePoint last_{};
    int warmup_minutes_ = 0;

    Transition finish_phase(TimePoint now);
};

}  // namespace callscribe::focus
