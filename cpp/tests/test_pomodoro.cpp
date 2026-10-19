#include <doctest/doctest.h>

#include "focus/pomodoro.hpp"

using namespace callscribe::focus;
using namespace std::chrono_literals;

namespace {
const PomodoroTimer::TimePoint t0{};
}

TEST_CASE("pomodoro: a fresh timer is a paused, full focus round") {
    PomodoroTimer timer;
    CHECK(timer.phase() == Phase::Focus);
    CHECK_FALSE(timer.running());
    CHECK(timer.remaining(t0) == 25min);
    CHECK(timer.progress(t0) == doctest::Approx(0.0f));
}

TEST_CASE("pomodoro: focus runs out into a short break that starts by itself") {
    PomodoroTimer timer;
    timer.start(t0);
    CHECK_FALSE(timer.advance(t0 + 10min).has_value());
    CHECK(timer.remaining(t0 + 10min) == 15min);

    auto t = timer.advance(t0 + 25min);
    REQUIRE(t.has_value());
    CHECK(t->from == Phase::Focus);
    CHECK(t->to == Phase::ShortBreak);
    CHECK(t->elapsed == 25min);
    CHECK(timer.running());
    CHECK(timer.completed_rounds() == 1);
    CHECK(timer.remaining(t0 + 25min) == 5min);
}

TEST_CASE("pomodoro: a break running out waits for the next focus round to be started") {
    PomodoroTimer timer;
    timer.start(t0);
    timer.advance(t0 + 25min);
    auto t = timer.advance(t0 + 30min);
    REQUIRE(t.has_value());
    CHECK(t->to == Phase::Focus);
    CHECK_FALSE(timer.running());
    CHECK(timer.remaining(t0 + 2h) == 25min);
}

TEST_CASE("pomodoro: the fourth focus round earns the long break, which clears the count") {
    PomodoroTimer timer;
    for (int round = 1; round <= 3; ++round) {
        CHECK(timer.skip(t0).to == Phase::ShortBreak);
        CHECK(timer.skip(t0).to == Phase::Focus);
    }
    CHECK(timer.skip(t0).to == Phase::LongBreak);
    CHECK(timer.completed_rounds() == 4);
    CHECK(timer.remaining(t0) == 15min);
    CHECK(timer.skip(t0).to == Phase::Focus);
    CHECK(timer.completed_rounds() == 0);
}

TEST_CASE("pomodoro: paused time doesn't count") {
    PomodoroTimer timer;
    timer.start(t0);
    timer.pause(t0 + 5min);
    CHECK_FALSE(timer.advance(t0 + 1h).has_value());
    CHECK(timer.elapsed(t0 + 1h) == 5min);
    timer.start(t0 + 1h);
    CHECK(timer.elapsed(t0 + 1h + 1min) == 6min);
}

TEST_CASE("pomodoro: skipping reports the time actually spent") {
    PomodoroTimer timer;
    timer.start(t0);
    auto t = timer.skip(t0 + 7min);
    CHECK(t.from == Phase::Focus);
    CHECK(t.elapsed == 7min);
}

TEST_CASE("pomodoro: reset goes back to a paused focus round") {
    PomodoroTimer timer;
    timer.skip(t0);
    timer.skip(t0);
    timer.start(t0);
    timer.reset();
    CHECK(timer.phase() == Phase::Focus);
    CHECK_FALSE(timer.running());
    CHECK(timer.completed_rounds() == 0);
    CHECK(timer.elapsed(t0 + 1h).count() == 0);
}

TEST_CASE("pomodoro: durations apply live and are clamped") {
    PomodoroTimer timer({50, 10, 30});
    CHECK(timer.length() == 50min);
    timer.set_durations({0, 10, 30});
    CHECK(timer.length() == std::chrono::minutes(kMinMinutes));
    timer.set_durations({999, 10, 30});
    CHECK(timer.length() == std::chrono::minutes(kMaxMinutes));
}

TEST_CASE("pomodoro: a warm-up stops after its few minutes and asks, without counting a round") {
    PomodoroTimer timer;
    timer.start_warmup(t0, 5);
    CHECK(timer.in_warmup());
    CHECK(timer.remaining(t0) == 5min);
    auto t = timer.advance(t0 + 5min);
    REQUIRE(t.has_value());
    CHECK(t->warmup_over);
    CHECK(t->from == Phase::Focus);
    CHECK(t->to == Phase::Focus);
    CHECK_FALSE(timer.running());
    CHECK(timer.completed_rounds() == 0);
    // Keeping going carries on to the full round, the 5 minutes already in.
    CHECK(timer.remaining(t0 + 5min) == 20min);
    timer.start(t0 + 6min);
    auto full = timer.advance(t0 + 26min);
    REQUIRE(full.has_value());
    CHECK_FALSE(full->warmup_over);
    CHECK(full->to == Phase::ShortBreak);
    CHECK(full->elapsed == 25min);
    CHECK(timer.completed_rounds() == 1);
}

TEST_CASE("pomodoro: stopping after a warm-up discards the round") {
    PomodoroTimer timer;
    timer.start_warmup(t0, 5);
    timer.advance(t0 + 5min);
    timer.discard_round();
    CHECK(timer.phase() == Phase::Focus);
    CHECK(timer.completed_rounds() == 0);
    CHECK(timer.elapsed(t0 + 1h).count() == 0);
    CHECK(timer.remaining(t0 + 1h) == 25min);
}

TEST_CASE("pomodoro: a warm-up only starts a fresh focus round") {
    PomodoroTimer timer;
    timer.start(t0);
    timer.pause(t0 + 1min);
    timer.start_warmup(t0 + 2min, 5);
    CHECK_FALSE(timer.in_warmup());
    CHECK_FALSE(timer.running());
}
