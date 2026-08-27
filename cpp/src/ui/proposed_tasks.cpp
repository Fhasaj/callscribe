#include "ui/proposed_tasks.hpp"

#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <cctype>
#include <utility>

#include "ui/styled_input.hpp"
#include "ui/theme.hpp"
#include "ui/wrapped_input.hpp"
#include "util/date_format.hpp"
#include "util/time.hpp"

namespace callscribe::ui {

namespace {

using namespace ftxui;

// Shared by every row's Toggle, which wants a pointer to a list that outlives
// the component.
const std::vector<std::string> kPriorityLabels = {"Low", "Med", "High"};

std::string priority_from_index(int index) {
    switch (index) {
        case 0:
            return db::kPriorityLow;
        case 2:
            return db::kPriorityHigh;
        default:
            return db::kPriorityMedium;
    }
}

int index_from_priority(const std::string& priority) {
    if (priority == db::kPriorityLow) return 0;
    if (priority == db::kPriorityHigh) return 2;
    return 1;
}

std::string trim_lower(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

// Accepts the same shorthand the Tasks tab does. Returns false (leaving `iso`
// untouched) when the text is present but unparsable; an empty box is a
// successful "no due date".
bool parse_due(const std::string& typed, std::string& iso) {
    if (typed.empty()) {
        iso.clear();
        return true;
    }
    std::string lowered = trim_lower(typed);
    if (lowered == "today") {
        iso = util::today_iso();
        return true;
    }
    if (lowered == "tomorrow" || lowered == "tom") {
        iso = util::offset_days_iso(1);
        return true;
    }
    std::string parsed = util::display_date_to_iso(typed);
    if (parsed.empty()) return false;
    iso = parsed;
    return true;
}

// A single-line button, as used on the Tasks tab rows: ButtonOption::Animated
// pads with borderEmpty, which would make every row three lines tall.
ButtonOption row_button(Color tint) {
    ButtonOption opt;
    opt.transform = [tint](const EntryState& s) {
        Element e = text(s.label) | color(tint);
        if (s.focused) e |= inverted;
        return e;
    };
    return opt;
}

}  // namespace

ProposedTasksPanel::ProposedTasksPanel(AppContext& ctx, std::function<int64_t()> call_id)
    : ctx_(ctx), call_id_(std::move(call_id)) {
    build_ui();
}

void ProposedTasksPanel::reload() {
    int64_t call = call_id_ ? call_id_() : -1;
    seen_call_id_ = call;
    seen_revision_ = ctx_.repo->revision();
    needs_rebuild_ = false;

    std::vector<db::Task> proposed;
    if (call >= 0) {
        for (auto& t : ctx_.repo->list_tasks_for_call(call))
            if (t.status == db::kStatusProposed) proposed.push_back(std::move(t));
    }

    // Reuse the row object for an id that is still proposed, so a description
    // being edited right now survives a reload triggered by an unrelated write.
    std::vector<std::shared_ptr<Row>> next;
    next.reserve(proposed.size());
    for (const db::Task& task : proposed) {
        auto existing = std::find_if(rows_.begin(), rows_.end(),
                                      [&task](const std::shared_ptr<Row>& r) { return r->id == task.id; });
        if (existing != rows_.end()) {
            next.push_back(*existing);
            continue;
        }
        auto row = std::make_shared<Row>();
        row->id = task.id;
        row->description = task.description;
        row->due = task.due_date.empty() ? "" : util::iso_date_to_display(task.due_date);
        row->priority = index_from_priority(task.priority);
        row->customer_name = task.customer_name;
        next.push_back(std::move(row));
    }

    bool changed = next.size() != rows_.size();
    for (size_t i = 0; !changed && i < next.size(); ++i) changed = next[i] != rows_[i];
    rows_ = std::move(next);
    if (changed) rebuild_rows();
}

void ProposedTasksPanel::rebuild_rows() {
    rows_container_->DetachAllChildren();
    for (const std::shared_ptr<Row>& row : rows_) {
        InputOption desc_opt = styled_input_option();
        desc_opt.multiline = false;
        desc_opt.on_enter = [this, row] { confirm(row); };
        auto desc_input = WrappedInput(&row->description, "Task description...", desc_opt);

        InputOption due_opt = styled_input_option();
        due_opt.multiline = false;
        due_opt.on_enter = [this, row] { confirm(row); };
        auto due_input = WrappedInput(&row->due, "Due (optional)", due_opt);

        auto priority_toggle = Toggle(&kPriorityLabels, &row->priority);
        auto confirm_btn = Button(
            "[✓ confirm]", [this, row] { confirm(row); }, row_button(theme::kSuccess));
        auto discard_btn = Button(
            "[x]", [this, row] { discard(row); }, row_button(theme::kDanger));

        auto controls = Container::Horizontal({desc_input, due_input, priority_toggle, confirm_btn, discard_btn});
        rows_container_->Add(Renderer(controls, [row, desc_input, due_input, priority_toggle, confirm_btn,
                                                  discard_btn] {
            Elements meta = {
                text("    "),
                text("Due ") | color(theme::kMuted),
                due_input->Render() | size(WIDTH, EQUAL, 26),
                text(" "),
                priority_toggle->Render(),
                text("  "),
            };
            if (!row->customer_name.empty()) {
                meta.push_back(text(row->customer_name) | color(theme::kAccent));
                meta.push_back(text("  "));
            }
            meta.push_back(confirm_btn->Render());
            meta.push_back(text(" "));
            meta.push_back(discard_btn->Render());
            meta.push_back(filler());

            Elements lines = {desc_input->Render(), hbox(std::move(meta))};
            if (!row->error.empty()) lines.push_back(text("    " + row->error) | color(theme::kDanger));
            return vbox(std::move(lines));
        }));
    }
}

bool ProposedTasksPanel::confirm(const std::shared_ptr<Row>& row) {
    row->error.clear();
    if (row->description.empty()) {
        row->error = "Give the task a description, or discard it.";
        return false;
    }
    std::string due_iso;
    if (!parse_due(row->due, due_iso)) {
        row->error = "Couldn't read '" + row->due + "' -- use DD MM YYYY, 'today', or 'tomorrow'.";
        return false;
    }

    std::optional<int64_t> customer_id;
    if (!row->customer_name.empty()) customer_id = ctx_.repo->get_or_create_customer(row->customer_name);

    // Edits are written first, then the status flips -- so a task only becomes
    // real in the shape the user actually approved.
    ctx_.repo->update_task(row->id, row->description, due_iso, priority_from_index(row->priority), customer_id);
    ctx_.repo->confirm_task(row->id);
    needs_rebuild_ = true;
    return true;
}

void ProposedTasksPanel::confirm_all() {
    int confirmed = 0;
    int failed = 0;
    // Copied: confirm() marks the panel dirty, and the reload it triggers
    // replaces rows_ underneath this loop.
    std::vector<std::shared_ptr<Row>> snapshot = rows_;
    for (const std::shared_ptr<Row>& row : snapshot) {
        if (confirm(row))
            ++confirmed;
        else
            ++failed;
    }
    status_message_ = failed == 0 ? std::to_string(confirmed) + " task(s) added."
                                   : std::to_string(confirmed) + " added, " + std::to_string(failed) +
                                         " need attention (see below).";
}

void ProposedTasksPanel::discard(const std::shared_ptr<Row>& row) {
    ctx_.repo->delete_task(row->id);
    needs_rebuild_ = true;
}

void ProposedTasksPanel::build_ui() {
    using namespace ftxui;

    rows_container_ = Container::Vertical({});

    auto confirm_all_btn = Button(
        " Confirm all ", [this] { confirm_all(); }, ButtonOption::Animated(theme::kSuccess));
    auto discard_all_btn = Button(
        " Discard all ", [this] {
            for (const std::shared_ptr<Row>& row : rows_) ctx_.repo->delete_task(row->id);
            status_message_ = "Proposed tasks discarded.";
            needs_rebuild_ = true;
        },
        ButtonOption::Animated(theme::kDanger));

    auto layout = Container::Vertical({Container::Horizontal({confirm_all_btn, discard_all_btn}), rows_container_});
    // With nothing to review the panel draws nothing, so its controls must also
    // leave the focus chain -- otherwise Tab would stop on invisible buttons.
    auto gated = Maybe(layout, [this] { return !rows_.empty(); });

    root_ = Renderer(gated, [this, confirm_all_btn, discard_all_btn] {
        int64_t call = call_id_ ? call_id_() : -1;
        if (needs_rebuild_ || call != seen_call_id_ || ctx_.repo->revision() != seen_revision_) reload();

        // Nothing to review: the host screen drops the section via empty(), and
        // the Maybe above has already taken the controls out of the focus chain.
        if (rows_.empty()) return text("");

        Elements children;
        children.push_back(hbox({
            text("Suggested tasks (" + std::to_string(rows_.size()) + ")") | bold | color(theme::kWarning),
            text("  not saved yet -- edit, then confirm or discard") | color(theme::kMuted),
            filler(),
            confirm_all_btn->Render(),
            text(" "),
            discard_all_btn->Render(),
        }));
        if (!status_message_.empty()) children.push_back(text(status_message_) | color(theme::kMuted));
        children.push_back(rows_container_->Render());
        return vbox(std::move(children)) | borderStyled(ROUNDED, theme::kWarning);
    });
}

}  // namespace callscribe::ui
