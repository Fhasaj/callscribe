#pragma once
#include <ctime>
#include <string>

namespace callscribe::util {

inline std::string now_iso() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    return buf;
}

inline std::string today_iso() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

// today_iso() shifted by `days` (negative goes backward). Goes through
// mktime() so month/year rollovers (e.g. "tomorrow" on the last day of the
// month) are normalized automatically.
inline std::string offset_days_iso(int days) {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    tm.tm_mday += days;
    std::mktime(&tm);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

inline std::string now_time_hhmm() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[8];
    std::strftime(buf, sizeof(buf), "%H:%M", &tm);
    return buf;
}

inline int current_year() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    return tm.tm_year + 1900;
}

} // namespace callscribe::util
