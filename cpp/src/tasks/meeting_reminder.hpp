#pragma once
#include <atomic>
#include <ctime>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "calendar/ics_parser.hpp"

namespace callscribe::tasks {

// A desktop notification a few minutes before each meeting on the imported
// calendar. Reads the calendar cache the Calendar tab keeps fresh (see
// calendar::load_calendar), so it works whether or not that tab was ever
// opened, and without fetching anything itself.
class MeetingReminder {
public:
    MeetingReminder();
    ~MeetingReminder();
    MeetingReminder(const MeetingReminder&) = delete;
    MeetingReminder& operator=(const MeetingReminder&) = delete;

    void start();
    void stop();
    // Minutes of warning; 0 turns the reminders off.
    void set_lead_minutes(int minutes) { lead_minutes_.store(minutes); }

    // The meetings due a reminder at `now`, given `lead_minutes` of warning:
    // starting within that window and not over yet, skipping all-day and
    // "show as free" entries and anything in `already`. Exposed for tests.
    static std::vector<calendar::Event> due(const std::vector<calendar::Event>& events, std::time_t now,
                                            int lead_minutes, const std::set<std::string>& already);
    // What a reminder says: title, then body.
    static std::pair<std::string, std::string> message(const calendar::Event& event, std::time_t now);
    static std::string key(const calendar::Event& event);
    // Out-of-office blocks aren't meetings: flagged OOF by Outlook, or titled
    // like one ("Out of office", "OOO", "Annual leave", "Holiday"...).
    static bool is_out_of_office(const calendar::Event& event);

private:
    std::atomic<int> lead_minutes_{15};
    std::atomic<bool> running_{false};
    std::thread thread_;

    // Worker thread only.
    std::time_t cache_seen_ = -1;
    std::vector<calendar::Event> upcoming_;
    std::time_t parsed_until_ = 0;
    std::set<std::string> notified_;

    void loop();
    void reload_if_needed(std::time_t now);
};

}  // namespace callscribe::tasks
