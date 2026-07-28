#include "ui/tasks_screen.hpp"

#include <ftxui/dom/elements.hpp>

#include "util/date_format.hpp"

namespace callscribe::ui {

TasksScreen::TasksScreen(AppContext& ctx) : ctx_(ctx) {
    build_ui();
    refresh();
}

void TasksScreen::refresh() {
    using namespace ftxui;

    tasks_ = ctx_.repo->list_tasks(show_completed_ ? "" : "open");
    task_checked_.assign(tasks_.size(), false);
    for (size_t i = 0; i < tasks_.size(); ++i) task_checked_[i] = (tasks_[i].status == "done");

    task_list_container_->DetachAllChildren();
    for (size_t i = 0; i < tasks_.size(); ++i) {
        std::string label = tasks_[i].description;
        if (!tasks_[i].customer_name.empty()) label += "  -- " + tasks_[i].customer_name;
        if (!tasks_[i].due_date.empty()) label += "  (due " + util::iso_date_to_display(tasks_[i].due_date) + ")";
        if (tasks_[i].source == "call") label += "  [call]";

        CheckboxOption cb_opt;
        cb_opt.on_change = [this, i] { toggle_task(static_cast<int>(i)); };
        auto checkbox = Checkbox(label, &task_checked_[i], cb_opt);
        auto delete_btn = Button(
            "x", [this, i] { delete_task_at(static_cast<int>(i)); }, ButtonOption::Ascii());

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
    if (!show_completed_ && checked_now) needs_refresh_ = true;
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
        due_iso = util::display_date_to_iso(new_task_due_);
        if (due_iso.empty()) {
            status_message_ = "Couldn't parse due date '" + new_task_due_ + "' -- use DD MM YYYY.";
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

    InputOption desc_opt;
    desc_opt.multiline = false;
    desc_opt.on_enter = [this] { add_task(); };
    auto desc_input = Input(&new_task_text_, "New task description...", desc_opt);

    InputOption due_opt;
    due_opt.multiline = false;
    auto due_input = Input(&new_task_due_, "Due DD MM YYYY (optional)", due_opt);

    auto add_button = Button("Add Task", [this] { add_task(); });

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
        return vbox({
                   text("CallScribe -- Tasks") | bold,
                   separator(),
                   hbox({
                       desc_input->Render() | flex,
                       text(" "),
                       due_input->Render() | size(WIDTH, EQUAL, 24),
                       text(" "),
                       add_button->Render(),
                   }),
                   show_done_checkbox->Render(),
                   status_message_.empty() ? text("") : (text(status_message_) | color(Color(Color::Red))),
                   separator(),
                   task_list_container_->Render() | frame | flex,
               }) |
               flex;
    });
}

}  // namespace callscribe::ui
