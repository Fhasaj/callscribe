#pragma once
#include <ftxui/component/component.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "context.hpp"

namespace callscribe::ui {

// Review panel for the action items the summarizer pulled out of a call.
//
// Those items used to go straight into the task list, which meant every
// hallucinated or half-heard "action" became a real task you then had to hunt
// down and delete. Now the summarizer files them as db::kStatusProposed --
// attached to the call, invisible everywhere else -- and this panel is where
// they are edited, discarded, or confirmed into real tasks.
//
// Editing is in place: each row is a live description / due-date / priority
// control bound to that row, and nothing is written to the database until the
// row is confirmed. Rows are only rebuilt from the DB when the *set* of
// proposed ids changes, so a summary finishing on another thread can't wipe
// out half-typed edits.
//
// Both the Record tab and the History tab own one of these, pointed at
// whichever call they are currently showing.
class ProposedTasksPanel {
public:
    // `call_id` is polled each render: return -1 when no call is selected.
    ProposedTasksPanel(AppContext& ctx, std::function<int64_t()> call_id);

    ftxui::Component component() { return root_; }

    // True when there is nothing awaiting review -- lets the host screen drop
    // the whole section (and its separator) rather than draw an empty box.
    bool empty() const { return rows_.empty(); }

private:
    // One editable row. Held by shared_ptr so the addresses handed to the
    // Input/Toggle components stay valid as the vector is rebuilt.
    struct Row {
        int64_t id = 0;
        std::string description;
        std::string due;  // DD MM YYYY / today / tomorrow, blank for none
        int priority = 1;  // index into the Low/Medium/High labels
        std::string customer_name;
        std::string error;  // set when a due date won't parse
    };

    AppContext& ctx_;
    std::function<int64_t()> call_id_;

    std::vector<std::shared_ptr<Row>> rows_;
    int64_t seen_call_id_ = -2;  // -2 so the first render always loads
    uint64_t seen_revision_ = 0;
    bool needs_rebuild_ = true;
    std::string status_message_;

    ftxui::Component rows_container_;
    ftxui::Component root_;

    // Re-reads the call's proposed tasks, but keeps the existing row objects
    // (and so any in-progress edits) for ids that are still there.
    void reload();
    void rebuild_rows();
    // Writes the row's edits back and flips it to a real open task. Returns
    // false and fills row->error when the due date can't be parsed.
    bool confirm(const std::shared_ptr<Row>& row);
    void confirm_all();
    void discard(const std::shared_ptr<Row>& row);
    void build_ui();
};

}  // namespace callscribe::ui
