#include <doctest/doctest.h>

#include "tasks/meeting_reminder.hpp"

using namespace callscribe;
using tasks::MeetingReminder;

namespace {
calendar::Event meeting(const std::string& uid, std::time_t start, bool all_day = false, bool busy = true) {
    calendar::Event e;
    e.uid = uid;
    e.summary = "Weekly sync";
    e.start = start;
    e.end = start + 1800;
    e.all_day = all_day;
    e.busy = busy;
    return e;
}
}  // namespace

TEST_CASE("meeting reminders: inside the window, once, real meetings only") {
    const std::time_t now = 1'800'000'000;
    std::vector<calendar::Event> events = {
        meeting("soon", now + 10 * 60),
        meeting("later", now + 20 * 60),
        meeting("started", now - 60),
        meeting("allday", now + 5 * 60, /*all_day=*/true),
        meeting("free", now + 5 * 60, false, /*busy=*/false),
    };
    auto due = MeetingReminder::due(events, now, 15, {});
    REQUIRE(due.size() == 1);
    CHECK(due[0].uid == "soon");

    // Already announced: not again.
    CHECK(MeetingReminder::due(events, now, 15, {MeetingReminder::key(due[0])}).empty());
    // 0 = off.
    CHECK(MeetingReminder::due(events, now, 0, {}).empty());
    // Each occurrence of a recurring series is its own reminder.
    CHECK(MeetingReminder::key(meeting("series", now)) != MeetingReminder::key(meeting("series", now + 86400)));
}

TEST_CASE("meeting reminders: the message says how long and how to join") {
    const std::time_t now = 1'800'000'000;
    auto e = meeting("m", now + 15 * 60);
    e.join_url = "https://teams.microsoft.com/l/meetup-join/abc";
    auto [title, body] = MeetingReminder::message(e, now);
    CHECK(title == "In 15 min: Weekly sync");
    CHECK(body.find("Join: https://teams.microsoft.com/l/meetup-join/abc") != std::string::npos);
    CHECK(MeetingReminder::message(meeting("n", now + 30), now).first.rfind("Starting now", 0) == 0);
}

TEST_CASE("meeting reminders: out of office is never announced") {
    const std::time_t now = 1'800'000'000;
    auto flagged = meeting("oof", now + 10 * 60);
    flagged.summary = "Busy";
    flagged.out_of_office = true;
    auto titled = meeting("ooo", now + 10 * 60);
    titled.summary = "OOO - dentist";
    auto leave = meeting("leave", now + 10 * 60);
    leave.summary = "Annual Leave";
    auto proof = meeting("proof", now + 10 * 60);
    proof.summary = "Proof review";  // not "oof"
    auto due = MeetingReminder::due({flagged, titled, leave, proof}, now, 15, {});
    REQUIRE(due.size() == 1);
    CHECK(due[0].uid == "proof");
}
