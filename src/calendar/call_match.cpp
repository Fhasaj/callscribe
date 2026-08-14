#include "calendar/call_match.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <string>

namespace callscribe::calendar {

namespace {

// "2026-08-13T14:05:00" (what the DB stores) -> epoch seconds, local time.
// 0 when it doesn't parse.
std::time_t iso_datetime_to_time(const std::string& iso) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    if (std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) < 3) return 0;
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = s;
    tm.tm_isdst = -1;
    return std::mktime(&tm);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool same_local_day(std::time_t a, std::time_t b) {
    std::tm ta{}, tb{};
    localtime_r(&a, &ta);
    localtime_r(&b, &tb);
    return ta.tm_year == tb.tm_year && ta.tm_yday == tb.tm_yday;
}

}  // namespace

const db::Call* recorded_call_for_event(const std::vector<db::Call>& calls, const Event& event) {
    constexpr double kSlackSeconds = 20 * 60;
    const std::string event_title = lower(event.summary);

    const db::Call* by_name = nullptr;
    for (const db::Call& call : calls) {
        if (call.status != "done") continue;
        std::time_t started = iso_datetime_to_time(call.started_at);
        if (started == 0) continue;

        std::time_t ended = iso_datetime_to_time(call.ended_at);
        if (ended == 0) ended = started + call.duration_seconds;
        if (started <= event.end + kSlackSeconds && ended >= event.start - kSlackSeconds) return &call;

        if (!by_name && !event_title.empty() && lower(call.meeting_name) == event_title &&
            same_local_day(started, event.start))
            by_name = &call;
    }
    return by_name;
}

}  // namespace callscribe::calendar
