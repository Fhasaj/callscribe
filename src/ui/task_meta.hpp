#pragma once
#include <string>

#include "db/repository.hpp"
#include "util/date_format.hpp"

namespace callscribe::ui {

// The one-line "Due 14 08 2026 · High priority · Acme Corp" caption shown under
// a task. The Tasks screen renders these fields itself (it colours them by
// urgency); this is for the read-only task lists on the Record and History
// screens, which just need the same facts in the same order.
inline std::string task_meta_line(const db::Task& task) {
    std::string out = task.due_date.empty() ? "No due date" : "Due " + util::iso_date_to_display(task.due_date);
    if (task.priority == db::kPriorityHigh) {
        out += "  ·  High priority";
    } else if (task.priority == db::kPriorityLow) {
        out += "  ·  Low priority";
    } else {
        out += "  ·  Medium priority";
    }
    if (!task.customer_name.empty()) out += "  ·  " + task.customer_name;
    return out;
}

}  // namespace callscribe::ui
