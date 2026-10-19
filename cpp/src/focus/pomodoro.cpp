#include "focus/pomodoro.hpp"

#include <algorithm>

namespace callscribe::focus {

const char* phase_label(Phase phase) {
    switch (phase) {
        case Phase::Focus: return "Focus";
        case Phase::ShortBreak: return "Short break";
        case Phase::LongBreak: return "Long break";
    }
    return "";
}

PomodoroTimer::PomodoroTimer(Durations durations) : durations_(durations) {}

std::chrono::milliseconds PomodoroTimer::length() const {
    int minutes = warmup_minutes_ > 0 ? warmup_minutes_ : durations_.focus_minutes;
    if (phase_ == Phase::ShortBreak) minutes = durations_.short_break_minutes;
    if (phase_ == Phase::LongBreak) minutes = durations_.long_break_minutes;
    return std::chrono::minutes(std::clamp(minutes, kMinMinutes, kMaxMinutes));
}

std::chrono::milliseconds PomodoroTimer::elapsed(TimePoint now) const {
    auto total = elapsed_;
    if (running_ && now > last_) total += std::chrono::duration_cast<std::chrono::milliseconds>(now - last_);
    return std::min(total, length());
}

std::chrono::milliseconds PomodoroTimer::remaining(TimePoint now) const { return length() - elapsed(now); }

float PomodoroTimer::progress(TimePoint now) const {
    return static_cast<float>(elapsed(now).count()) / static_cast<float>(length().count());
}

void PomodoroTimer::start(TimePoint now) {
    if (running_) return;
    running_ = true;
    last_ = now;
}

void PomodoroTimer::pause(TimePoint now) {
    if (!running_) return;
    elapsed_ = elapsed(now);
    running_ = false;
}

void PomodoroTimer::start_warmup(TimePoint now, int minutes) {
    if (running_ || phase_ != Phase::Focus || elapsed_.count() > 0) return;
    warmup_minutes_ = std::clamp(minutes, kMinMinutes, kMaxMinutes);
    start(now);
}

void PomodoroTimer::discard_round() {
    phase_ = Phase::Focus;
    running_ = false;
    warmup_minutes_ = 0;
    elapsed_ = std::chrono::milliseconds(0);
}

std::optional<Transition> PomodoroTimer::advance(TimePoint now) {
    if (!running_) return std::nullopt;
    elapsed_ = elapsed(now);
    last_ = now;
    if (elapsed_ < length()) return std::nullopt;
    if (warmup_minutes_ > 0) {
        // Same round, paused, now measured against the full length.
        warmup_minutes_ = 0;
        running_ = false;
        return Transition{Phase::Focus, Phase::Focus, elapsed_, true};
    }
    return finish_phase(now);
}

Transition PomodoroTimer::skip(TimePoint now) {
    elapsed_ = elapsed(now);
    return finish_phase(now);
}

void PomodoroTimer::reset() {
    warmup_minutes_ = 0;
    phase_ = Phase::Focus;
    running_ = false;
    completed_rounds_ = 0;
    elapsed_ = std::chrono::milliseconds(0);
}

Transition PomodoroTimer::finish_phase(TimePoint now) {
    warmup_minutes_ = 0;
    Transition t{phase_, Phase::Focus, elapsed_};
    if (phase_ == Phase::Focus) {
        ++completed_rounds_;
        t.to = completed_rounds_ >= kRoundsBeforeLongBreak ? Phase::LongBreak : Phase::ShortBreak;
    } else if (phase_ == Phase::LongBreak) {
        completed_rounds_ = 0;
    }

    phase_ = t.to;
    elapsed_ = std::chrono::milliseconds(0);
    last_ = now;
    running_ = t.to != Phase::Focus;
    return t;
}

}  // namespace callscribe::focus
