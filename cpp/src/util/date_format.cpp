#include "util/date_format.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>

#include "util/time.hpp"

namespace callscribe::util {

namespace {

std::string lower_trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n.\"'");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n.\"'");
    std::string out = s.substr(start, end - start + 1);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// `reference_iso` + `days`, normalized through mktime so month/year rollovers
// take care of themselves. Falls back to today if the reference doesn't parse.
std::string iso_plus_days(const std::string& reference_iso, int days) {
    int y = 0, m = 0, d = 0;
    if (std::sscanf(reference_iso.c_str(), "%d-%d-%d", &y, &m, &d) != 3) return offset_days_iso(days);
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = m - 1;
    tm.tm_mday = d + days;
    tm.tm_hour = 12;  // midday, so a DST shift can't tip the result onto another day
    if (std::mktime(&tm) == -1) return "";
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

// Weekday index (0 = Sunday) of an ISO date, or -1 if it doesn't parse.
int iso_weekday(const std::string& reference_iso) {
    int y = 0, m = 0, d = 0;
    if (std::sscanf(reference_iso.c_str(), "%d-%d-%d", &y, &m, &d) != 3) return -1;
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = m - 1;
    tm.tm_mday = d;
    tm.tm_hour = 12;
    if (std::mktime(&tm) == -1) return -1;
    return tm.tm_wday;
}

}  // namespace

std::string iso_date_to_display(const std::string& iso_date) {
    int y, m, d;
    if (std::sscanf(iso_date.c_str(), "%d-%d-%d", &y, &m, &d) != 3) return iso_date;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d %02d %04d", d, m, y);
    return buf;
}

std::string display_date_to_iso(const std::string& display_date) {
    std::string normalized = display_date;
    for (auto& c : normalized) {
        if (c == '-' || c == '/') c = ' ';
    }
    int d, m, y;
    if (std::sscanf(normalized.c_str(), "%d %d %d", &d, &m, &y) != 3) return "";
    if (y < 100) y += 2000;
    if (m < 1 || m > 12 || d < 1 || d > 31) return "";
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, m, d);
    return buf;
}

std::string normalize_due_date(const std::string& text, const std::string& reference_iso) {
    const std::string s = lower_trim(text);
    if (s.empty()) return "";
    // Placeholders models emit instead of leaving the field empty.
    if (s == "-" || s == "n/a" || s == "na" || s == "none" || s == "null" || s == "tbd" || s == "asap" ||
        s.find("not specified") != std::string::npos || s.find("no due") != std::string::npos ||
        s.find("unspecified") != std::string::npos || s.find("not set") != std::string::npos)
        return "";

    const std::string base = reference_iso.empty() ? today_iso() : reference_iso;

    // Already ISO (possibly with a time part hanging off it).
    int y = 0, m = 0, d = 0;
    if (std::sscanf(s.c_str(), "%4d-%2d-%2d", &y, &m, &d) == 3 && m >= 1 && m <= 12 && d >= 1 && d <= 31) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, m, d);
        return buf;
    }

    if (s == "today" || s == "eod" || s == "end of day") return base;
    if (s == "tomorrow" || s == "tom" || s == "next day") return iso_plus_days(base, 1);
    if (s == "next week") return iso_plus_days(base, 7);
    if (s == "end of week" || s == "eow" || s == "this week") {
        int wd = iso_weekday(base);  // aim at Friday
        if (wd < 0) return "";
        int delta = (5 - wd + 7) % 7;
        return iso_plus_days(base, delta == 0 ? 7 : delta);
    }
    if (s == "next month") return iso_plus_days(base, 30);

    static const char* kDays[7] = {"sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday"};
    for (int i = 0; i < 7; ++i) {
        if (s != kDays[i] && s != std::string("next ") + kDays[i] && s != std::string("this ") + kDays[i]) continue;
        int wd = iso_weekday(base);
        if (wd < 0) return "";
        int delta = (i - wd + 7) % 7;
        return iso_plus_days(base, delta == 0 ? 7 : delta);  // "friday" said on a Friday means the next one
    }

    return display_date_to_iso(s);
}

}  // namespace callscribe::util
