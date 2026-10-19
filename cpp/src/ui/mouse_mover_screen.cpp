#include "ui/mouse_mover_screen.hpp"

#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <random>
#include <utility>

#include "ui/styled_input.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"
#include "ui/wrapped_input.hpp"

namespace callscribe::ui {

namespace {

// Tolerant of stray spaces and of the field being cleared mid-edit; anything
// unparseable falls back to `fallback` rather than to zero.
int parse_int(const std::string& text, int fallback) {
    try {
        size_t consumed = 0;
        int value = std::stoi(text, &consumed);
        return consumed == 0 ? fallback : value;
    } catch (const std::exception&) {
        return fallback;
    }
}

constexpr int kMinInterval = 5;
constexpr int kMaxInterval = 3600;
constexpr int kMinDistance = 1;
constexpr int kMaxDistance = 200;

}  // namespace

MouseMoverScreen::MouseMoverScreen(AppContext& ctx) : ctx_(ctx) {
    interval_text_ = std::to_string(ctx_.settings.mouse_mover_interval_seconds);
    distance_text_ = std::to_string(ctx_.settings.mouse_mover_pixels);
    autostart_ = ctx_.settings.mouse_mover_autostart;
    status_ = "Idle.";
    build_ui();
    if (autostart_) start();
}

MouseMoverScreen::~MouseMoverScreen() { stop(); }

void MouseMoverScreen::set_status(std::string s) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = std::move(s);
}

void MouseMoverScreen::start() {
    if (running_.load()) return;
    // A previous run that ended by itself (e.g. /dev/uinput was unavailable)
    // leaves a finished-but-unjoined thread behind.
    if (worker_.joinable()) worker_.join();

    const int interval = std::clamp(parse_int(interval_text_, ctx_.settings.mouse_mover_interval_seconds),
                                    kMinInterval, kMaxInterval);
    const int distance =
        std::clamp(parse_int(distance_text_, ctx_.settings.mouse_mover_pixels), kMinDistance, kMaxDistance);
    // Echo the clamped values back into the fields so what's shown is what runs.
    interval_text_ = std::to_string(interval);
    distance_text_ = std::to_string(distance);

    ctx_.settings.mouse_mover_interval_seconds = interval;
    ctx_.settings.mouse_mover_pixels = distance;
    ctx_.settings.mouse_mover_autostart = autostart_;
    ctx_.save_settings();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        move_count_ = 0;
        have_next_move_ = false;
        status_ = "Creating the virtual pointer...";
    }

    running_.store(true);
    worker_ = std::thread([this, interval, distance] { run_loop(interval, distance); });
}

void MouseMoverScreen::stop() {
    const bool was_running = running_.exchange(false);
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    if (!was_running) return;
    std::lock_guard<std::mutex> lock(mutex_);
    have_next_move_ = false;
    status_ = "Stopped.";
}

void MouseMoverScreen::run_loop(int interval_seconds, int distance) {
    std::string error;
    if (!pointer_.open(error)) {
        set_status(error);
        running_.store(false);
        return;
    }
    set_status("Active -- nudging the pointer about every " + std::to_string(interval_seconds) + "s.");

    std::mt19937 rng{std::random_device{}()};
    std::uniform_real_distribution<double> jitter(0.85, 1.15);

    while (running_.load()) {
        const auto wait = std::chrono::milliseconds(
            static_cast<long long>(interval_seconds * 1000.0 * jitter(rng)));
        {
            std::unique_lock<std::mutex> lock(mutex_);
            next_move_at_ = std::chrono::steady_clock::now() + wait;
            have_next_move_ = true;
            cv_.wait_for(lock, wait, [this] { return !running_.load(); });
        }
        if (!running_.load()) break;

        // A small square -- right, down, left, up -- so the cursor ends exactly
        // where it started. Each leg is its own event batch with a gap between
        // them, so libinput sees motion spread over time rather than one jump.
        const std::pair<int, int> legs[] = {{distance, 0}, {0, distance}, {-distance, 0}, {0, -distance}};
        bool ok = true;
        for (const auto& [dx, dy] : legs) {
            ok = pointer_.move(dx, dy) && ok;
            std::this_thread::sleep_for(std::chrono::milliseconds(35));
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (ok) {
            ++move_count_;
        } else {
            status_ = "The virtual pointer stopped accepting events -- Stop and Start again.";
        }
    }

    pointer_.close();
}

void MouseMoverScreen::build_ui() {
    using namespace ftxui;

    InputOption interval_opt = styled_input_option();
    interval_opt.multiline = false;
    auto interval_input = WrappedInput(&interval_text_, "seconds", interval_opt);

    InputOption distance_opt = styled_input_option();
    distance_opt.multiline = false;
    auto distance_input = WrappedInput(&distance_text_, "pixels", distance_opt);

    auto autostart_checkbox = Checkbox("Start automatically when CallScribe launches", &autostart_);

    auto toggle_button = Button(
        "Start / Stop",
        [this] {
            if (running_.load())
                stop();
            else
                start();
        },
        [this] {
            ButtonOption opt;
            opt.transform = [this](const EntryState& s) {
                EntryState labelled = s;
                labelled.label = running_.load() ? "■ Stop" : "▶ Start";
                return button_style(running_.load() ? theme::kDanger : theme::kSuccess).transform(labelled);
            };
            return opt;
        }());

    auto nudge_button = Button(
        "Nudge now",
        [this] {
            // A one-off move that works whether or not the timer is running --
            // the quickest way to confirm /dev/uinput is actually wired up.
            std::string error;
            if (!pointer_.is_open() && !pointer_.open(error)) {
                set_status(error);
                return;
            }
            const int distance =
                std::clamp(parse_int(distance_text_, ctx_.settings.mouse_mover_pixels), kMinDistance, kMaxDistance);
            const bool ok = pointer_.move(distance, 0) && pointer_.move(-distance, 0);
            set_status(ok ? "Nudged." : "The virtual pointer rejected the move.");
            if (!running_.load()) pointer_.close();
        },
        button_style(theme::kMuted));

    auto layout = Container::Vertical({
        Container::Horizontal({interval_input, distance_input}),
        autostart_checkbox,
        Container::Horizontal({toggle_button, nudge_button}),
    });

    root_ = Renderer(layout, [this, interval_input, distance_input, autostart_checkbox, toggle_button, nudge_button] {
        std::string status_copy;
        int moves = 0;
        long long seconds_left = -1;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            status_copy = status_;
            moves = move_count_;
            if (have_next_move_) {
                const auto remaining = next_move_at_ - std::chrono::steady_clock::now();
                seconds_left =
                    std::max<long long>(0, std::chrono::duration_cast<std::chrono::seconds>(remaining).count());
            }
        }
        const bool active = running_.load();

        Elements children;
        children.push_back(paragraph("Moves the pointer a few pixels on a timer so the session never goes idle "
                                     "and Teams keeps showing you as Available. Wayland has no pointer-warp API "
                                     "for apps, so this registers a virtual mouse with the kernel (/dev/uinput) "
                                     "and sends real input events through it.") |
                           dim);
        children.push_back(text(""));
        children.push_back(hbox({
            field_row("Move every (s)", interval_input->Render() | size(WIDTH, EQUAL, 12), 15),
            text("    "),
            field_row("Distance (px)", distance_input->Render() | size(WIDTH, EQUAL, 12), 15),
            filler(),
        }));
        children.push_back(autostart_checkbox->Render());
        children.push_back(text(""));
        children.push_back(hbox({
            toggle_button->Render(),
            text(" "),
            nudge_button->Render(),
            text("   "),
            (active ? badge("Active", theme::kSuccess) : badge("Inactive", theme::kMuted)) | vcenter,
            filler(),
        }));
        Elements status = {paragraph(status_copy) | color(active ? theme::kSuccess : theme::kMuted)};
        if (active) {
            std::string line = "Nudges so far: " + std::to_string(moves);
            if (seconds_left >= 0) line += "   next in ~" + std::to_string(seconds_left) + "s";
            status.push_back(text(line) | color(theme::kWarning));
        }
        children.push_back(vbox(std::move(status)));
        Element page = section("Mouse Mover", vbox(std::move(children)));
        return vbox({page, filler()}) | flex;
    });
}

}  // namespace callscribe::ui
