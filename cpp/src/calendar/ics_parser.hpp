#pragma once
#include <ctime>
#include <string>
#include <vector>

// iCalendar (RFC 5545) reader, scoped to what Outlook/Teams actually emits.
// Deliberately not a general-purpose implementation: it covers VEVENT with
// the recurrence rules real meetings use (daily/weekly/monthly/yearly,
// INTERVAL, COUNT, UNTIL, BYDAY, BYMONTHDAY, BYMONTH), EXDATE, and
// RECURRENCE-ID overrides. VALARM, VTODO, VJOURNAL and VFREEBUSY are ignored.
namespace callscribe::calendar {

// One concrete occurrence on the calendar -- a recurring series is expanded
// into one Event per instance, so nothing downstream has to know about RRULE.
struct Event {
    std::string uid;
    std::string summary;
    std::string location;
    std::string organizer;
    std::string description;
    // Teams/Zoom/Meet link lifted out of the description or location, so the
    // agenda can show "has a join link" without dumping the whole invite body.
    std::string join_url;
    std::vector<std::string> attendees;
    std::time_t start = 0;  // epoch seconds
    std::time_t end = 0;
    bool all_day = false;
    bool recurring = false;  // this instance came from an RRULE
    bool busy = true;        // false for TRANSP:TRANSPARENT ("show as free")
    // Outlook's "show as: Out of office" (X-MICROSOFT-CDO-BUSYSTATUS:OOF).
    bool out_of_office = false;
};

// Parses an iCalendar document and returns every occurrence overlapping
// [window_start, window_end), sorted by start time. Cancelled events and
// occurrences excluded by EXDATE are dropped. Never throws on malformed
// input -- unparsable components are skipped, so a partially broken feed
// still yields the events that did parse.
std::vector<Event> parse_ics(const std::string& text, std::time_t window_start, std::time_t window_end);

// -- exposed for tests ---------------------------------------------------

// Reverses RFC 5545 line folding: a CRLF (or LF) followed by a space or tab
// is a continuation, not a line break.
std::string unfold(const std::string& text);

// Reverses TEXT escaping: \n \N -> newline, \, \; \\ -> the literal char.
std::string unescape_text(const std::string& value);

// Maps a Windows time zone name ("W. Europe Standard Time") to its IANA
// equivalent ("Europe/Berlin"). Returns the input unchanged if it already
// looks like an IANA name, or is not in the table.
std::string windows_tz_to_iana(const std::string& tzid);

}  // namespace callscribe::calendar
