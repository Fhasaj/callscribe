#include "ui/tasks_screen.hpp"

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <cctype>

#include "llm/summarizer.hpp"
#include "ui/styled_input.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"
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

const std::vector<std::string> kPriorityLabels = {"Low", "Medium", "High"};

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

ftxui::Color priority_color(const std::string& priority) {
    if (priority == db::kPriorityHigh) return theme::kDanger;
    if (priority == db::kPriorityLow) return theme::kMuted;
    return theme::kWarning;
}

std::string priority_display(const std::string& priority) {
    return kPriorityLabels[static_cast<size_t>(index_from_priority(priority))];
}

}  // namespace

TasksScreen::TasksScreen(AppContext& ctx) : ctx_(ctx) {
    build_ui();
    refresh();
}

TasksScreen::~TasksScreen() {
    if (breakdown_worker_.joinable()) breakdown_worker_.join();
}

void TasksScreen::start_breakdown(int index) {
    if (index < 0 || index >= static_cast<int>(tasks_.size()) || breaking_down_.exchange(true)) return;
    if (breakdown_worker_.joinable()) breakdown_worker_.join();

    breakdown_task_ = tasks_[static_cast<size_t>(index)];
    status_message_ = "Breaking down \"" + breakdown_task_.description + "\"...";
    const std::string host = ctx_.settings.llm_host();
    const std::string model = ctx_.settings.llm_model();
    const db::Task task = breakdown_task_;
    breakdown_worker_ = std::thread([this, host, model, task] {
        std::vector<std::string> steps;
        std::string error;
        try {
            steps = llm::break_down_task(host, model, task.description, task.customer_name, task.due_date);
            if (steps.empty()) error = "The model didn't suggest any steps -- try again?";
        } catch (const std::exception& e) {
            error = e.what();
        }
        std::lock_guard<std::mutex> lock(breakdown_mutex_);
        breakdown_result_ = std::move(steps);
        breakdown_error_ = std::move(error);
        breakdown_ready_ = true;
    });
}

void TasksScreen::take_breakdown_result() {
    {
        std::lock_guard<std::mutex> lock(breakdown_mutex_);
        if (!breakdown_ready_) return;
        breakdown_ready_ = false;
        breakdown_steps_ = std::move(breakdown_result_);
        if (!breakdown_error_.empty()) status_message_ = "Couldn't break it down: " + breakdown_error_;
    }
    breaking_down_.store(false);
    if (breakdown_steps_.empty()) return;
    status_message_.clear();

    using namespace ftxui;
    breakdown_checked_.assign(breakdown_steps_.size(), true);
    breakdown_list_->DetachAllChildren();
    for (size_t i = 0; i < breakdown_steps_.size(); ++i)
        breakdown_list_->Add(Checkbox(breakdown_steps_[i], &breakdown_checked_[i]));
    breakdown_visible_ = true;
    breakdown_first_->TakeFocus();
}

void TasksScreen::add_breakdown_steps() {
    int added = 0;
    for (size_t i = 0; i < breakdown_steps_.size(); ++i) {
        if (!breakdown_checked_[i]) continue;
        // Same customer, due date and priority as the task they came from, so
        // they sort and remind alongside it.
        ctx_.repo->add_task(breakdown_steps_[i], std::nullopt, breakdown_task_.customer_id, breakdown_task_.due_date,
                            "breakdown", breakdown_task_.priority);
        ++added;
    }
    breakdown_visible_ = false;
    status_message_ = added ? "Added " + std::to_string(added) + " step(s)." : "";
    needs_refresh_ = true;
}

void TasksScreen::refresh() {
    using namespace ftxui;

    seen_revision_ = ctx_.repo->revision();
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

    // The task being edited is still listed, but flagged, so it's obvious
    // which row the form at the top belongs to.
    task_list_container_->DetachAllChildren();
    for (size_t i = 0; i < tasks_.size(); ++i) {
        const db::Task& task = tasks_[i];
        std::string label = task.description;
        if (task.source == "call") label += "  [call]";
        TaskRowState state = row_state_[i];

        CheckboxOption cb_opt;
        cb_opt.on_change = [this, i] { toggle_task(static_cast<int>(i)); };
        cb_opt.transform = [state](const EntryState& s) {
            auto prefix = text(s.state ? "▣ " : "☐ ");
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
        // Plain single-line buttons: ButtonOption::Animated pads its label with
        // borderEmpty, which would make every task row three lines tall and
        // push the description away from its own due-date line.
        auto row_button_option = [](ftxui::Color tint) {
            ButtonOption opt;
            opt.transform = [tint](const EntryState& s) {
                Element e = text(s.label) | color(tint);
                if (s.focused) e |= inverted;
                return e;
            };
            return opt;
        };
        auto edit_btn = Button(
            "[edit]", [this, i] { begin_edit(static_cast<int>(i)); }, row_button_option(theme::kAccent));
        auto delete_btn = Button(
            "[x]", [this, i] { delete_task_at(static_cast<int>(i)); }, row_button_option(theme::kDanger));
        auto steps_btn = Button(
            "[steps]", [this, i] { start_breakdown(static_cast<int>(i)); }, row_button_option(theme::kSuccess));

        auto controls = Container::Horizontal({checkbox, steps_btn, edit_btn, delete_btn});
        // The due date / priority / customer live on their own line under the
        // description, so a long description doesn't push them off-screen.
        bool being_edited = task.id == editing_id_;
        task_list_container_->Add(Renderer(controls, [this, controls, checkbox, steps_btn, edit_btn, delete_btn, task,
                                                       state, being_edited] {
            Elements meta;
            meta.push_back(text("    "));
            if (task.due_date.empty()) {
                meta.push_back(text("No due date") | color(theme::kMuted));
            } else {
                std::string due = "Due " + util::iso_date_to_display(task.due_date);
                if (state == TaskRowState::Overdue) due += " (overdue)";
                if (state == TaskRowState::DueToday) due += " (today)";
                Color due_color = state == TaskRowState::Overdue    ? theme::kDanger
                                   : state == TaskRowState::DueToday ? theme::kWarning
                                                                     : theme::kMuted;
                meta.push_back(text(due) | color(due_color));
            }
            meta.push_back(text("  ·  ") | color(theme::kMuted));
            meta.push_back(text(priority_display(task.priority)) | color(priority_color(task.priority)) |
                            (task.priority == db::kPriorityHigh ? bold : nothing));
            meta.push_back(text(" priority") | color(theme::kMuted));
            if (!task.customer_name.empty()) {
                meta.push_back(text("  ·  ") | color(theme::kMuted));
                meta.push_back(text(task.customer_name) | color(theme::kAccent));
            }
            if (being_edited) {
                meta.push_back(text("  ·  editing above") | color(theme::kWarning) | bold);
            }
            // The buttons sit on the (always short) meta line rather than after
            // the description: the list renders inside a `frame`, so a long
            // description would otherwise push them off the right edge.
            meta.push_back(text("   "));
            if (task.status != db::kStatusDone) {
                meta.push_back(steps_btn->Render());
                meta.push_back(text(" "));
            }
            meta.push_back(edit_btn->Render());
            meta.push_back(text(" "));
            meta.push_back(delete_btn->Render());

            return vbox({hbox({checkbox->Render()}), hbox(meta)});
        }));
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
    if (tasks_[index].id == editing_id_) cancel_edit();
    ctx_.repo->delete_task(tasks_[index].id);
    needs_refresh_ = true;
}

void TasksScreen::begin_edit(int index) {
    if (index < 0 || index >= static_cast<int>(tasks_.size())) return;
    const db::Task& task = tasks_[index];
    editing_id_ = task.id;
    form_text_ = task.description;
    form_due_ = task.due_date.empty() ? "" : util::iso_date_to_display(task.due_date);
    form_customer_ = task.customer_name;
    form_priority_ = index_from_priority(task.priority);
    status_message_ = "Editing task -- change the fields above and press Save.";
    needs_refresh_ = true;
}

void TasksScreen::cancel_edit() {
    editing_id_ = -1;
    clear_form();
    status_message_.clear();
    needs_refresh_ = true;
}

void TasksScreen::clear_form() {
    form_text_.clear();
    form_due_.clear();
    form_customer_.clear();
    form_priority_ = 1;
}

void TasksScreen::submit_form() {
    if (form_text_.empty()) {
        status_message_ = "Give the task a description first.";
        return;
    }

    std::string due_iso;
    if (!form_due_.empty()) {
        std::string typed = trim_lower(form_due_);
        if (typed == "today") {
            due_iso = util::today_iso();
        } else if (typed == "tomorrow" || typed == "tom") {
            due_iso = util::offset_days_iso(1);
        } else {
            due_iso = util::display_date_to_iso(form_due_);
        }
        if (due_iso.empty()) {
            status_message_ =
                "Couldn't parse due date '" + form_due_ + "' -- use DD MM YYYY, 'today', or 'tomorrow'.";
            return;
        }
    }

    std::optional<int64_t> customer_id;
    if (!form_customer_.empty()) customer_id = ctx_.repo->get_or_create_customer(form_customer_);

    if (editing_id_ >= 0) {
        ctx_.repo->update_task(editing_id_, form_text_, due_iso, priority_from_index(form_priority_), customer_id);
        editing_id_ = -1;
        status_message_ = "Task updated.";
    } else {
        ctx_.repo->add_task(form_text_, std::nullopt, customer_id, due_iso, "manual",
                             priority_from_index(form_priority_));
        status_message_.clear();
    }
    clear_form();
    needs_refresh_ = true;
}

void TasksScreen::build_ui() {
    using namespace ftxui;

    task_list_container_ = Container::Vertical({});

    InputOption desc_opt = styled_input_option();
    desc_opt.multiline = false;
    desc_opt.on_enter = [this] { submit_form(); };
    auto desc_input = WrappedInput(&form_text_, "Task description...", desc_opt);

    InputOption due_opt = styled_input_option();
    due_opt.multiline = false;
    due_opt.on_enter = [this] { submit_form(); };
    auto due_input = WrappedInput(&form_due_, "Due: today / DD MM YYYY", due_opt);

    InputOption customer_opt = styled_input_option();
    customer_opt.multiline = false;
    customer_opt.on_enter = [this] { submit_form(); };
    auto customer_input = WrappedInput(&form_customer_, "Customer (optional)", customer_opt);

    auto priority_toggle = Menu(&kPriorityLabels, &form_priority_, MenuOption::Horizontal());

    ButtonOption submit_opt;
    submit_opt.transform = [this](const EntryState& s) {
        EntryState labelled = s;
        labelled.label = editing_id_ >= 0 ? "Save changes" : "Add task";
        return button_style(theme::kAccent).transform(labelled);
    };
    auto submit_button = Button("Add Task", [this] { submit_form(); }, submit_opt);
    auto cancel_button = Button("Cancel", [this] { cancel_edit(); }, button_style(theme::kMuted));
    auto cancel_slot = Maybe(cancel_button, [this] { return editing_id_ >= 0; });

    CheckboxOption show_done_opt;
    show_done_opt.on_change = [this] { needs_refresh_ = true; };
    auto show_done_checkbox = Checkbox("Show completed", &show_completed_, show_done_opt);

    auto form_row = Container::Horizontal({desc_input, due_input, priority_toggle, customer_input, submit_button,
                                            cancel_slot});
    auto layout = Container::Vertical({form_row, show_done_checkbox, task_list_container_});

    breakdown_list_ = Container::Vertical({});
    auto add_steps_btn = Button("Add ticked steps", [this] { add_breakdown_steps(); },
                                button_style(theme::kSuccess));
    auto cancel_steps_btn = Button("Cancel", [this] { breakdown_visible_ = false; }, button_style(theme::kMuted));
    breakdown_first_ = add_steps_btn;
    auto breakdown_container =
        Container::Vertical({breakdown_list_, Container::Horizontal({add_steps_btn, cancel_steps_btn})});
    auto breakdown_modal = Renderer(breakdown_container, [this, add_steps_btn, cancel_steps_btn] {
                               return vbox({
                                          text("First steps") | bold | color(theme::kSuccess),
                                          paragraph(breakdown_task_.description) | dim,
                                          separator() | color(theme::kMuted),
                                          breakdown_list_->Render(),
                                          separator() | color(theme::kMuted),
                                          text("Untick any you don't want. They're added as tasks with the same "
                                               "customer and due date.") |
                                              dim,
                                          hbox({add_steps_btn->Render(), text("  "), cancel_steps_btn->Render()}) |
                                              hcenter,
                                      }) |
                                      size(WIDTH, EQUAL, 76) | borderStyled(ROUNDED, theme::kSuccess) |
                                      bgcolor(Color::Black) | clear_under | center;
                           }) |
                           CatchEvent([this](const Event& e) {
                               if (e != Event::Escape) return false;
                               breakdown_visible_ = false;
                               return true;
                           });

    root_ = Renderer(layout, [this, desc_input, due_input, priority_toggle, customer_input, submit_button, cancel_slot,
                               show_done_checkbox] {
        take_breakdown_result();
        if (needs_refresh_ || ctx_.repo->revision() != seen_revision_) {
            needs_refresh_ = false;
            refresh();
        }

        Elements children;

        Element form = vbox({
            hbox({
                desc_input->Render() | flex,
                text(" "),
                due_input->Render() | size(WIDTH, EQUAL, 34),
            }),
            hbox({
                text("Priority ") | color(Color::GrayLight) | vcenter,
                priority_toggle->Render() | vcenter,
                text("   "),
                customer_input->Render() | size(WIDTH, EQUAL, 30),
                filler(),
                cancel_slot->Render(),
                text(" "),
                submit_button->Render(),
            }),
        });
        children.push_back(section(editing_id_ >= 0 ? "Edit task" : "New task", form,
                                   editing_id_ >= 0 ? theme::kWarning : theme::kAccent));
        if (!status_message_.empty()) {
            children.push_back(paragraph(status_message_) |
                               color(editing_id_ >= 0 ? theme::kWarning : theme::kDanger));
        }

        Elements counts = {badge(std::to_string(open_count_) + " open", theme::kAccent)};
        if (due_today_count_ > 0) counts.push_back(badge(std::to_string(due_today_count_) + " due today", theme::kWarning));
        if (overdue_count_ > 0) counts.push_back(badge(std::to_string(overdue_count_) + " overdue", theme::kDanger));
        counts.push_back(filler());
        counts.push_back(show_done_checkbox->Render());
        children.push_back(hbox(std::move(counts)));
        children.push_back(task_list_container_->Render() | frame | flex | borderStyled(ROUNDED, theme::kMuted));

        return vbox(children) | flex;
    });
    root_ |= Modal(breakdown_modal, &breakdown_visible_);
}

}  // namespace callscribe::ui
