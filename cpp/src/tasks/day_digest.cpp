#include "tasks/day_digest.hpp"

namespace callscribe::tasks {

namespace {

std::string format_hm(int seconds) {
    const int minutes = (seconds + 30) / 60;
    if (minutes < 60) return std::to_string(minutes) + "m";
    return std::to_string(minutes / 60) + "h" + (minutes % 60 ? " " + std::to_string(minutes % 60) + "m" : "");
}

// "2026-10-19T14:05:33" -> "14:05"
std::string clock(const std::string& iso) { return iso.size() >= 16 ? iso.substr(11, 5) : ""; }

}  // namespace

std::string build_day_digest(db::Repository& repo, const std::string& day) {
    // The next day, as a string bound: "2026-10-19" < anything on the 19th < "2026-10-19~".
    const std::string to = day + "~";
    std::string out;

    auto calls = repo.list_calls_between(day, to);
    out += "Calls: " + std::to_string(calls.size()) + "\n";
    for (const auto& c : calls) {
        std::string who = c.customer_name;
        if (!c.meeting_name.empty()) who += (who.empty() ? "" : " -- ") + c.meeting_name;
        out += "  " + clock(c.started_at) + "  " + (who.empty() ? "(untitled call)" : who);
        if (c.duration_seconds > 0) out += " (" + format_hm(c.duration_seconds) + ")";
        out += "\n";
    }

    auto done = repo.list_completed_tasks(day, to);
    out += "Tasks done: " + std::to_string(done.size()) + "\n";
    for (const auto& t : done) {
        out += "  " + t.description;
        if (!t.customer_name.empty()) out += " (" + t.customer_name + ")";
        out += "\n";
    }

    auto focus = repo.focus_seconds_by_tag(day, to);
    out += "Focus:";
    if (focus.empty()) out += " none";
    for (size_t i = 0; i < focus.size(); ++i) out += (i ? ", " : " ") + focus[i].first + " " + format_hm(focus[i].second);
    out += "\n";

    auto wins = repo.list_wins(day, to);
    out += "Wins: " + std::to_string(wins.size()) + "\n";
    for (auto it = wins.rbegin(); it != wins.rend(); ++it)
        out += "  " + (it->tag.empty() ? "" : "[" + it->tag + "] ") + it->text + "\n";

    const size_t parked = repo.list_parked().size();
    if (parked > 0) out += "Still parked: " + std::to_string(parked) + "\n";
    return out;
}

}  // namespace callscribe::tasks
