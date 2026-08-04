#include "ui/tasks_screen.hpp"

#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <cctype>

#include "ui/styled_input.hpp"
#include "ui/theme.hpp"
#include "ui/wrapped_input.hpp"
#include "util/date_format.hpp"
#include "util/time.hpp"

namespace callscribe::ui {

namespace {

std::string trim_lower(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

}  // namespace

TasksScreen::TasksScreen(AppContext& ctx) : ctx_(ctx) {
    build_ui();
    refresh();
}

void TasksScreen::refresh() {
    using namespace ftxui;

    tasks_ = ctx_.repo->list_tasks(show_completed_ ? "" : "open");
    std::string today = util::today_iso();

    task_checked_.assign(tasks_.size(), false);
    row_state_.assign(tasks_.size(), TaskRowState::Normal);
    open_count_ = 0;
    due_today_count_ = 0;
    overdue_count_ = 0;

    for (size_t i = 0; i < tasks_.size(); ++i) {
        task_checked_[i] = (tasks_[i].status == "done");
        if (tasks_[i].status == "done") {
            row_state_[i] = TaskRowState::Done;
            continue;
        }
        ++open_count_;
        if (!tasks_[i].due_date.empty() && tasks_[i].due_date < today) {
            row_state_[i] = TaskRowState::Overdue;
            ++overdue_count_;
        } else if (tasks_[i].due_date == today) {
            row_state_[i] = TaskRowState::DueToday;
            ++due_today_count_;
        }
    }

    task_list_container_->DetachAllChildren();
    for (size_t i = 0; i < tasks_.size(); ++i) {
        std::string label = tasks_[i].description;
        if (!tasks_[i].customer_name.empty()) label += "  -- " + tasks_[i].customer_name;
        if (!tasks_[i].due_date.empty()) label += "  (due " + util::iso_date_to_display(tasks_[i].due_date) + ")";
        if (tasks_[i].source == "call") label += "  [call]";
        TaskRowState state = row_state_[i];
        if (state == TaskRowState::Overdue) label += "  [overdue]";
        if (state == TaskRowState::DueToday) label += "  [today]";

        CheckboxOption cb_opt;
        cb_opt.on_change = [this, i] { toggle_task(static_cast<int>(i)); };
        cb_opt.transform = [state](const EntryState& s) {
            auto prefix = text(s.state ? "▣ " : "☐ ");  // "▣ " / "☐ "
            auto label_text = text(s.label);
            if (s.active) label_text |= bold;
            if (s.focused) label_text |= inverted;
            Element row = hbox({prefix, label_text});
            switch (state) {
                case TaskRowState::Overdue:
                    row |= color(theme::kDanger);
                    break;
                case TaskRowState::DueToday:
                    row |= color(theme::kWarning);
                    break;
                case TaskRowState::Done:
                    row |= color(theme::kMuted);
                    break;
                case TaskRowState::Normal:
                    break;
            }
            return row;
        };
        auto checkbox = Checkbox(label, &task_checked_[i], cb_opt);
        auto delete_btn = Button(
            "x", [this, i] { delete_task_at(static_cast<int>(i)); }, ButtonOption::Animated(theme::kDanger));

        task_list_container_->Add(Container::Horizontal({checkbox, delete_btn}));
    }
}

void TasksScreen::toggle_task(int index) {
    if (index < 0 || index >= static_cast<int>(tasks_.size())) return;
    bool checked_now = task_checked_[index];
    if (checked_now) {
        ctx_.repo->complete_task(tasks_[index].id);
        tasks_[index].status = "done";
    } else {
        ctx_.repo->reopen_task(tasks_[index].id);
        tasks_[index].status = "open";
    }
    needs_refresh_ = true;
}

void TasksScreen::delete_task_at(int index) {
    if (index < 0 || index >= static_cast<int>(tasks_.size())) return;
    ctx_.repo->delete_task(tasks_[index].id);
    needs_refresh_ = true;
}

void TasksScreen::add_task() {
    if (new_task_text_.empty()) return;

    std::string due_iso;
    if (!new_task_due_.empty()) {
        std::string typed = trim_lower(new_task_due_);
        if (typed == "today") {
            due_iso = util::today_iso();
        } else if (typed == "tomorrow" || typed == "tom") {
            due_iso = util::offset_days_iso(1);
        } else {
            due_iso = util::display_date_to_iso(new_task_due_);
        }
        if (due_iso.empty()) {
            status_message_ = "Couldn't parse due date '" + new_task_due_ + "' -- use DD MM YYYY, 'today', or 'tomorrow'.";
            return;
        }
    }

    ctx_.repo->add_task(new_task_text_, std::nullopt, std::nullopt, due_iso);
    new_task_text_.clear();
    new_task_due_.clear();
    status_message_.clear();
    needs_refresh_ = true;
}

void TasksScreen::build_ui() {
    using namespace ftxui;

    task_list_container_ = Container::Vertical({});

    InputOption desc_opt = styled_input_option();
    desc_opt.multiline = false;
    desc_opt.on_enter = [this] { add_task(); };
    auto desc_input = WrappedInput(&new_task_text_, "New task description...", desc_opt);

    InputOption due_opt = styled_input_option();
    due_opt.multiline = false;
    due_opt.on_enter = [this] { add_task(); };
    auto due_input = WrappedInput(&new_task_due_, "Due DD MM YYYY / today / tomorrow (optional)", due_opt);

    auto add_button = Button("Add Task", [this] { add_task(); }, ButtonOption::Animated(theme::kAccent));

    CheckboxOption show_done_opt;
    show_done_opt.on_change = [this] { needs_refresh_ = true; };
    auto show_done_checkbox = Checkbox("Show completed", &show_completed_, show_done_opt);

    auto top_row = Container::Horizontal({desc_input, due_input, add_button});
    auto layout = Container::Vertical({top_row, show_done_checkbox, task_list_container_});

    root_ = Renderer(layout, [this, desc_input, due_input, add_button, show_done_checkbox] {
        if (needs_refresh_) {
            needs_refresh_ = false;
            refresh();
        }

        Elements children;
        children.push_back(text("CallScribe -- Tasks") | bold | color(theme::kAccent));

        Elements summary_parts = {text(std::to_string(open_count_) + " open")};
        if (due_today_count_ > 0) {
            summary_parts.push_back(text("   "));
            summary_parts.push_back(text(std::to_string(due_today_count_) + " due today") | color(theme::kWarning));
        }
        if (overdue_count_ > 0) {
            summary_parts.push_back(text("   "));
            summary_parts.push_back(text(std::to_string(overdue_count_) + " overdue") | color(theme::kDanger) | bold);
        }
        children.push_back(hbox(summary_parts));
        children.push_back(separator() | color(theme::kMuted));

        children.push_back(hbox({
            desc_input->Render() | flex,
            text(" "),
            due_input->Render() | size(WIDTH, EQUAL, 34),
            text(" "),
            add_button->Render(),
        }));
        children.push_back(show_done_checkbox->Render());
        if (!status_message_.empty()) children.push_back(text(status_message_) | color(theme::kDanger));

        if (overdue_count_ > 0) {
            children.push_back(text(std::to_string(overdue_count_) + " task(s) overdue -- take a look") |
                                color(theme::kDanger) | bold);
        } else if (due_today_count_ > 0) {
            children.push_back(text(std::to_string(due_today_count_) + " task(s) due today") | color(theme::kWarning));
        }

        children.push_back(separator() | color(theme::kMuted));
        children.push_back(task_list_container_->Render() | frame | flex);

        return vbox(children) | flex;
    });
}

}  // namespace callscribe::ui
