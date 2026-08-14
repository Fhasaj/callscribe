#include <doctest/doctest.h>

#include <ctime>

#include "calendar/call_match.hpp"

using namespace callscribe;

namespace {

// Local-time epoch seconds for a date/time, so the expectations don't depend
// on the machine's timezone.
std::time_t at(int year, int month, int day, int hour, int minute) {
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_isdst = -1;
    return std::mktime(&tm);
}

calendar::Event event_at(const std::string& summary, std::time_t start, std::time_t end) {
    calendar::Event e;
    e.summary = summary;
    e.start = start;
    e.end = end;
    return e;
}

db::Call call_at(const std::string& meeting, const std::string& started, const std::string& ended,
                 const std::string& status = "done") {
    db::Call c;
    c.id = 1;
    c.meeting_name = meeting;
    c.started_at = started;
    c.ended_at = ended;
    c.status = status;
    return c;
}

}  // namespace

TEST_CASE("a call recorded during the meeting marks it completed") {
    auto event = event_at("Steering group", at(2026, 8, 13, 10, 0), at(2026, 8, 13, 11, 0));
    std::vector<db::Call> calls = {call_at("Whatever I typed", "2026-08-13T10:04:00", "2026-08-13T10:52:00")};

    CHECK(calendar::recorded_call_for_event(calls, event) != nullptr);
}

TEST_CASE("joining early or starting late still counts") {
    auto event = event_at("Steering group", at(2026, 8, 13, 10, 0), at(2026, 8, 13, 11, 0));

    std::vector<db::Call> early = {call_at("", "2026-08-13T09:47:00", "2026-08-13T10:30:00")};
    CHECK(calendar::recorded_call_for_event(early, event) != nullptr);

    std::vector<db::Call> late = {call_at("", "2026-08-13T11:12:00", "2026-08-13T11:40:00")};
    CHECK(calendar::recorded_call_for_event(late, event) != nullptr);
}

TEST_CASE("a call at an unrelated time does not mark the meeting") {
    auto event = event_at("Steering group", at(2026, 8, 13, 10, 0), at(2026, 8, 13, 11, 0));
    std::vector<db::Call> calls = {call_at("Other customer", "2026-08-13T15:00:00", "2026-08-13T15:30:00")};

    CHECK(calendar::recorded_call_for_event(calls, event) == nullptr);
}

TEST_CASE("same day plus a matching meeting name is enough") {
    auto event = event_at("Steering Group", at(2026, 8, 13, 10, 0), at(2026, 8, 13, 11, 0));
    // Recorded in the afternoon (the meeting ran late / was rescheduled), but
    // named after the event -- case-insensitively.
    std::vector<db::Call> calls = {call_at("steering group", "2026-08-13T16:00:00", "2026-08-13T16:30:00")};
    CHECK(calendar::recorded_call_for_event(calls, event) != nullptr);

    // A different day with the same name is a different meeting.
    std::vector<db::Call> other_day = {call_at("steering group", "2026-08-12T16:00:00", "2026-08-12T16:30:00")};
    CHECK(calendar::recorded_call_for_event(other_day, event) == nullptr);
}

TEST_CASE("a call that failed to process is not completed") {
    auto event = event_at("Steering group", at(2026, 8, 13, 10, 0), at(2026, 8, 13, 11, 0));
    std::vector<db::Call> calls = {call_at("", "2026-08-13T10:04:00", "2026-08-13T10:52:00", "error")};

    CHECK(calendar::recorded_call_for_event(calls, event) == nullptr);
}

TEST_CASE("an unfinished call falls back to its duration for the end time") {
    auto event = event_at("Steering group", at(2026, 8, 13, 10, 0), at(2026, 8, 13, 11, 0));
    db::Call call = call_at("", "2026-08-13T10:04:00", "");
    call.duration_seconds = 1800;
    std::vector<db::Call> calls = {call};

    CHECK(calendar::recorded_call_for_event(calls, event) != nullptr);
}

TEST_CASE("no calls at all") {
    auto event = event_at("Steering group", at(2026, 8, 13, 10, 0), at(2026, 8, 13, 11, 0));
    CHECK(calendar::recorded_call_for_event({}, event) == nullptr);
}
