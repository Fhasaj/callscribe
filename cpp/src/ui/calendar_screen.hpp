#pragma once
#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <utility>
#include <string>
#include <thread>
#include <vector>

#include "calendar/calendar_source.hpp"
#include "context.hpp"
#include "db/repository.hpp"

namespace callscribe::ui {

class CalendarScreen {
public:
    explicit CalendarScreen(AppContext& ctx);
    ~CalendarScreen();
    CalendarScreen(const CalendarScreen&) = delete;
    CalendarScreen& operator=(const CalendarScreen&) = delete;

    ftxui::Component component() { return root_; }

    // Called when a meeting's "record" button is pressed, with the event's
    // title and its attendees (the user's own name filtered out). The app wires
    // this to the Record tab, which fills its form in and takes focus -- so a
    // scheduled call is set up by pressing one button rather than retyping the
    // meeting name and everyone on it.
    using RecordRequest =
        std::function<void(const std::string& meeting_name, const std::vector<std::string>& participants)>;
    void set_on_record(RecordRequest handler) { on_record_ = std::move(handler); }

private:
    AppContext& ctx_;

    // Source setup, edited on this screen and written straight to Settings --
    // picking between the two import options is the whole point of the tab.
    std::vector<std::string> source_labels_ = {"Off", "Published ICS URL", "Local .ics file"};
    int source_selected_ = 0;
    std::string ics_url_;
    std::string ics_path_;
    std::string refresh_minutes_;
    bool setup_expanded_ = false;

    // The grid is the default -- "Calendar" reading as a list was the whole
    // complaint; the agenda stays available for a dense, scannable rundown.
    std::vector<std::string> view_labels_ = {"Calendar", "Agenda"};
    int view_selected_ = 0;
    std::atomic<int> active_view_{0};

    std::vector<std::string> range_labels_ = {"Day", "Week", "Month"};
    int range_selected_ = 1;
    // The menu needs a plain int*, but the worker thread reads the range too --
    // it gets this copy instead.
    std::atomic<int> active_range_{1};

    // The source config the worker reads. Kept separate from ctx_.settings,
    // which belongs to the UI thread, so a mid-fetch Save can't tear a string
    // out from under the download.
    mutable std::mutex config_mutex_;
    calendar::SourceKind active_kind_ = calendar::SourceKind::Off;
    std::string active_url_;
    std::string active_path_;
    int active_refresh_minutes_ = 30;

    mutable std::mutex events_mutex_;
    std::vector<calendar::Event> events_;
    std::string status_;
    std::string error_;
    bool from_cache_ = false;
    std::time_t fetched_at_ = 0;

    std::atomic<bool> loading_{false};
    std::atomic<bool> stop_{false};
    std::atomic<bool> refresh_requested_{false};  // re-download / re-read the source
    std::atomic<bool> reparse_requested_{false};  // re-expand the cache into a new date range
    std::thread worker_;

    // Calls already recorded, so a meeting that has been through CallScribe can
    // be marked as done on the calendar. Reloaded when the repository changes
    // rather than per frame -- the grids ask about every event they draw.
    std::vector<db::Call> recorded_calls_;
    uint64_t seen_revision_ = 0;

    ftxui::Component list_container_;
    // The agenda is a real container, not part of list_container_'s drawn
    // content: every row carries a "record" button, and a scroll pane is a
    // leaf component that can't hold focusable children.
    ftxui::Component agenda_container_;
    // GUI-thread copy of what the agenda rows were built from. Comparing
    // against it is how we know the worker has delivered a different set of
    // events and the rows have to be rebuilt -- which must not happen every
    // frame, or the buttons would be replaced out from under the click.
    std::vector<calendar::Event> agenda_events_;
    RecordRequest on_record_;

    ftxui::Component root_;

    void start_worker();
    void request_refresh();
    // `fetch` false re-expands the cached feed only -- switching date range
    // shouldn't cost a round trip to Microsoft.
    void load_now(bool fetch);
    void save_source();
    void window_for_range(std::time_t& start, std::time_t& end) const;

    void refresh_recorded_calls();
    // The call recorded for this event, if there is one: either a call whose
    // recording overlaps the event's slot, or one named after the event on the
    // same day. Null when the meeting hasn't been recorded.
    const db::Call* recorded_call_for(const calendar::Event& event) const;

    // Copies the current events out from under events_mutex_ and rebuilds the
    // agenda rows if they changed. GUI thread only.
    void sync_agenda();
    void rebuild_agenda_rows();

    // The grid renderers take events_mutex_ already held by the caller.
    ftxui::Element render_month(int width) const;
    ftxui::Element render_week(int width) const;
    ftxui::Element render_day(int width) const;

    void build_ui();
};

}  // namespace callscribe::ui
