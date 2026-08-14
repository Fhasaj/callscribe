#pragma once
#include <ftxui/component/component.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "context.hpp"
#include "util/virtual_pointer.hpp"

namespace callscribe::ui {

// Keeps the desktop session looking active: on a timer, nudges the pointer
// around a small square (ending exactly where it started, so nothing drifts)
// through a uinput virtual mouse. Real input events are what reset the
// compositor's idle timer, and the idle timer is what Teams/Slack read to flip
// your presence to Away.
class MouseMoverScreen {
public:
    explicit MouseMoverScreen(AppContext& ctx);
    ~MouseMoverScreen();
    MouseMoverScreen(const MouseMoverScreen&) = delete;
    MouseMoverScreen& operator=(const MouseMoverScreen&) = delete;

    ftxui::Component component() { return root_; }
    bool is_running() const { return running_.load(); }

private:
    AppContext& ctx_;

    // Form state (strings so the user can type freely; validated on Start).
    std::string interval_text_;
    std::string distance_text_;
    bool autostart_ = false;

    std::atomic<bool> running_{false};
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;  // wakes the worker's sleep so Stop is instant

    // Guarded by mutex_.
    std::string status_;
    int move_count_ = 0;
    std::chrono::steady_clock::time_point next_move_at_;
    bool have_next_move_ = false;

    util::VirtualPointer pointer_;

    void start();
    void stop();
    void run_loop(int interval_seconds, int distance);
    void set_status(std::string s);
    void build_ui();

    ftxui::Component root_;
};

}  // namespace callscribe::ui
