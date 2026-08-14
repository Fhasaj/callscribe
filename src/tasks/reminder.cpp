#include "tasks/reminder.hpp"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>

#include "util/time.hpp"

namespace callscribe::tasks {

void send_notification(const std::string& title, const std::string& body) {
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execlp("notify-send", "notify-send", "--app-name=CallScribe", "--", title.c_str(), body.c_str(),
               static_cast<char*>(nullptr));
        _exit(127);  // notify-send not installed -- in-app callback still covers it
    }
    int status = 0;
    waitpid(pid, &status, 0);
}

ReminderScheduler::ReminderScheduler(db::Repository& repo, std::string reminder_time, bool enabled)
    : repo_(repo), reminder_time_(std::move(reminder_time)), enabled_(enabled) {}

ReminderScheduler::~ReminderScheduler() { stop(); }

void ReminderScheduler::start() {
    if (running_) return;
    running_ = true;
    thread_ = std::thread(&ReminderScheduler::loop, this);
}

void ReminderScheduler::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void ReminderScheduler::set_reminder_time(const std::string& time) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    reminder_time_ = time;
}

void ReminderScheduler::set_enabled(bool enabled) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    enabled_ = enabled;
}

void ReminderScheduler::set_on_fired(Callback cb) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    on_fired_ = std::move(cb);
}

void ReminderScheduler::loop() {
    int elapsed_seconds = 0;
    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!running_) break;
        if (++elapsed_seconds < 60) continue;
        elapsed_seconds = 0;

        bool enabled;
        std::string reminder_time;
        {
            std::lock_guard<std::mutex> lock(config_mutex_);
            enabled = enabled_;
            reminder_time = reminder_time_;
        }
        if (!enabled) continue;

        std::string today = util::today_iso();
        if (today == last_fired_date_) continue;
        if (util::now_time_hhmm() < reminder_time) continue;

        last_fired_date_ = today;
        fire_now();
    }
}

std::vector<db::Task> ReminderScheduler::fire_now() {
    auto due = repo_.due_tasks(util::today_iso());
    if (!due.empty()) {
        std::string lines;
        size_t limit = due.size() < 10 ? due.size() : 10;
        for (size_t i = 0; i < limit; ++i) {
            if (!lines.empty()) lines += "\n";
            lines += "- " + due[i].description;
            // Flag the urgent ones so the notification can be triaged without
            // opening the app.
            if (due[i].priority == db::kPriorityHigh) lines += " [HIGH]";
            if (!due[i].customer_name.empty()) lines += " (" + due[i].customer_name + ")";
        }
        if (due.size() > 10) lines += "\n(+" + std::to_string(due.size() - 10) + " more)";
        send_notification(std::to_string(due.size()) + " task(s) due", lines);
    }
    Callback cb;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        cb = on_fired_;
    }
    if (cb) cb(due);
    return due;
}

}  // namespace callscribe::tasks
