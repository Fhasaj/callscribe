#include "calendar/ics_parser.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace callscribe::calendar {

namespace {

// -- small string helpers ------------------------------------------------

std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
    return s;
}

// -- civil date arithmetic (Howard Hinnant's algorithms) -----------------
//
// Done by hand rather than through mktime() because recurrence stepping has
// to walk calendar days in the *event's* time zone, which is not necessarily
// the machine's -- mktime() only knows the latter.

struct Date {
    int y = 1970;
    int m = 1;
    int d = 1;
    bool operator<(const Date& o) const {
        if (y != o.y) return y < o.y;
        if (m != o.m) return m < o.m;
        return d < o.d;
    }
    bool operator==(const Date& o) const { return y == o.y && m == o.m && d == o.d; }
};

long long days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097LL + static_cast<long long>(doe) - 719468;
}

Date civil_from_days(long long z) {
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long long y = static_cast<long long>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp + (mp < 10 ? 3 : -9);
    return Date{static_cast<int>(y + (m <= 2)), static_cast<int>(m), static_cast<int>(d)};
}

// 0 = Sunday .. 6 = Saturday.
int weekday_of(const Date& date) {
    long long z = days_from_civil(date.y, date.m, date.d);
    return static_cast<int>((z + 4) % 7 + 7) % 7;
}

Date add_days(const Date& date, long long n) { return civil_from_days(days_from_civil(date.y, date.m, date.d) + n); }

int days_in_month(int y, int m) {
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m < 1 || m > 12) return 30;
    if (m == 2) {
        bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
        return leap ? 29 : 28;
    }
    return kDays[m - 1];
}

struct Civil {
    Date date;
    int h = 0;
    int mi = 0;
    int s = 0;
};

// -- time zone resolution -------------------------------------------------

// Exchange writes Windows zone names ("W. Europe Standard Time"); Google and
// Apple write IANA names. Only the zones a business calendar realistically
// uses are listed -- anything unmapped falls through to the machine's local
// zone, which is the right guess when the feed came from a colleague nearby.
const std::unordered_map<std::string, std::string>& windows_tz_table() {
    static const std::unordered_map<std::string, std::string> kTable = {
        {"UTC", "Etc/UTC"},
        {"GMT Standard Time", "Europe/London"},
        {"Greenwich Standard Time", "Atlantic/Reykjavik"},
        {"W. Europe Standard Time", "Europe/Berlin"},
        {"Central Europe Standard Time", "Europe/Budapest"},
        {"Central European Standard Time", "Europe/Warsaw"},
        {"Romance Standard Time", "Europe/Paris"},
        {"E. Europe Standard Time", "Europe/Chisinau"},
        {"FLE Standard Time", "Europe/Kiev"},
        {"GTB Standard Time", "Europe/Bucharest"},
        {"Turkey Standard Time", "Europe/Istanbul"},
        {"Israel Standard Time", "Asia/Jerusalem"},
        {"Russian Standard Time", "Europe/Moscow"},
        {"Belarus Standard Time", "Europe/Minsk"},
        {"Kaliningrad Standard Time", "Europe/Kaliningrad"},
        {"Morocco Standard Time", "Africa/Casablanca"},
        {"Egypt Standard Time", "Africa/Cairo"},
        {"South Africa Standard Time", "Africa/Johannesburg"},
        {"W. Central Africa Standard Time", "Africa/Lagos"},
        {"Arabian Standard Time", "Asia/Dubai"},
        {"Arab Standard Time", "Asia/Riyadh"},
        {"Middle East Standard Time", "Asia/Beirut"},
        {"Jordan Standard Time", "Asia/Amman"},
        {"Iran Standard Time", "Asia/Tehran"},
        {"Pakistan Standard Time", "Asia/Karachi"},
        {"India Standard Time", "Asia/Kolkata"},
        {"Sri Lanka Standard Time", "Asia/Colombo"},
        {"Nepal Standard Time", "Asia/Kathmandu"},
        {"Bangladesh Standard Time", "Asia/Dhaka"},
        {"SE Asia Standard Time", "Asia/Bangkok"},
        {"Singapore Standard Time", "Asia/Singapore"},
        {"China Standard Time", "Asia/Shanghai"},
        {"Taipei Standard Time", "Asia/Taipei"},
        {"Tokyo Standard Time", "Asia/Tokyo"},
        {"Korea Standard Time", "Asia/Seoul"},
        {"West Asia Standard Time", "Asia/Tashkent"},
        {"Central Asia Standard Time", "Asia/Almaty"},
        {"AUS Eastern Standard Time", "Australia/Sydney"},
        {"E. Australia Standard Time", "Australia/Brisbane"},
        {"AUS Central Standard Time", "Australia/Darwin"},
        {"Cen. Australia Standard Time", "Australia/Adelaide"},
        {"Tasmania Standard Time", "Australia/Hobart"},
        {"W. Australia Standard Time", "Australia/Perth"},
        {"New Zealand Standard Time", "Pacific/Auckland"},
        {"Eastern Standard Time", "America/New_York"},
        {"Central Standard Time", "America/Chicago"},
        {"Mountain Standard Time", "America/Denver"},
        {"US Mountain Standard Time", "America/Phoenix"},
        {"Pacific Standard Time", "America/Los_Angeles"},
        {"Alaskan Standard Time", "America/Anchorage"},
        {"Hawaiian Standard Time", "Pacific/Honolulu"},
        {"Atlantic Standard Time", "America/Halifax"},
        {"Newfoundland Standard Time", "America/St_Johns"},
        {"Central Standard Time (Mexico)", "America/Mexico_City"},
        {"Central America Standard Time", "America/Guatemala"},
        {"SA Pacific Standard Time", "America/Bogota"},
        {"SA Western Standard Time", "America/La_Paz"},
        {"SA Eastern Standard Time", "America/Cayenne"},
        {"E. South America Standard Time", "America/Sao_Paulo"},
        {"Argentina Standard Time", "America/Argentina/Buenos_Aires"},
        {"Pacific SA Standard Time", "America/Santiago"},
        {"Venezuela Standard Time", "America/Caracas"},
    };
    return kTable;
}

// Seconds since the epoch for a wall-clock time in a named zone.
//
// Uses the C++20 time zone database, which knows historical DST rules, so a
// weekly 09:00 meeting stays at 09:00 across a DST switch instead of drifting
// an hour. If the toolchain or system has no tzdb, or the name is unknown, it
// falls back to the machine's local zone via mktime().
std::time_t to_epoch(const Civil& c, const std::string& tzid) {
    // UTC is handled without the tzdb -- it is by far the most common case in
    // Exchange feeds and timegm() is always available.
    if (tzid == "UTC" || tzid == "Etc/UTC" || tzid == "Z") {
        std::tm tm{};
        tm.tm_year = c.date.y - 1900;
        tm.tm_mon = c.date.m - 1;
        tm.tm_mday = c.date.d;
        tm.tm_hour = c.h;
        tm.tm_min = c.mi;
        tm.tm_sec = c.s;
        return timegm(&tm);
    }

    if (!tzid.empty()) {
        try {
            using namespace std::chrono;
            const std::string iana = windows_tz_to_iana(tzid);
            const time_zone* zone = locate_zone(iana);
            local_seconds lt = local_days{sys_days(year{c.date.y} / c.date.m / c.date.d).time_since_epoch()} +
                                hours{c.h} + minutes{c.mi} + seconds{c.s};
            // A spring-forward gap or an autumn ambiguity would otherwise
            // throw; picking the earliest reading matches what Outlook shows.
            auto st = zone->to_sys(lt, choose::earliest);
            return static_cast<std::time_t>(st.time_since_epoch().count());
        } catch (const std::exception&) {
            // fall through to local time
        }
    }

    std::tm tm{};
    tm.tm_year = c.date.y - 1900;
    tm.tm_mon = c.date.m - 1;
    tm.tm_mday = c.date.d;
    tm.tm_hour = c.h;
    tm.tm_min = c.mi;
    tm.tm_sec = c.s;
    tm.tm_isdst = -1;
    return std::mktime(&tm);
}

// -- property lines -------------------------------------------------------

struct Prop {
    std::string name;                             // upper-cased
    std::map<std::string, std::string> params;    // upper-cased keys
    std::string value;                            // raw, still escaped
};

Prop parse_prop(const std::string& line) {
    Prop p;
    size_t i = 0;
    while (i < line.size() && line[i] != ';' && line[i] != ':') ++i;
    p.name = upper(line.substr(0, i));

    while (i < line.size() && line[i] == ';') {
        ++i;
        size_t key_start = i;
        while (i < line.size() && line[i] != '=' && line[i] != ';' && line[i] != ':') ++i;
        std::string key = upper(line.substr(key_start, i - key_start));
        std::string val;
        if (i < line.size() && line[i] == '=') {
            ++i;
            bool quoted = false;
            std::string acc;
            // A quoted param value may contain ';' and ':' -- CN="Doe, J:" is
            // legal -- so the scan has to respect the quoting.
            while (i < line.size()) {
                char ch = line[i];
                if (ch == '"') {
                    quoted = !quoted;
                    ++i;
                    continue;
                }
                if (!quoted && (ch == ';' || ch == ':')) break;
                acc += ch;
                ++i;
            }
            val = acc;
        }
        p.params[key] = val;
    }
    if (i < line.size() && line[i] == ':') p.value = line.substr(i + 1);
    return p;
}

// -- date/time values -----------------------------------------------------

// Parses YYYYMMDD, YYYYMMDDTHHMMSS, or YYYYMMDDTHHMMSSZ.
bool parse_datetime(const std::string& raw, Civil& out, bool& is_utc, bool& is_date_only) {
    std::string v = trim(raw);
    is_utc = false;
    is_date_only = false;
    if (v.size() < 8) return false;
    for (int i = 0; i < 8; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(v[i]))) return false;
    }
    out.date.y = std::atoi(v.substr(0, 4).c_str());
    out.date.m = std::atoi(v.substr(4, 2).c_str());
    out.date.d = std::atoi(v.substr(6, 2).c_str());
    if (out.date.m < 1 || out.date.m > 12 || out.date.d < 1 || out.date.d > 31) return false;

    if (v.size() < 15 || v[8] != 'T') {
        is_date_only = true;
        return true;
    }
    out.h = std::atoi(v.substr(9, 2).c_str());
    out.mi = std::atoi(v.substr(11, 2).c_str());
    out.s = std::atoi(v.substr(13, 2).c_str());
    if (v.size() >= 16 && (v[15] == 'Z' || v[15] == 'z')) is_utc = true;
    return true;
}

// The zone a property's value should be read in: explicit TZID, "UTC" for a
// trailing Z, or "" meaning floating local time.
std::string zone_for(const Prop& p, bool is_utc) {
    if (is_utc) return "UTC";
    auto it = p.params.find("TZID");
    if (it != p.params.end() && !it->second.empty()) return it->second;
    return "";
}

// ISO 8601 duration, the subset Exchange emits for DURATION: P[n]DT[n]H[n]M[n]S
// (also P[n]W). Returns 0 if it doesn't parse.
long parse_duration(const std::string& raw) {
    std::string v = trim(raw);
    if (v.empty()) return 0;
    bool negative = false;
    size_t i = 0;
    if (v[i] == '-' || v[i] == '+') {
        negative = v[i] == '-';
        ++i;
    }
    if (i >= v.size() || v[i] != 'P') return 0;
    ++i;
    long total = 0;
    bool in_time = false;
    while (i < v.size()) {
        if (v[i] == 'T') {
            in_time = true;
            ++i;
            continue;
        }
        long n = 0;
        bool any = false;
        while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i]))) {
            n = n * 10 + (v[i] - '0');
            any = true;
            ++i;
        }
        if (!any || i >= v.size()) break;
        switch (v[i]) {
            case 'W': total += n * 7 * 86400; break;
            case 'D': total += n * 86400; break;
            case 'H': total += n * 3600; break;
            case 'M': total += in_time ? n * 60 : 0; break;
            case 'S': total += n; break;
            default: break;
        }
        ++i;
    }
    return negative ? -total : total;
}

// -- recurrence rules -----------------------------------------------------

struct ByDay {
    int weekday = 0;  // 0 = Sunday
    int nth = 0;      // 0 = every matching weekday, +n = nth, -n = nth from end
};

struct RRule {
    std::string freq;  // DAILY / WEEKLY / MONTHLY / YEARLY
    int interval = 1;
    int count = 0;              // 0 = unbounded
    std::time_t until = 0;      // 0 = unbounded
    std::vector<ByDay> by_day;
    std::vector<int> by_month_day;  // negative counts back from month end
    std::vector<int> by_month;
    int week_start = 1;  // 0 = Sunday; RFC default is Monday
};

int weekday_from_code(const std::string& code) {
    static const std::array<const char*, 7> kCodes = {"SU", "MO", "TU", "WE", "TH", "FR", "SA"};
    for (int i = 0; i < 7; ++i) {
        if (code == kCodes[i]) return i;
    }
    return -1;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

bool parse_rrule(const std::string& value, const std::string& tzid, RRule& out) {
    for (const std::string& part : split(value, ';')) {
        size_t eq = part.find('=');
        if (eq == std::string::npos) continue;
        std::string key = upper(trim(part.substr(0, eq)));
        std::string val = trim(part.substr(eq + 1));
        if (key == "FREQ") {
            out.freq = upper(val);
        } else if (key == "INTERVAL") {
            out.interval = std::max(1, std::atoi(val.c_str()));
        } else if (key == "COUNT") {
            out.count = std::atoi(val.c_str());
        } else if (key == "UNTIL") {
            Civil c;
            bool is_utc = false, date_only = false;
            if (parse_datetime(val, c, is_utc, date_only)) {
                if (date_only) {
                    c.h = 23;
                    c.mi = 59;
                    c.s = 59;
                }
                // An UNTIL without a Z is expressed in the rule's own zone.
                out.until = to_epoch(c, is_utc ? "UTC" : tzid);
            }
        } else if (key == "BYDAY") {
            for (const std::string& token_raw : split(upper(val), ',')) {
                std::string token = trim(token_raw);
                if (token.size() < 2) continue;
                size_t code_at = token.size() - 2;
                int wd = weekday_from_code(token.substr(code_at));
                if (wd < 0) continue;
                ByDay bd;
                bd.weekday = wd;
                if (code_at > 0) bd.nth = std::atoi(token.substr(0, code_at).c_str());
                out.by_day.push_back(bd);
            }
        } else if (key == "BYMONTHDAY") {
            for (const std::string& token : split(val, ',')) {
                int n = std::atoi(trim(token).c_str());
                if (n != 0) out.by_month_day.push_back(n);
            }
        } else if (key == "BYMONTH") {
            for (const std::string& token : split(val, ',')) {
                int n = std::atoi(trim(token).c_str());
                if (n >= 1 && n <= 12) out.by_month.push_back(n);
            }
        } else if (key == "WKST") {
            int wd = weekday_from_code(upper(val));
            if (wd >= 0) out.week_start = wd;
        }
    }
    return !out.freq.empty();
}

// Resolves a BYMONTHDAY entry (possibly negative) to a day of the month, or 0
// if the month is too short for it.
int resolve_month_day(int y, int m, int spec) {
    int last = days_in_month(y, m);
    int day = spec > 0 ? spec : last + spec + 1;
    return (day >= 1 && day <= last) ? day : 0;
}

// All days in a month matching a BYDAY entry, in ascending order.
void collect_by_day_in_month(int y, int m, const ByDay& bd, std::vector<Date>& out) {
    int last = days_in_month(y, m);
    std::vector<int> matches;
    for (int d = 1; d <= last; ++d) {
        if (weekday_of(Date{y, m, d}) == bd.weekday) matches.push_back(d);
    }
    if (matches.empty()) return;
    if (bd.nth == 0) {
        for (int d : matches) out.push_back(Date{y, m, d});
        return;
    }
    int index = bd.nth > 0 ? bd.nth - 1 : static_cast<int>(matches.size()) + bd.nth;
    if (index >= 0 && index < static_cast<int>(matches.size())) out.push_back(Date{y, m, matches[index]});
}

// Candidate start dates for one period of the rule, ascending.
std::vector<Date> candidates_for_period(const RRule& rule, const Date& dtstart, long long period) {
    std::vector<Date> out;

    if (rule.freq == "DAILY") {
        out.push_back(add_days(dtstart, period * rule.interval));
    } else if (rule.freq == "WEEKLY") {
        // Weeks are anchored on WKST so that INTERVAL=2 with several BYDAYs
        // keeps all of a week's meetings in the same week.
        int start_wd = weekday_of(dtstart);
        int offset_to_wkst = ((start_wd - rule.week_start) % 7 + 7) % 7;
        Date week_base = add_days(dtstart, -offset_to_wkst + period * rule.interval * 7);
        std::vector<int> weekdays;
        if (rule.by_day.empty()) {
            weekdays.push_back(start_wd);
        } else {
            for (const ByDay& bd : rule.by_day) weekdays.push_back(bd.weekday);
        }
        for (int wd : weekdays) {
            int offset = ((wd - rule.week_start) % 7 + 7) % 7;
            out.push_back(add_days(week_base, offset));
        }
    } else if (rule.freq == "MONTHLY") {
        long long month_index = static_cast<long long>(dtstart.y) * 12 + (dtstart.m - 1) + period * rule.interval;
        int y = static_cast<int>(month_index / 12);
        int m = static_cast<int>(month_index % 12) + 1;
        if (!rule.by_month_day.empty()) {
            for (int spec : rule.by_month_day) {
                int d = resolve_month_day(y, m, spec);
                if (d) out.push_back(Date{y, m, d});
            }
        } else if (!rule.by_day.empty()) {
            for (const ByDay& bd : rule.by_day) collect_by_day_in_month(y, m, bd, out);
        } else if (dtstart.d <= days_in_month(y, m)) {
            // A "31st of every month" series simply skips the short months,
            // which is what Outlook does too.
            out.push_back(Date{y, m, dtstart.d});
        }
    } else if (rule.freq == "YEARLY") {
        int y = dtstart.y + static_cast<int>(period * rule.interval);
        std::vector<int> months = rule.by_month.empty() ? std::vector<int>{dtstart.m} : rule.by_month;
        for (int m : months) {
            if (!rule.by_month_day.empty()) {
                for (int spec : rule.by_month_day) {
                    int d = resolve_month_day(y, m, spec);
                    if (d) out.push_back(Date{y, m, d});
                }
            } else if (!rule.by_day.empty()) {
                for (const ByDay& bd : rule.by_day) collect_by_day_in_month(y, m, bd, out);
            } else if (dtstart.d <= days_in_month(y, m)) {
                out.push_back(Date{y, m, dtstart.d});
            }
        }
    }

    // BYMONTH also acts as a plain filter on the sub-yearly frequencies.
    if (!rule.by_month.empty() && rule.freq != "YEARLY") {
        out.erase(std::remove_if(out.begin(), out.end(),
                                 [&rule](const Date& d) {
                                     return std::find(rule.by_month.begin(), rule.by_month.end(), d.m) ==
                                            rule.by_month.end();
                                 }),
                  out.end());
    }

    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// -- join links -----------------------------------------------------------

std::string extract_join_url(const std::string& text) {
    static const std::array<const char*, 5> kHosts = {"teams.microsoft.com/l/meetup-join", "teams.live.com/meet",
                                                       "zoom.us/j/", "meet.google.com/", "webex.com/"};
    size_t best = std::string::npos;
    for (const char* host : kHosts) {
        size_t at = text.find(host);
        if (at == std::string::npos) continue;
        // Walk back to the scheme so the link is actually clickable.
        size_t scheme = text.rfind("https://", at);
        if (scheme == std::string::npos) continue;
        if (best == std::string::npos || scheme < best) best = scheme;
    }
    if (best == std::string::npos) return "";
    size_t end = best;
    while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end])) && text[end] != '<' &&
            text[end] != '>' && text[end] != '"') {
        ++end;
    }
    std::string url = text.substr(best, end - best);
    // Invite bodies routinely wrap the link in angle brackets or trail it with
    // punctuation from the surrounding sentence.
    while (!url.empty() && (url.back() == ',' || url.back() == '.' || url.back() == ')')) url.pop_back();
    return url;
}

// -- one VEVENT before recurrence expansion -------------------------------

struct RawEvent {
    Event tmpl;
    std::string tzid;
    Civil start_civil;
    long duration = 0;  // seconds
    bool has_rrule = false;
    RRule rrule;
    std::vector<std::time_t> exdates;
    bool has_recurrence_id = false;
    std::time_t recurrence_id = 0;
    bool cancelled = false;
    bool valid = false;
};

RawEvent parse_vevent(const std::vector<std::string>& lines) {
    RawEvent ev;
    bool have_start = false;
    bool have_end = false;
    std::string rrule_value;
    std::time_t end_epoch = 0;
    long duration_prop = 0;

    for (const std::string& line : lines) {
        Prop p = parse_prop(line);
        if (p.name == "UID") {
            ev.tmpl.uid = unescape_text(p.value);
        } else if (p.name == "SUMMARY") {
            ev.tmpl.summary = unescape_text(p.value);
        } else if (p.name == "LOCATION") {
            ev.tmpl.location = unescape_text(p.value);
        } else if (p.name == "DESCRIPTION") {
            ev.tmpl.description = unescape_text(p.value);
        } else if (p.name == "ORGANIZER") {
            auto cn = p.params.find("CN");
            if (cn != p.params.end() && !cn->second.empty()) {
                ev.tmpl.organizer = cn->second;
            } else {
                std::string v = trim(p.value);
                if (upper(v).rfind("MAILTO:", 0) == 0) v = v.substr(7);
                ev.tmpl.organizer = v;
            }
        } else if (p.name == "ATTENDEE") {
            auto cn = p.params.find("CN");
            std::string who;
            if (cn != p.params.end() && !cn->second.empty()) {
                who = cn->second;
            } else {
                who = trim(p.value);
                if (upper(who).rfind("MAILTO:", 0) == 0) who = who.substr(7);
            }
            if (!who.empty()) ev.tmpl.attendees.push_back(who);
        } else if (p.name == "STATUS") {
            if (upper(trim(p.value)) == "CANCELLED") ev.cancelled = true;
        } else if (p.name == "TRANSP") {
            ev.tmpl.busy = upper(trim(p.value)) != "TRANSPARENT";
        } else if (p.name == "X-MICROSOFT-CDO-BUSYSTATUS") {
            ev.tmpl.out_of_office = upper(trim(p.value)) == "OOF";
        } else if (p.name == "DTSTART") {
            Civil c;
            bool is_utc = false, date_only = false;
            if (parse_datetime(p.value, c, is_utc, date_only)) {
                ev.start_civil = c;
                ev.tmpl.all_day = date_only;
                ev.tzid = zone_for(p, is_utc);
                have_start = true;
            }
        } else if (p.name == "DTEND") {
            Civil c;
            bool is_utc = false, date_only = false;
            if (parse_datetime(p.value, c, is_utc, date_only)) {
                end_epoch = to_epoch(c, zone_for(p, is_utc));
                have_end = true;
            }
        } else if (p.name == "DURATION") {
            duration_prop = parse_duration(p.value);
        } else if (p.name == "RRULE") {
            rrule_value = p.value;
        } else if (p.name == "EXDATE") {
            std::string zone = zone_for(p, false);
            for (const std::string& token : split(p.value, ',')) {
                Civil c;
                bool is_utc = false, date_only = false;
                if (!parse_datetime(token, c, is_utc, date_only)) continue;
                ev.exdates.push_back(to_epoch(c, is_utc ? "UTC" : zone));
            }
        } else if (p.name == "RECURRENCE-ID") {
            Civil c;
            bool is_utc = false, date_only = false;
            if (parse_datetime(p.value, c, is_utc, date_only)) {
                ev.recurrence_id = to_epoch(c, zone_for(p, is_utc));
                ev.has_recurrence_id = true;
            }
        }
    }

    if (!have_start) return ev;

    ev.tmpl.start = to_epoch(ev.start_civil, ev.tzid);
    if (have_end) {
        ev.duration = static_cast<long>(end_epoch - ev.tmpl.start);
    } else if (duration_prop > 0) {
        ev.duration = duration_prop;
    } else {
        ev.duration = ev.tmpl.all_day ? 86400 : 0;
    }
    if (ev.duration < 0) ev.duration = 0;
    ev.tmpl.end = ev.tmpl.start + ev.duration;

    if (!rrule_value.empty()) ev.has_rrule = parse_rrule(rrule_value, ev.tzid, ev.rrule);

    ev.tmpl.join_url = extract_join_url(ev.tmpl.location);
    if (ev.tmpl.join_url.empty()) ev.tmpl.join_url = extract_join_url(ev.tmpl.description);

    ev.valid = true;
    return ev;
}

// Safety valves: a malformed rule (FREQ=DAILY;INTERVAL=1 with no end, read
// against a wide window) must not spin forever.
constexpr int kMaxPeriods = 4000;
constexpr int kMaxOccurrences = 5000;

void expand_series(const RawEvent& ev, const std::map<std::time_t, const RawEvent*>& overrides,
                    std::time_t window_start, std::time_t window_end, std::vector<Event>& out) {
    const RRule& rule = ev.rrule;
    int emitted = 0;
    int occurrence_index = 0;

    for (long long period = 0; period < kMaxPeriods; ++period) {
        std::vector<Date> dates = candidates_for_period(rule, ev.start_civil.date, period);
        bool all_past_end = !dates.empty();

        for (const Date& date : dates) {
            Civil c = ev.start_civil;
            c.date = date;
            std::time_t start = to_epoch(c, ev.tzid);
            if (start < ev.tmpl.start) continue;  // periods can start before DTSTART
            if (rule.until != 0 && start > rule.until) return;
            if (rule.count != 0 && occurrence_index >= rule.count) return;
            ++occurrence_index;
            if (start < window_end) all_past_end = false;

            if (std::find(ev.exdates.begin(), ev.exdates.end(), start) != ev.exdates.end()) continue;
            // An occurrence the organizer moved or cancelled is emitted (or
            // dropped) by the override component instead.
            if (overrides.count(start)) continue;

            std::time_t end = start + ev.duration;
            if (end <= window_start || start >= window_end) continue;

            Event e = ev.tmpl;
            e.start = start;
            e.end = end;
            e.recurring = true;
            out.push_back(e);
            if (++emitted >= kMaxOccurrences) return;
        }

        // Every candidate in this period was already past the window and the
        // rule has no COUNT left to satisfy, so nothing later can qualify.
        if (all_past_end && rule.count == 0) return;
    }
}

}  // namespace

// -- public API -----------------------------------------------------------

std::string unfold(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '\r') continue;
        if (c == '\n') {
            // A line break followed by whitespace is a fold, not a new line.
            if (i + 1 < text.size() && (text[i + 1] == ' ' || text[i + 1] == '\t')) {
                ++i;
                continue;
            }
            out += '\n';
            continue;
        }
        out += c;
    }
    return out;
}

std::string unescape_text(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '\\' || i + 1 >= value.size()) {
            out += value[i];
            continue;
        }
        char next = value[++i];
        switch (next) {
            case 'n':
            case 'N': out += '\n'; break;
            case ',': out += ','; break;
            case ';': out += ';'; break;
            case '\\': out += '\\'; break;
            default:
                out += '\\';
                out += next;
                break;
        }
    }
    return out;
}

std::string windows_tz_to_iana(const std::string& tzid) {
    if (tzid.find('/') != std::string::npos) return tzid;  // already IANA
    const auto& table = windows_tz_table();
    auto it = table.find(tzid);
    if (it != table.end()) return it->second;
    // Exchange sometimes wraps the name in parentheses with a UTC offset,
    // e.g. "(UTC+01:00) W. Europe Standard Time".
    size_t close = tzid.find(") ");
    if (close != std::string::npos) {
        auto it2 = table.find(tzid.substr(close + 2));
        if (it2 != table.end()) return it2->second;
    }
    return tzid;
}

std::vector<Event> parse_ics(const std::string& text, std::time_t window_start, std::time_t window_end) {
    std::vector<Event> out;
    if (window_end <= window_start) return out;

    std::istringstream stream(unfold(text));
    std::string line;
    std::vector<std::string> current;
    bool in_event = false;
    std::vector<RawEvent> raw;

    while (std::getline(stream, line)) {
        std::string trimmed = trim(line);
        if (trimmed.empty()) continue;
        std::string upper_line = upper(trimmed);
        if (upper_line == "BEGIN:VEVENT") {
            in_event = true;
            current.clear();
            continue;
        }
        if (upper_line == "END:VEVENT") {
            if (in_event) {
                RawEvent ev = parse_vevent(current);
                if (ev.valid) raw.push_back(std::move(ev));
            }
            in_event = false;
            current.clear();
            continue;
        }
        // Everything outside a VEVENT (VTIMEZONE, VALARM, calendar headers) is
        // not needed: zones are resolved from the tzdb by name instead.
        if (in_event) current.push_back(trimmed);
    }

    // Modified/cancelled single occurrences carry the series UID plus a
    // RECURRENCE-ID naming the instance they replace.
    std::map<std::string, std::map<std::time_t, const RawEvent*>> overrides;
    for (const RawEvent& ev : raw) {
        if (ev.has_recurrence_id) overrides[ev.tmpl.uid][ev.recurrence_id] = &ev;
    }
    static const std::map<std::time_t, const RawEvent*> kNoOverrides;

    for (const RawEvent& ev : raw) {
        if (ev.cancelled) continue;
        if (ev.has_recurrence_id) {
            // The replacement instance itself -- a plain event at its new time.
            if (ev.tmpl.end > window_start && ev.tmpl.start < window_end) {
                Event e = ev.tmpl;
                e.recurring = true;
                out.push_back(e);
            }
            continue;
        }
        if (ev.has_rrule) {
            auto it = overrides.find(ev.tmpl.uid);
            expand_series(ev, it == overrides.end() ? kNoOverrides : it->second, window_start, window_end, out);
            continue;
        }
        if (ev.tmpl.end > window_start && ev.tmpl.start < window_end) out.push_back(ev.tmpl);
    }

    std::sort(out.begin(), out.end(), [](const Event& a, const Event& b) {
        if (a.start != b.start) return a.start < b.start;
        return a.summary < b.summary;
    });
    return out;
}

}  // namespace callscribe::calendar
