#include "ui/calendar_screen.hpp"

#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "calendar/call_match.hpp"
#include "ui/scroll_pane.hpp"
#include "ui/styled_input.hpp"
#include "ui/text_wrap.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"
#include "ui/wrapped_input.hpp"

namespace callscribe::ui {

namespace {

std::tm local_tm(std::time_t t) {
    std::tm tm{};
    localtime_r(&t, &tm);
    return tm;
}

std::time_t start_of_day(std::time_t t) {
    std::tm tm = local_tm(t);
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    return std::mktime(&tm);
}

std::time_t add_days(std::time_t t, int days) {
    std::tm tm = local_tm(t);
    tm.tm_mday += days;
    tm.tm_isdst = -1;
    return std::mktime(&tm);
}

std::string hhmm(std::time_t t) {
    std::tm tm = local_tm(t);
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    return buf;
}

// "Tue 04 08 2026" -- day/month/year ordering to match the rest of the app.
std::string day_heading(std::time_t t) {
    static const char* kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    std::tm tm = local_tm(t);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s %02d %02d %04d", kDays[tm.tm_wday], tm.tm_mday, tm.tm_mon + 1,
                   tm.tm_year + 1900);
    return buf;
}

std::string relative_age(std::time_t then) {
    if (then == 0) return "never";
    long mins = static_cast<long>(std::difftime(std::time(nullptr), then) / 60);
    if (mins < 1) return "just now";
    if (mins < 60) return std::to_string(mins) + " min ago";
    long hours = mins / 60;
    if (hours < 24) return std::to_string(hours) + "h ago";
    return std::to_string(hours / 24) + "d ago";
}

std::string truncate(const std::string& s, size_t max_len) {
    if (s.size() <= max_len) return s;
    return s.substr(0, max_len - 1) + "…";
}

// 0 = Monday .. 6 = Sunday. The grids are Monday-first, matching Outlook's
// default for a European work week.
int weekday_mon0(std::time_t t) {
    std::tm tm = local_tm(t);
    return (tm.tm_wday + 6) % 7;
}

std::time_t start_of_week(std::time_t t) { return add_days(start_of_day(t), -weekday_mon0(t)); }

std::time_t start_of_month(std::time_t t) {
    std::tm tm = local_tm(t);
    tm.tm_mday = 1;
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    tm.tm_isdst = -1;
    return std::mktime(&tm);
}

std::time_t add_months(std::time_t t, int months) {
    std::tm tm = local_tm(t);
    tm.tm_mon += months;
    tm.tm_isdst = -1;
    return std::mktime(&tm);
}

int day_of_month(std::time_t t) { return local_tm(t).tm_mday; }

bool same_day(std::time_t a, std::time_t b) { return start_of_day(a) == start_of_day(b); }

const char* kWeekdayNames[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};

std::string month_title(std::time_t t) {
    static const char* kMonths[] = {"January", "February", "March",     "April",   "May",      "June",
                                     "July",    "August",   "September", "October", "November", "December"};
    std::tm tm = local_tm(t);
    return std::string(kMonths[tm.tm_mon]) + " " + std::to_string(tm.tm_year + 1900);
}

}  // namespace

CalendarScreen::CalendarScreen(AppContext& ctx) : ctx_(ctx) {
    ics_url_ = ctx_.settings.calendar_ics_url;
    ics_path_ = ctx_.settings.calendar_ics_path;
    refresh_minutes_ = std::to_string(ctx_.settings.calendar_refresh_minutes);
    active_kind_ = calendar::source_kind_from_string(ctx_.settings.calendar_source);
    active_url_ = ctx_.settings.calendar_ics_url;
    active_path_ = ctx_.settings.calendar_ics_path;
    active_refresh_minutes_ = std::max(5, ctx_.settings.calendar_refresh_minutes);
    switch (active_kind_) {
        case calendar::SourceKind::Url: source_selected_ = 1; break;
        case calendar::SourceKind::File: source_selected_ = 2; break;
        case calendar::SourceKind::Off: source_selected_ = 0; break;
    }
    // Nothing configured yet -- open the setup box rather than showing an
    // empty agenda with no hint of what to do.
    setup_expanded_ = source_selected_ == 0;

    refresh_recorded_calls();
    build_ui();
    start_worker();
    if (source_selected_ != 0) request_refresh();
}

CalendarScreen::~CalendarScreen() {
    stop_ = true;
    if (worker_.joinable()) worker_.join();
}

void CalendarScreen::start_worker() {
    worker_ = std::thread([this] {
        auto last_refresh = std::chrono::steady_clock::now();
        bool ever_loaded = false;
        while (!stop_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (stop_) break;

            // A range switch only needs the cached feed re-expanded, so it is
            // handled without touching the network.
            if (reparse_requested_.exchange(false) && !refresh_requested_.load()) {
                load_now(false);
                continue;
            }

            bool configured;
            int interval_minutes;
            {
                std::lock_guard<std::mutex> lock(config_mutex_);
                configured = active_kind_ != calendar::SourceKind::Off;
                interval_minutes = active_refresh_minutes_;
            }

            bool due = false;
            if (refresh_requested_.exchange(false)) {
                due = true;
            } else if (ever_loaded && configured &&
                        std::chrono::steady_clock::now() - last_refresh >= std::chrono::minutes(interval_minutes)) {
                due = true;
            }
            if (!due) continue;

            loading_ = true;
            load_now(true);
            loading_ = false;
            last_refresh = std::chrono::steady_clock::now();
            ever_loaded = true;
        }
    });
}

void CalendarScreen::request_refresh() { refresh_requested_ = true; }

void CalendarScreen::window_for_range(std::time_t& start, std::time_t& end) const {
    std::time_t now = std::time(nullptr);
    int range = active_range_.load();
    start = start_of_day(now);
    if (range == 0) {
        end = add_days(start, 1);
    } else if (range == 2) {
        start = start_of_month(now);
        end = add_months(start, 1);
        // The month grid draws whole weeks, so it needs the tail of the
        // previous month and the head of the next one to fill its edge cells.
        if (active_view_.load() == 0) {
            start = start_of_week(start);
            end = add_days(start_of_week(add_days(end, -1)), 7);
        }
    } else {
        end = add_days(start, 7);
    }
}

void CalendarScreen::load_now(bool fetch) {
    std::time_t window_start = 0, window_end = 0;
    window_for_range(window_start, window_end);

    calendar::SourceKind kind;
    std::string url, path;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        kind = active_kind_;
        url = active_url_;
        path = active_path_;
    }

    if (!fetch) {
        std::string cached = calendar::read_cache();
        if (cached.empty()) return;
        auto events = calendar::parse_ics(cached, window_start, window_end);
        std::lock_guard<std::mutex> lock(events_mutex_);
        events_ = std::move(events);
        status_ = std::to_string(events_.size()) + " event(s)";
        return;
    }

    calendar::LoadResult result = calendar::load_calendar(kind, url, path, window_start, window_end);

    std::lock_guard<std::mutex> lock(events_mutex_);
    events_ = std::move(result.events);
    error_ = result.error;
    from_cache_ = result.from_cache;
    fetched_at_ = result.fetched_at;
    status_ = std::to_string(events_.size()) + " event(s)";
}

void CalendarScreen::save_source() {
    ctx_.settings.calendar_source =
        source_selected_ == 1 ? "url" : (source_selected_ == 2 ? "file" : "off");
    ctx_.settings.calendar_ics_url = ics_url_;
    ctx_.settings.calendar_ics_path = ics_path_;
    int minutes = std::atoi(refresh_minutes_.c_str());
    ctx_.settings.calendar_refresh_minutes = minutes >= 5 ? minutes : 30;
    refresh_minutes_ = std::to_string(ctx_.settings.calendar_refresh_minutes);
    ctx_.save_settings();

    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        active_kind_ = calendar::source_kind_from_string(ctx_.settings.calendar_source);
        active_url_ = ctx_.settings.calendar_ics_url;
        active_path_ = ctx_.settings.calendar_ics_path;
        active_refresh_minutes_ = ctx_.settings.calendar_refresh_minutes;
    }

    if (ctx_.settings.calendar_source == "off") {
        std::lock_guard<std::mutex> lock(events_mutex_);
        events_.clear();
        error_.clear();
        fetched_at_ = 0;
        from_cache_ = false;
        status_ = "Calendar import is off.";
        return;
    }
    request_refresh();
}

void CalendarScreen::refresh_recorded_calls() {
    seen_revision_ = ctx_.repo->revision();
    recorded_calls_ = ctx_.repo->list_calls();
}

const db::Call* CalendarScreen::recorded_call_for(const calendar::Event& event) const {
    return calendar::recorded_call_for_event(recorded_calls_, event);
}

void CalendarScreen::sync_agenda() {
    std::vector<calendar::Event> current;
    {
        std::lock_guard<std::mutex> lock(events_mutex_);
        current = events_;
    }
    // Only the identity of the events matters here; whether one has since been
    // recorded is read live when the row renders, so a finished call doesn't
    // have to rebuild the list.
    bool changed = current.size() != agenda_events_.size();
    for (size_t i = 0; !changed && i < current.size(); ++i)
        changed = current[i].uid != agenda_events_[i].uid || current[i].start != agenda_events_[i].start ||
                   current[i].summary != agenda_events_[i].summary;
    if (!changed) return;
    agenda_events_ = std::move(current);
    rebuild_agenda_rows();
}

void CalendarScreen::rebuild_agenda_rows() {
    using namespace ftxui;

    agenda_container_->DetachAllChildren();
    std::time_t current_day = 0;
    for (size_t i = 0; i < agenda_events_.size(); ++i) {
        const calendar::Event& event = agenda_events_[i];
        if (std::time_t day = start_of_day(event.start); day != current_day) {
            bool first = current_day == 0;
            current_day = day;
            std::string heading = day_heading(event.start);
            agenda_container_->Add(Renderer([heading, first] {
                Elements lines;
                if (!first) lines.push_back(text(""));
                lines.push_back(text(heading) | bold | color(theme::kAccent));
                return vbox(std::move(lines));
            }));
        }

        ButtonOption record_opt;
        record_opt.transform = [](const EntryState& s) {
            Element e = text(s.label) | color(theme::kSuccess);
            if (s.focused) e |= inverted;
            return e;
        };
        auto record_btn = Button(
            "[record]",
            [this, i] {
                if (!on_record_ || i >= agenda_events_.size()) return;
                const calendar::Event& e = agenda_events_[i];
                // Your own name is on the invite too; the Record screen's
                // participant list is everyone *else*.
                std::vector<std::string> participants;
                for (const std::string& attendee : e.attendees)
                    if (attendee != ctx_.settings.user_display_name) participants.push_back(attendee);
                on_record_(e.summary, participants);
            },
            record_opt);

        // The event is copied in, not referenced: sync_agenda() replaces
        // agenda_events_ wholesale, and a row that outlived its element would
        // be reading freed memory.
        agenda_container_->Add(Renderer(record_btn, [this, event, record_btn] {
            std::time_t now = std::time(nullptr);
            bool in_progress = now >= event.start && now < event.end;
            const db::Call* call = recorded_call_for(event);
            std::string when = event.all_day ? "all day" : hhmm(event.start) + "-" + hhmm(event.end);
            Element time_cell = text(" " + when + "  ") | size(WIDTH, EQUAL, 15);
            std::string title = event.summary.empty() ? "(no title)" : event.summary;
            // No manual truncation: the row is an hbox inside a framed pane, so
            // an over-long title is clipped by the layout rather than by a
            // width this renderer would have to be told.
            Element title_cell = text(title);
            if (in_progress) title_cell |= bold;
            if (!event.busy) title_cell |= dim;  // "show as free" -- not really a commitment
            // A recorded meeting is struck through and tagged, so a day's agenda
            // shows at a glance what has actually been dealt with.
            if (call) title_cell |= strikethrough;

            Elements row_cells = {time_cell, title_cell};
            if (call) row_cells.push_back(text("  ✓ completed") | color(theme::kSuccess) | bold);
            Element row = hbox(std::move(row_cells));
            if (call)
                row |= color(theme::kMuted);
            else if (in_progress)
                row |= color(theme::kSuccess);
            else if (event.end <= now)
                row |= color(theme::kMuted);

            Elements lines = {hbox({row, filler(), record_btn->Render(), text(" ")})};
            if (!event.organizer.empty()) lines.push_back(text("   " + truncate(event.organizer, 40)) | dim);
            if (!event.location.empty() && event.join_url.empty())
                lines.push_back(text("   " + truncate(event.location, 60)) | dim);
            if (!event.join_url.empty()) lines.push_back(text("   " + event.join_url) | dim);
            return vbox(std::move(lines));
        }));
    }
}

ftxui::Element CalendarScreen::render_month(int width) const {
    using namespace ftxui;
    std::time_t now = std::time(nullptr);
    std::time_t first = start_of_month(now);
    std::time_t grid_start = start_of_week(first);
    std::time_t month_end = add_months(first, 1);

    // Six rows covers every possible month layout; trailing all-blank weeks
    // are dropped below so a short month doesn't leave an empty band.
    int cell_width = std::max(12, (width - 2) / 7);
    int chips_per_cell = 3;

    std::vector<Elements> grid;
    Elements header;
    for (int i = 0; i < 7; ++i) {
        Element h = text(kWeekdayNames[i]) | hcenter | size(WIDTH, EQUAL, cell_width) | bold;
        h |= color(i >= 5 ? theme::kMuted : theme::kAccent);
        header.push_back(h);
    }
    grid.push_back(header);

    for (int week = 0; week < 6; ++week) {
        std::vector<Elements> cells;
        bool any_in_month = false;
        size_t tallest = 1;
        for (int dow = 0; dow < 7; ++dow) {
            std::time_t day = add_days(grid_start, week * 7 + dow);
            std::time_t day_end = add_days(day, 1);
            bool in_month = day >= first && day < month_end;
            bool is_today = same_day(day, now);
            if (in_month) any_in_month = true;

            Elements cell;
            Element num = text(std::to_string(day_of_month(day)));
            if (is_today)
                num = text("[" + std::to_string(day_of_month(day)) + "]") | bold | color(theme::kAccent);
            else if (!in_month)
                num |= color(theme::kMuted) | dim;
            else if (dow >= 5)
                num |= color(theme::kMuted);
            cell.push_back(num);

            int shown = 0, total = 0;
            for (const calendar::Event& e : events_) {
                if (e.start >= day_end || e.end <= day) continue;
                ++total;
                if (shown >= chips_per_cell) continue;
                const db::Call* chip_call = recorded_call_for(e);
                std::string label = e.all_day ? truncate(e.summary, cell_width - 1)
                                               : hhmm(e.start) + " " + truncate(e.summary, cell_width - 7);
                if (chip_call) label = "✓ " + label;
                Element chip = text(label);
                if (chip_call)
                    chip |= color(theme::kMuted) | strikethrough;
                else if (!e.busy)
                    chip |= dim;
                else if (e.end <= now)
                    chip |= color(theme::kMuted);
                else if (now >= e.start && now < e.end)
                    chip |= color(theme::kSuccess) | bold;
                cell.push_back(chip);
                ++shown;
            }
            if (total > shown) cell.push_back(text("+" + std::to_string(total - shown) + " more") | dim);
            tallest = std::max(tallest, cell.size());
            cells.push_back(std::move(cell));
        }
        if (!any_in_month) break;  // month ended before this week

        // Padded to the busiest day of *this* week rather than a fixed height,
        // so a quiet week collapses instead of reserving three blank chip rows.
        Elements row;
        for (int dow = 0; dow < 7; ++dow) {
            std::time_t day = add_days(grid_start, week * 7 + dow);
            while (cells[dow].size() < tallest) cells[dow].push_back(text(""));
            Element box = vbox(std::move(cells[dow])) | size(WIDTH, EQUAL, cell_width);
            box |= borderStyled(ROUNDED, same_day(day, now) ? theme::kAccent : theme::kMuted);
            row.push_back(box);
        }
        grid.push_back(std::move(row));
    }

    return vbox({
        text(month_title(now)) | bold | color(theme::kAccent) | hcenter,
        gridbox(std::move(grid)),
    });
}

ftxui::Element CalendarScreen::render_week(int width) const {
    using namespace ftxui;
    std::time_t now = std::time(nullptr);
    // Anchored on today rather than on Monday: a "next 7 days" window that
    // opened on Wednesday shouldn't render two mostly-empty past columns.
    std::time_t start = start_of_day(now);
    int cell_width = std::max(14, (width - 2) / 7);

    std::vector<Elements> grid;
    Elements header, body;
    for (int i = 0; i < 7; ++i) {
        std::time_t day = add_days(start, i);
        std::time_t day_end = add_days(day, 1);
        bool is_today = same_day(day, now);
        std::tm tm = local_tm(day);

        char label[32];
        std::snprintf(label, sizeof(label), "%s %02d/%02d", kWeekdayNames[weekday_mon0(day)], tm.tm_mday,
                       tm.tm_mon + 1);
        Element h = text(is_today ? std::string("[") + label + "]" : label) | hcenter |
                     size(WIDTH, EQUAL, cell_width) | bold;
        h |= color(is_today ? theme::kAccent : (weekday_mon0(day) >= 5 ? theme::kMuted : theme::kAccent));
        header.push_back(h);

        Elements cell;
        for (const calendar::Event& e : events_) {
            if (e.start >= day_end || e.end <= day) continue;
            const db::Call* chip_call = recorded_call_for(e);
            std::string title = e.summary.empty() ? "(no title)" : e.summary;
            if (chip_call) title = "✓ " + title;
            Element when = text(e.all_day ? "all day" : hhmm(e.start) + "-" + hhmm(e.end));
            Element what = text(truncate(title, cell_width - 1));
            Element chip = vbox({when | bold, what});
            if (chip_call)
                chip |= color(theme::kMuted) | strikethrough;
            else if (!e.busy)
                chip |= dim;
            else if (e.end <= now)
                chip |= color(theme::kMuted);
            else if (now >= e.start && now < e.end)
                chip |= color(theme::kSuccess);
            cell.push_back(chip);
            cell.push_back(text(""));
        }
        if (cell.empty()) cell.push_back(text("--") | dim | hcenter);

        // No flex here -- gridbox already levels the row to the busiest day,
        // and flex would stretch every column to the full pane height.
        Element box = vbox(std::move(cell)) | size(WIDTH, EQUAL, cell_width);
        box |= borderStyled(ROUNDED, is_today ? theme::kAccent : theme::kMuted);
        body.push_back(box);
    }
    grid.push_back(std::move(header));
    grid.push_back(std::move(body));
    return gridbox(std::move(grid));
}

ftxui::Element CalendarScreen::render_day(int width) const {
    using namespace ftxui;
    std::time_t now = std::time(nullptr);
    std::time_t day = start_of_day(now);
    std::time_t day_end = add_days(day, 1);

    Elements all_day_rows;
    // The hour band defaults to a working day and stretches only as far as the
    // events actually require, so a normal day isn't 24 rows of empty.
    int first_hour = 8, last_hour = 18;
    for (const calendar::Event& e : events_) {
        if (e.start >= day_end || e.end <= day) continue;
        if (e.all_day) {
            all_day_rows.push_back(hbox({text(" all day  │ "), text(e.summary)}) | color(theme::kWarning));
            continue;
        }
        int sh = local_tm(std::max(e.start, day)).tm_hour;
        int eh = local_tm(std::min(e.end, day_end - 1)).tm_hour;
        first_hour = std::min(first_hour, sh);
        last_hour = std::max(last_hour, eh);
    }
    int now_hour = local_tm(now).tm_hour;
    first_hour = std::min(first_hour, now_hour);
    last_hour = std::max(last_hour, now_hour);

    Elements rows;
    rows.push_back(text(day_heading(day)) | bold | color(theme::kAccent) | hcenter);
    for (Element& r : all_day_rows) rows.push_back(std::move(r));
    if (!all_day_rows.empty()) rows.push_back(separator() | color(theme::kMuted));

    for (int hour = first_hour; hour <= last_hour; ++hour) {
        std::time_t slot = day + hour * 3600;
        std::time_t slot_end = slot + 3600;

        Elements in_slot;
        for (const calendar::Event& e : events_) {
            if (e.all_day) continue;
            // Listed against the hour it starts in; an event running past the
            // hour keeps its full time range in the label.
            if (e.start < slot || e.start >= slot_end) continue;
            const db::Call* chip_call = recorded_call_for(e);
            std::string title = e.summary.empty() ? "(no title)" : e.summary;
            std::string label = hhmm(e.start) + "-" + hhmm(e.end) + "  " + title;
            if (!e.location.empty()) label += "  (" + e.location + ")";
            if (chip_call) label += "   ✓ completed";
            Element chip = text(truncate(label, std::max(20, width - 14)));
            if (chip_call)
                chip |= color(theme::kSuccess);
            else if (!e.busy)
                chip |= dim;
            else if (e.end <= now)
                chip |= color(theme::kMuted);
            else if (now >= e.start && now < e.end)
                chip |= color(theme::kSuccess) | bold;
            in_slot.push_back(chip);
        }

        char hour_label[8];
        std::snprintf(hour_label, sizeof(hour_label), "%02d:00", hour);
        bool current = now >= slot && now < slot_end;
        Element gutter = text(std::string(current ? ">" : " ") + hour_label + " │ ");
        gutter |= current ? color(theme::kAccent) | bold : color(theme::kMuted);

        if (in_slot.empty()) {
            rows.push_back(hbox({gutter, text("")}));
        } else {
            rows.push_back(hbox({gutter, vbox(std::move(in_slot))}));
        }
    }
    return vbox(std::move(rows));
}

void CalendarScreen::build_ui() {
    using namespace ftxui;

    auto source_dropdown = Dropdown(&source_labels_, &source_selected_);

    InputOption url_opt = styled_input_option();
    url_opt.multiline = false;
    auto url_input = WrappedInput(&ics_url_, "https://outlook.office365.com/owa/calendar/.../reachcalendar.ics", url_opt);

    InputOption path_opt = styled_input_option();
    path_opt.multiline = false;
    auto path_input = WrappedInput(&ics_path_, "/home/you/Downloads/calendar.ics", path_opt);

    InputOption mins_opt = styled_input_option();
    mins_opt.multiline = false;
    auto mins_input = WrappedInput(&refresh_minutes_, "30", mins_opt);

    auto save_btn = Button("Save & Import", [this] { save_source(); }, button_style(theme::kAccent));
    auto refresh_btn = Button("Refresh now", [this] { request_refresh(); }, button_style(theme::kAccent));
    auto setup_btn = Button("Setup", [this] { setup_expanded_ = !setup_expanded_; }, button_style(theme::kMuted));

    MenuOption range_opt = MenuOption::HorizontalAnimated();
    range_opt.on_change = [this] {
        active_range_.store(range_selected_);
        reparse_requested_ = true;
    };
    auto range_menu = Menu(&range_labels_, &range_selected_, range_opt);

    MenuOption view_opt = MenuOption::HorizontalAnimated();
    // The grid needs the neighbouring days that pad a month to whole weeks, so
    // switching view can change the parse window, not just the drawing.
    view_opt.on_change = [this] {
        active_view_.store(view_selected_);
        reparse_requested_ = true;
    };
    auto view_menu = Menu(&view_labels_, &view_selected_, view_opt);

    agenda_container_ = Container::Vertical({});
    list_container_ = ScrollableContent([this](int width) -> Element {
        std::lock_guard<std::mutex> lock(events_mutex_);
        if (events_.empty() && ctx_.settings.calendar_source == "off")
            return vbox({text("No calendar imported yet -- open Setup and pick a source.") | dim});

        switch (range_selected_) {
            case 0: return render_day(width);
            case 2: return render_month(width);
            default: return render_week(width);
        }
    });

    // Wrapped in Maybe so hidden fields drop out of the focus chain -- without
    // it, Tab would cycle through inputs that aren't on screen.
    auto setup_container = Maybe(Container::Vertical({
                                      source_dropdown,
                                      Maybe(url_input, [this] { return source_selected_ == 1; }),
                                      Maybe(path_input, [this] { return source_selected_ == 2; }),
                                      Maybe(mins_input, [this] { return source_selected_ != 0; }),
                                      save_btn,
                                  }),
                                  &setup_expanded_);

    // Only the view actually on screen stays in the focus chain, so Tab does
    // not walk through a hidden agenda's record buttons.
    auto layout = Container::Vertical({
        Container::Horizontal({view_menu, range_menu, refresh_btn, setup_btn}),
        setup_container,
        Maybe(agenda_container_, [this] { return view_selected_ == 1; }),
        Maybe(list_container_, [this] { return view_selected_ != 1; }),
    });

    root_ = Renderer(layout, [this, source_dropdown, url_input, path_input, mins_input, save_btn, refresh_btn,
                               setup_btn, range_menu, view_menu] {
        // A call finishing on the Record tab is what turns a meeting here into
        // a completed one, so the recorded-call list is re-read when the
        // repository moves rather than only at startup.
        if (ctx_.repo->revision() != seen_revision_) refresh_recorded_calls();

        Elements children;

        std::string status_copy, error_copy;
        bool cached_copy;
        std::time_t fetched_copy;
        {
            std::lock_guard<std::mutex> lock(events_mutex_);
            status_copy = status_;
            error_copy = error_;
            cached_copy = from_cache_;
            fetched_copy = fetched_at_;
        }

        // Status sits in the toolbar, beside the button it's about.
        Elements status_row = {text(status_copy) | dim};
        if (loading_.load()) {
            status_row.push_back(text("  importing...") | color(theme::kAccent));
        } else if (fetched_copy != 0) {
            status_row.push_back(text("  updated " + relative_age(fetched_copy)) | dim);
        }
        if (cached_copy) status_row.push_back(text("  cached copy") | color(theme::kWarning));

        children.push_back(hbox({
            view_menu->Render() | vcenter,
            // A plain glyph rather than separator(), which would draw a box
            // rule and weld itself to the panel border underneath.
            text("   ·   ") | color(theme::kMuted) | vcenter,
            range_menu->Render() | vcenter,
            filler(),
            hbox(std::move(status_row)) | vcenter,
            text("  "),
            refresh_btn->Render(),
            text(" "),
            setup_btn->Render(),
        }));
        if (!error_copy.empty())
            children.push_back(text(truncate(error_copy, 110)) | color(theme::kDanger));

        if (setup_expanded_) {
            Elements setup;
            setup.push_back(field_row("Source", source_dropdown->Render() | size(WIDTH, EQUAL, 30), 10));
            if (source_selected_ == 1) {
                setup.push_back(text("Outlook on the web -> Settings -> Calendar -> Shared calendars ->") | dim);
                setup.push_back(text("Publish a calendar -> 'Can view all details' -> copy the ICS link.") | dim);
                setup.push_back(text("Read-only, no sign-in. Treat the link as a password.") | dim);
                setup.push_back(url_input->Render());
                setup.push_back(hbox({text("Refresh every (minutes, min 5): "), mins_input->Render() | size(WIDTH, EQUAL, 12)}));
            } else if (source_selected_ == 2) {
                setup.push_back(text("Outlook -> File -> Save Calendar as .ics (or save a meeting invite),") | dim);
                setup.push_back(text("then point at the file. Re-export whenever you want fresh data.") | dim);
                setup.push_back(path_input->Render());
                setup.push_back(hbox({text("Re-read every (minutes, min 5): "), mins_input->Render() | size(WIDTH, EQUAL, 12)}));
            } else {
                setup.push_back(text("Calendar import is off.") | dim);
            }
            setup.push_back(hbox({filler(), save_btn->Render()}));
            children.push_back(section("Calendar source", vbox(std::move(setup))));
        }

        if (view_selected_ == 1) {
            sync_agenda();
            bool nothing_configured = agenda_events_.empty() && ctx_.settings.calendar_source == "off";
            if (nothing_configured)
                children.push_back(text("No calendar imported yet -- open Setup and pick a source.") | dim);
            else if (agenda_events_.empty())
                children.push_back(text("Nothing scheduled in this range.") | dim);
            else
                children.push_back(agenda_container_->Render() | frame | flex |
                                    borderStyled(ROUNDED, theme::kMuted));
        } else {
            children.push_back(list_container_->Render() | flex);
        }
        return vbox(std::move(children)) | flex;
    });
}

}  // namespace callscribe::ui
