#include "util/date_format.hpp"

#include <cstdio>

namespace callscribe::util {

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

}  // namespace callscribe::util
