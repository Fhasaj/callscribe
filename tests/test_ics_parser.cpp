#include <doctest/doctest.h>

#include <ctime>
#include <string>

#include "calendar/ics_parser.hpp"

using namespace callscribe::calendar;

namespace {

// A UTC instant, so the expectations below don't depend on the machine's zone.
std::time_t utc(int y, int mo, int d, int h = 0, int mi = 0) {
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    return timegm(&tm);
}

std::string wrap(const std::string& vevents) {
    return "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//Microsoft Corporation//Outlook//EN\r\n" + vevents +
            "END:VCALENDAR\r\n";
}

}  // namespace

TEST_CASE("unfold joins continuation lines") {
    // The fold is CRLF *plus* the whitespace that follows it -- both go away,
    // so a folded word rejoins with no space inserted.
    CHECK(unfold("SUMMARY:Hello\r\n World") == "SUMMARY:HelloWorld");
    CHECK(unfold("A:1\r\nB:2\r\n") == "A:1\nB:2\n");
    CHECK(unfold("DESC:one\r\n\ttwo") == "DESC:onetwo");
}

TEST_CASE("unescape_text handles the RFC 5545 escapes") {
    CHECK(unescape_text("line one\\nline two") == "line one\nline two");
    CHECK(unescape_text("Smith\\, John") == "Smith, John");
    CHECK(unescape_text("a\\;b") == "a;b");
    CHECK(unescape_text("back\\\\slash") == "back\\slash");
}

TEST_CASE("windows_tz_to_iana maps Exchange zone names") {
    CHECK(windows_tz_to_iana("W. Europe Standard Time") == "Europe/Berlin");
    CHECK(windows_tz_to_iana("GMT Standard Time") == "Europe/London");
    CHECK(windows_tz_to_iana("(UTC+01:00) W. Europe Standard Time") == "Europe/Berlin");
    CHECK(windows_tz_to_iana("Europe/Berlin") == "Europe/Berlin");  // already IANA
    CHECK(windows_tz_to_iana("Nowhere Standard Time") == "Nowhere Standard Time");
}

TEST_CASE("a single UTC event parses with its times and metadata") {
    std::string ics = wrap(
        "BEGIN:VEVENT\r\n"
        "UID:abc-123\r\n"
        "SUMMARY:Quarterly review\r\n"
        "LOCATION:Room 4\r\n"
        "ORGANIZER;CN=\"Doe, Jane\":mailto:jane@example.com\r\n"
        "ATTENDEE;CN=Bob:mailto:bob@example.com\r\n"
        "DTSTART:20260804T090000Z\r\n"
        "DTEND:20260804T100000Z\r\n"
        "END:VEVENT\r\n");

    auto events = parse_ics(ics, utc(2026, 8, 1), utc(2026, 8, 10));
    REQUIRE(events.size() == 1);
    CHECK(events[0].uid == "abc-123");
    CHECK(events[0].summary == "Quarterly review");
    CHECK(events[0].location == "Room 4");
    CHECK(events[0].organizer == "Doe, Jane");  // quoted param value keeps its comma
    REQUIRE(events[0].attendees.size() == 1);
    CHECK(events[0].attendees[0] == "Bob");
    CHECK(events[0].start == utc(2026, 8, 4, 9, 0));
    CHECK(events[0].end == utc(2026, 8, 4, 10, 0));
    CHECK_FALSE(events[0].recurring);
}

TEST_CASE("events outside the window are dropped") {
    std::string ics = wrap(
        "BEGIN:VEVENT\r\nUID:x\r\nSUMMARY:Old\r\nDTSTART:20250101T090000Z\r\nDTEND:20250101T100000Z\r\nEND:VEVENT\r\n");
    CHECK(parse_ics(ics, utc(2026, 8, 1), utc(2026, 8, 10)).empty());
}

TEST_CASE("an event straddling the window edge is kept") {
    std::string ics = wrap(
        "BEGIN:VEVENT\r\nUID:x\r\nSUMMARY:Long\r\nDTSTART:20260731T230000Z\r\nDTEND:20260801T010000Z\r\nEND:VEVENT\r\n");
    CHECK(parse_ics(ics, utc(2026, 8, 1), utc(2026, 8, 2)).size() == 1);
}

TEST_CASE("DURATION is used when DTEND is absent") {
    std::string ics = wrap(
        "BEGIN:VEVENT\r\nUID:x\r\nSUMMARY:Standup\r\nDTSTART:20260804T090000Z\r\nDURATION:PT15M\r\nEND:VEVENT\r\n");
    auto events = parse_ics(ics, utc(2026, 8, 1), utc(2026, 8, 10));
    REQUIRE(events.size() == 1);
    CHECK(events[0].end - events[0].start == 15 * 60);
}

TEST_CASE("cancelled events are dropped") {
    std::string ics = wrap(
        "BEGIN:VEVENT\r\nUID:x\r\nSUMMARY:Gone\r\nSTATUS:CANCELLED\r\nDTSTART:20260804T090000Z\r\n"
        "DTEND:20260804T100000Z\r\nEND:VEVENT\r\n");
    CHECK(parse_ics(ics, utc(2026, 8, 1), utc(2026, 8, 10)).empty());
}

TEST_CASE("TRANSP:TRANSPARENT marks an event as not busy") {
    std::string ics = wrap(
        "BEGIN:VEVENT\r\nUID:x\r\nSUMMARY:Focus\r\nTRANSP:TRANSPARENT\r\nDTSTART:20260804T090000Z\r\n"
        "DTEND:20260804T100000Z\r\nEND:VEVENT\r\n");
    auto events = parse_ics(ics, utc(2026, 8, 1), utc(2026, 8, 10));
    REQUIRE(events.size() == 1);
    CHECK_FALSE(events[0].busy);
}

TEST_CASE("a Teams join link is lifted out of the description") {
    std::string ics = wrap(
        "BEGIN:VEVENT\r\nUID:x\r\nSUMMARY:Sync\r\n"
        "DESCRIPTION:Click here to join:\\n<https://teams.microsoft.com/l/meetup-join/19%3ameeting_abc>\\n"
        "Thanks\r\n"
        "DTSTART:20260804T090000Z\r\nDTEND:20260804T100000Z\r\nEND:VEVENT\r\n");
    auto events = parse_ics(ics, utc(2026, 8, 1), utc(2026, 8, 10));
    REQUIRE(events.size() == 1);
    CHECK(events[0].join_url == "https://teams.microsoft.com/l/meetup-join/19%3ameeting_abc");
}

TEST_CASE("a folded description is reassembled before parsing") {
    std::string ics = wrap(
        "BEGIN:VEVENT\r\nUID:x\r\nSUMMARY:Sync\r\n"
        "DESCRIPTION:Join here https://teams.microsoft.com/l/meetup-join/19%3amee\r\n"
        " ting_folded\r\n"
        "DTSTART:20260804T090000Z\r\nDTEND:20260804T100000Z\r\nEND:VEVENT\r\n");
    auto events = parse_ics(ics, utc(2026, 8, 1), utc(2026, 8, 10));
    REQUIRE(events.size() == 1);
    CHECK(events[0].join_url == "https://teams.microsoft.com/l/meetup-join/19%3ameeting_folded");
}

TEST_CASE("a daily rule with COUNT expands to exactly COUNT occurrences") {
    std::string ics = wrap(
        "BEGIN:VEVENT\r\nUID:x\r\nSUMMARY:Daily\r\nDTSTART:20260803T090000Z\r\nDTEND:20260803T091500Z\r\n"
        "RRULE:FREQ=DAILY;COUNT=3\r\nEND:VEVENT\r\n");
    auto events = parse_ics(ics, utc(2026, 8, 1), utc(2026, 9, 1));
    REQUIRE(events.size() == 3);
    CHECK(events[0].start == utc(2026, 8, 3, 9, 0));
    CHECK(events[1].start == utc(2026, 8, 4, 9, 0));
    CHECK(events[2].start == utc(2026, 8, 5, 9, 0));
    CHECK(events[0].recurring);
}

TEST_CASE("a weekly rule with several BYDAYs expands within the window") {
    // 2026-08-03 is a Monday.
    std::string ics = wrap(
        "BEGIN:VEVENT\r\nUID:x\r\nSUMMARY:Standup\r\nDTSTART:20260803T080000Z\r\nDTEND:20260803T081500Z\r\n"
        "RRULE:FREQ=WEEKLY;BYDAY=MO,WE,FR\r\nEND:VEVENT\r\n");
    auto events = parse_ics(ics, utc(2026, 8, 3), utc(2026, 8, 10));
    REQUIRE(events.size() == 3);
    CHECK(events[0].start == utc(2026, 8, 3, 8, 0));   // Mon
    CHECK(events[1].start == utc(2026, 8, 5, 8, 0));   // Wed
    CHECK(events[2].start == utc(2026, 8, 7, 8, 0));   // Fri
}

TEST_CASE("INTERVAL=2 skips alternate weeks") {
    std::string ics = wrap(
        "BEGIN:VEVENT\r\nUID:x\r\nSUMMARY:Biweekly\r\nDTSTART:20260803T080000Z\r\nDTEND:20260803T090000Z\r\n"
        "RRULE:FREQ=WEEKLY;INTERVAL=2;BYDAY=MO\r\nEND:VEVENT\r\n");
    auto events = parse_ics(ics, utc(2026, 8, 1), utc(2026, 9, 1));
    REQUIRE(events.size() == 3);
    CHECK(events[0].start == utc(2026, 8, 3, 8, 0));
    CHECK(events[1].start == utc(2026, 8, 17, 8, 0));
    CHECK(events[2].start == utc(2026, 8, 31, 8, 0));
}

TEST_CASE("UNTIL bounds a recurring series") {
    std::string ics = wrap(
        "BEGIN:VEVENT\r\nUID:x\r\nSUMMARY:Daily\r\nDTSTART:20260803T090000Z\r\nDTEND:20260803T091500Z\r\n"
        "RRULE:FREQ=DAILY;UNTIL=20260805T090000Z\r\nEND:VEVENT\r\n");
    auto events = parse_ics(ics, utc(2026, 8, 1), utc(2026, 9, 1));
    REQUIRE(events.size() == 3);
    CHECK(events.back().start == utc(2026, 8, 5, 9, 0));
}

TEST_CASE("a monthly nth-weekday rule lands on the right day") {
    // Third Thursday of each month.
    std::string ics = wrap(
        "BEGIN:VEVENT\r\nUID:x\r\nSUMMARY:Board\r\nDTSTART:20260820T140000Z\r\nDTEND:20260820T150000Z\r\n"
        "RRULE:FREQ=MONTHLY;BYDAY=3TH\r\nEND:VEVENT\r\n");
    auto events = parse_ics(ics, utc(2026, 8, 1), utc(2026, 11, 1));
    REQUIRE(events.size() == 3);
    CHECK(events[0].start == utc(2026, 8, 20, 14, 0));
    CHECK(events[1].start == utc(2026, 9, 17, 14, 0));
    CHECK(events[2].start == utc(2026, 10, 15, 14, 0));
}

TEST_CASE("a monthly last-weekday rule counts back from the month end") {
    std::string ics = wrap(
        "BEGIN:VEVENT\r\nUID:x\r\nSUMMARY:Retro\r\nDTSTART:20260828T140000Z\r\nDTEND:20260828T150000Z\r\n"
        "RRULE:FREQ=MONTHLY;BYDAY=-1FR\r\nEND:VEVENT\r\n");
    auto events = parse_ics(ics, utc(2026, 8, 1), utc(2026, 10, 1));
    REQUIRE(events.size() == 2);
    CHECK(events[0].start == utc(2026, 8, 28, 14, 0));
    CHECK(events[1].start == utc(2026, 9, 25, 14, 0));
}

TEST_CASE("a yearly rule repeats on the same date") {
    std::string ics = wrap(
        "BEGIN:VEVENT\r\nUID:x\r\nSUMMARY:Kickoff\r\nDTSTART:20260804T090000Z\r\nDTEND:20260804T100000Z\r\n"
        "RRULE:FREQ=YEARLY\r\nEND:VEVENT\r\n");
    auto events = parse_ics(ics, utc(2026, 1, 1), utc(2029, 1, 1));
    REQUIRE(events.size() == 3);
    CHECK(events[2].start == utc(2028, 8, 4, 9, 0));
}

TEST_CASE("EXDATE removes a single occurrence from a series") {
    std::string ics = wrap(
        "BEGIN:VEVENT\r\nUID:x\r\nSUMMARY:Daily\r\nDTSTART:20260803T090000Z\r\nDTEND:20260803T091500Z\r\n"
        "RRULE:FREQ=DAILY;COUNT=3\r\nEXDATE:20260804T090000Z\r\nEND:VEVENT\r\n");
    auto events = parse_ics(ics, utc(2026, 8, 1), utc(2026, 9, 1));
    REQUIRE(events.size() == 2);
    CHECK(events[0].start == utc(2026, 8, 3, 9, 0));
    CHECK(events[1].start == utc(2026, 8, 5, 9, 0));
}

TEST_CASE("RECURRENCE-ID moves one occurrence without disturbing the rest") {
    std::string ics = wrap(
        "BEGIN:VEVENT\r\nUID:x\r\nSUMMARY:Daily\r\nDTSTART:20260803T090000Z\r\nDTEND:20260803T091500Z\r\n"
        "RRULE:FREQ=DAILY;COUNT=3\r\nEND:VEVENT\r\n"
        "BEGIN:VEVENT\r\nUID:x\r\nRECURRENCE-ID:20260804T090000Z\r\nSUMMARY:Daily (moved)\r\n"
        "DTSTART:20260804T140000Z\r\nDTEND:20260804T141500Z\r\nEND:VEVENT\r\n");
    auto events = parse_ics(ics, utc(2026, 8, 1), utc(2026, 9, 1));
    REQUIRE(events.size() == 3);
    CHECK(events[0].start == utc(2026, 8, 3, 9, 0));
    CHECK(events[1].start == utc(2026, 8, 4, 14, 0));
    CHECK(events[1].summary == "Daily (moved)");
    CHECK(events[2].start == utc(2026, 8, 5, 9, 0));
}

TEST_CASE("a cancelled single occurrence drops out of its series") {
    std::string ics = wrap(
        "BEGIN:VEVENT\r\nUID:x\r\nSUMMARY:Daily\r\nDTSTART:20260803T090000Z\r\nDTEND:20260803T091500Z\r\n"
        "RRULE:FREQ=DAILY;COUNT=3\r\nEND:VEVENT\r\n"
        "BEGIN:VEVENT\r\nUID:x\r\nRECURRENCE-ID:20260804T090000Z\r\nSTATUS:CANCELLED\r\nSUMMARY:Daily\r\n"
        "DTSTART:20260804T090000Z\r\nDTEND:20260804T091500Z\r\nEND:VEVENT\r\n");
    auto events = parse_ics(ics, utc(2026, 8, 1), utc(2026, 9, 1));
    REQUIRE(events.size() == 2);
    CHECK(events[0].start == utc(2026, 8, 3, 9, 0));
    CHECK(events[1].start == utc(2026, 8, 5, 9, 0));
}

TEST_CASE("an all-day event is flagged and spans the day") {
    std::string ics = wrap(
        "BEGIN:VEVENT\r\nUID:x\r\nSUMMARY:Public holiday\r\nDTSTART;VALUE=DATE:20260804\r\n"
        "DTEND;VALUE=DATE:20260805\r\nEND:VEVENT\r\n");
    auto events = parse_ics(ics, utc(2026, 8, 1), utc(2026, 8, 10));
    REQUIRE(events.size() == 1);
    CHECK(events[0].all_day);
    CHECK(events[0].end - events[0].start == 86400);
}

TEST_CASE("a TZID event keeps its wall-clock time across a DST change") {
    // 09:00 Berlin is 07:00Z in summer and 08:00Z after the October switch --
    // the meeting must not drift in local terms.
    std::string ics = wrap(
        "BEGIN:VEVENT\r\nUID:x\r\nSUMMARY:Weekly\r\n"
        "DTSTART;TZID=W. Europe Standard Time:20261019T090000\r\n"
        "DTEND;TZID=W. Europe Standard Time:20261019T100000\r\n"
        "RRULE:FREQ=WEEKLY;BYDAY=MO;COUNT=3\r\nEND:VEVENT\r\n");
    auto events = parse_ics(ics, utc(2026, 10, 1), utc(2026, 12, 1));
    REQUIRE(events.size() == 3);
    // EU summer time ends on 25 Oct 2026, between the first and second
    // occurrence: 09:00 local stays 09:00 local, which is one hour *later* in
    // UTC terms. The third gap is a plain week, both ends on CET.
    CHECK(events[1].start - events[0].start == 7 * 86400 + 3600);
    CHECK(events[2].start - events[1].start == 7 * 86400);
    CHECK(events[0].start == utc(2026, 10, 19, 7, 0));  // 09:00 CEST
    CHECK(events[1].start == utc(2026, 10, 26, 8, 0));  // 09:00 CET
}

TEST_CASE("malformed components are skipped without losing the good ones") {
    std::string ics = wrap(
        "BEGIN:VEVENT\r\nUID:broken\r\nSUMMARY:No start\r\nEND:VEVENT\r\n"
        "BEGIN:VEVENT\r\nUID:ok\r\nSUMMARY:Fine\r\nDTSTART:20260804T090000Z\r\nDTEND:20260804T100000Z\r\nEND:VEVENT\r\n");
    auto events = parse_ics(ics, utc(2026, 8, 1), utc(2026, 8, 10));
    REQUIRE(events.size() == 1);
    CHECK(events[0].summary == "Fine");
}

TEST_CASE("empty and non-calendar input yield no events") {
    CHECK(parse_ics("", utc(2026, 8, 1), utc(2026, 8, 10)).empty());
    CHECK(parse_ics("<html>sign in</html>", utc(2026, 8, 1), utc(2026, 8, 10)).empty());
}
