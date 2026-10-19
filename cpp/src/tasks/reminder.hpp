#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "db/repository.hpp"

namespace callscribe::tasks {

// Fires a desktop notification via `notify-send`, spawned with execlp (no
// shell involved) so task text containing shell metacharacters can't be
// interpreted -- and passes "--" before the positional args since this
// notify-send build otherwise misparses body text starting with '-'
// (bullet lists) as an option flag.
void send_notification(const std::string& title, const std::string& body);

// Background thread: checks once a minute, fires a notification once per
// day at the configured time, and invokes the on-fired callback with the
// due task list so the Tasks screen can refresh.
class ReminderScheduler {
public:
    ReminderScheduler(db::Repository& repo, std::string reminder_time, bool enabled);
    ~ReminderScheduler();
    ReminderScheduler(const ReminderScheduler&) = delete;
    ReminderScheduler& operator=(const ReminderScheduler&) = delete;

    void start();
    void stop();

    void set_reminder_time(const std::string& time);
    void set_enabled(bool enabled);
    // The end-of-day wrap-up nudge: once a day at `time`, unless today's log
    // has already been saved.
    void set_end_of_day(const std::string& time, bool enabled);

    std::vector<db::Task> fire_now();

    using Callback = std::function<void(const std::vector<db::Task>&)>;
    void set_on_fired(Callback cb);

private:
    db::Repository& repo_;

    std::mutex config_mutex_;
    std::string reminder_time_;
    bool enabled_;
    std::string last_fired_date_;
    std::string end_of_day_time_ = "17:30";
    bool end_of_day_enabled_ = false;
    std::string last_end_of_day_date_;

    std::mutex callback_mutex_;
    Callback on_fired_;

    std::atomic<bool> running_{false};
    std::thread thread_;

    void loop();
};

}  // namespace callscribe::tasks
