#include "tasks/meeting_reminder.hpp"

#include <cctype>
#include <chrono>

#include "calendar/calendar_source.hpp"
#include "tasks/reminder.hpp"

namespace callscribe::tasks {

namespace {
// How far ahead the cache is expanded at a time. Re-expanded well before it
// runs out, and whenever the cache changes.
constexpr std::time_t kHorizon = 6 * 3600;

std::string clock(std::time_t t) {
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[8];
    std::strftime(buf, sizeof(buf), "%H:%M", &tm);
    return buf;
}
}  // namespace

MeetingReminder::MeetingReminder() = default;

bool MeetingReminder::is_out_of_office(const calendar::Event& event) {
    if (event.out_of_office) return true;
    std::string title;
    for (char c : event.summary) title += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const char* marker : {"out of office", "out-of-office", "ooo", "oof", "annual leave", "holiday",
                               "vacation", "on leave", "pto", "day off", "sick"}) {
        const std::string m = marker;
        const size_t at = title.find(m);
        if (at == std::string::npos) continue;
        // Whole words only, so "Proof review" or "Woof" don't count.
        const bool start_ok = at == 0 || !std::isalnum(static_cast<unsigned char>(title[at - 1]));
        const size_t end = at + m.size();
        const bool end_ok = end == title.size() || !std::isalnum(static_cast<unsigned char>(title[end]));
        if (start_ok && end_ok) return true;
    }
    return false;
}

MeetingReminder::~MeetingReminder() { stop(); }

void MeetingReminder::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread([this] { loop(); });
}

void MeetingReminder::stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
}

std::string MeetingReminder::key(const calendar::Event& event) {
    return event.uid + "@" + std::to_string(event.start);
}

std::vector<calendar::Event> MeetingReminder::due(const std::vector<calendar::Event>& events, std::time_t now,
                                                  int lead_minutes, const std::set<std::string>& already) {
    std::vector<calendar::Event> out;
    if (lead_minutes <= 0) return out;
    const std::time_t lead = static_cast<std::time_t>(lead_minutes) * 60;
    for (const auto& e : events) {
        if (e.all_day || !e.busy || is_out_of_office(e)) continue;
        // Inside the warning window, and not already under way -- opening the
        // app five minutes into a meeting shouldn't announce it.
        if (e.start - now > lead || e.start <= now) continue;
        if (already.count(key(e))) continue;
        out.push_back(e);
    }
    return out;
}

std::pair<std::string, std::string> MeetingReminder::message(const calendar::Event& event, std::time_t now) {
    const long minutes = static_cast<long>((event.start - now + 59) / 60);
    std::string title = (minutes <= 1 ? "Starting now: " : "In " + std::to_string(minutes) + " min: ") +
                        (event.summary.empty() ? std::string("(untitled meeting)") : event.summary);
    std::string body = clock(event.start) + "–" + clock(event.end);
    if (!event.location.empty()) body += " · " + event.location;
    if (!event.join_url.empty()) body += "\nJoin: " + event.join_url;
    body += "\nRecord it from CallScribe's Calendar tab.";
    return {title, body};
}

void MeetingReminder::reload_if_needed(std::time_t now) {
    const std::time_t mtime = calendar::cache_mtime();
    if (mtime == cache_seen_ && now + kHorizon / 2 < parsed_until_) return;
    cache_seen_ = mtime;
    parsed_until_ = now + kHorizon;
    const std::string ics = calendar::read_cache();
    upcoming_ = ics.empty() ? std::vector<calendar::Event>{} : calendar::parse_ics(ics, now, parsed_until_);
}

void MeetingReminder::loop() {
    int tick = 0;
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        // Every 15 s is plenty for a 15-minute warning, and keeps the parse
        // (only when the cache changed) off the hot path.
        if (++tick < 15) continue;
        tick = 0;

        const int lead = lead_minutes_.load();
        if (lead <= 0) continue;
        const std::time_t now = std::time(nullptr);
        reload_if_needed(now);
        for (const auto& e : due(upcoming_, now, lead, notified_)) {
            notified_.insert(key(e));
            auto [title, body] = message(e, now);
            send_notification(title, body);
        }
    }
}

}  // namespace callscribe::tasks
