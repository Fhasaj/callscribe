#include "ui/history_screen.hpp"

#include <ftxui/dom/elements.hpp>

#include <algorithm>

#include "llm/summarizer.hpp"
#include "ui/scroll_pane.hpp"
#include "ui/styled_input.hpp"
#include "ui/task_meta.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"
#include "ui/wrapped_input.hpp"
#include "util/clipboard.hpp"
#include "util/date_format.hpp"

namespace callscribe::ui {

HistoryScreen::HistoryScreen(AppContext& ctx)
    : ctx_(ctx), proposed_tasks_(ctx, [this] {
          return (selected_ >= 0 && selected_ < static_cast<int>(calls_.size())) ? calls_[selected_].id : -1;
      }) {
    refresh_list();
    build_ui();
}

HistoryScreen::~HistoryScreen() {
    if (regen_thread_.joinable()) regen_thread_.join();
}

void HistoryScreen::refresh_list() {
    // Reloading must not move the user off the call they were reading, so the
    // selection is restored by id rather than by position (a new call lands at
    // the top of the list and shifts every index below it).
    int64_t previously_selected =
        (selected_ >= 0 && selected_ < static_cast<int>(calls_.size())) ? calls_[selected_].id : -1;

    seen_revision_ = ctx_.repo->revision();
    calls_ = ctx_.repo->list_calls("", search_text_);
    call_labels_.clear();
    for (auto& c : calls_) {
        std::string when = c.started_at;
        if (c.started_at.size() >= 16) {
            std::string date_part = util::iso_date_to_display(c.started_at.substr(0, 10));
            std::string time_part = c.started_at.substr(11, 5);
            when = date_part + " " + time_part;
        }
        call_labels_.push_back(when + " -- " + (c.customer_name.empty() ? "Unknown" : c.customer_name) + " (" +
                                c.status + ")");
    }
    if (previously_selected >= 0) {
        for (size_t i = 0; i < calls_.size(); ++i) {
            if (calls_[i].id != previously_selected) continue;
            selected_ = static_cast<int>(i);
            break;
        }
    }
    if (selected_ >= static_cast<int>(call_labels_.size())) {
        selected_ = std::max(0, static_cast<int>(call_labels_.size()) - 1);
    }
    load_detail();
}

void HistoryScreen::load_detail() {
    notes_ = (selected_ >= 0 && selected_ < static_cast<int>(calls_.size())) ? calls_[selected_].manual_notes : "";
    linked_tasks_dirty_ = true;

    std::lock_guard<std::mutex> lock(detail_mutex_);
    if (selected_ < 0 || selected_ >= static_cast<int>(calls_.size())) {
        transcript_.clear();
        summary_.clear();
        email_.clear();
        return;
    }
    const auto& call = calls_[selected_];
    transcript_ = ctx_.repo->render_transcript(call.id);
    auto note = ctx_.repo->get_note_for_call(call.id);
    summary_ = note ? note->summary : "";
    email_ = note ? note->email_draft : "";
}

void HistoryScreen::refresh_linked_tasks() {
    linked_tasks_.clear();
    if (selected_ >= 0 && selected_ < static_cast<int>(calls_.size())) {
        // Proposed ones belong to the review panel, not this list.
        for (auto& task : ctx_.repo->list_tasks_for_call(calls_[selected_].id))
            if (task.status != db::kStatusProposed) linked_tasks_.push_back(std::move(task));
    }
    linked_tasks_dirty_ = false;
}

void HistoryScreen::add_task_for_selected() {
    if (new_task_text_.empty() || selected_ < 0 || selected_ >= static_cast<int>(calls_.size())) return;
    const auto& call = calls_[selected_];
    ctx_.repo->add_task(new_task_text_, call.id, call.customer_id, "", "manual");
    new_task_text_.clear();
    linked_tasks_dirty_ = true;
}

void HistoryScreen::save_current_notes() {
    if (selected_ < 0 || selected_ >= static_cast<int>(calls_.size())) return;
    calls_[selected_].manual_notes = notes_;
    ctx_.repo->update_manual_notes(calls_[selected_].id, notes_);
}

void HistoryScreen::regenerate() {
    if (selected_ < 0 || selected_ >= static_cast<int>(calls_.size())) return;
    std::string model = ctx_.settings.llm_model();
    if (model.empty()) {
        std::lock_guard<std::mutex> lock(detail_mutex_);
        status_message_ = "No AI model selected -- pick one in Settings.";
        return;
    }
    if (regen_thread_.joinable()) regen_thread_.join();
    db::Call call = calls_[selected_];
    {
        std::lock_guard<std::mutex> lock(detail_mutex_);
        regenerating_ = true;
        status_message_ = "Regenerating...";
    }
    regen_thread_ = std::thread([this, call, model] {
        std::string transcript = ctx_.repo->render_transcript(call.id);
        try {
            auto result = llm::summarize_call(ctx_.settings.llm_host(), model, transcript, call.customer_name,
                                               call.meeting_name, call.participants,
                                               util::iso_date_to_display(call.started_at.substr(0, 10)),
                                               [this](const std::string& phase) {
                                                   std::lock_guard<std::mutex> lock(detail_mutex_);
                                                   status_message_ = phase;
                                               },
                                               llm::PromptPresets{ctx_.settings.summary_prompt,
                                                                  ctx_.settings.email_prompt,
                                                                  ctx_.settings.tasks_prompt},
                                           ctx_.settings.user_display_name);
            for (auto& t : result.tasks) {
                std::optional<int64_t> task_customer = call.customer_id;
                if (!t.customer.empty() && t.customer != call.customer_name)
                    task_customer = ctx_.repo->get_or_create_customer(t.customer);
                // Proposed, not open -- see the review panel under the summary.
                ctx_.repo->add_task(t.description, call.id, task_customer, t.due_date, "call", t.priority,
                                     db::kStatusProposed);
            }
            linked_tasks_dirty_ = true;
            ctx_.repo->add_note(call.id, result.summary_text, result.email_draft, "[]");
            std::lock_guard<std::mutex> lock(detail_mutex_);
            summary_ = result.summary_text;
            email_ = result.email_draft;
            // Don't report plain success behind an empty pane -- say why it's blank.
            status_message_ = result.summary_text.empty()
                                  ? "Done, but the model returned no summary points for this call."
                                  : "Done.";
            regenerating_ = false;
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(detail_mutex_);
            status_message_ = std::string("Failed: ") + e.what();
            regenerating_ = false;
        }
    });
}

void HistoryScreen::build_ui() {
    using namespace ftxui;

    InputOption search_opt = styled_input_option();
    search_opt.multiline = false;
    search_opt.on_enter = [this] { refresh_list(); };
    auto search_input = WrappedInput(&search_text_, "Search transcript or customer...", search_opt);
    auto search_button = Button("Search", [this] { refresh_list(); }, button_style(theme::kAccent));

    MenuOption menu_opt = MenuOption::Vertical();
    menu_opt.on_change = [this] { load_detail(); };
    auto call_menu = Menu(&call_labels_, &selected_, menu_opt);

    auto regen_button =
        Button("Re-generate summary", [this] { regenerate(); }, button_style(theme::kAccent));
    auto copy_transcript_btn = Button("Copy Transcript", [this] {
        std::lock_guard<std::mutex> lock(detail_mutex_);
        util::copy_to_clipboard(transcript_);
    }, button_style(theme::kMuted));
    auto copy_summary_btn = Button("Copy Summary", [this] {
        std::lock_guard<std::mutex> lock(detail_mutex_);
        util::copy_to_clipboard(summary_);
    }, button_style(theme::kMuted));
    auto copy_email_btn = Button("Copy Email", [this] {
        std::lock_guard<std::mutex> lock(detail_mutex_);
        util::copy_to_clipboard(email_);
    }, button_style(theme::kMuted));

    auto left = Container::Vertical({
        Container::Horizontal({search_input, search_button}),
        call_menu,
    });

    auto transcript_pane = ScrollableText(
        [this] {
            std::lock_guard<std::mutex> lock(detail_mutex_);
            return transcript_;
        },
        "(select a call)");
    auto transcript_page =
        Renderer(Container::Vertical({transcript_pane, copy_transcript_btn}),
                 [transcript_pane, copy_transcript_btn] {
                     return vbox({transcript_pane->Render() | flex, hbox({filler(), copy_transcript_btn->Render()})}) | flex;
                 });

    InputOption notes_opt;
    notes_opt.multiline = true;
    notes_opt.on_change = [this] { save_current_notes(); };
    auto notes_input = WrappedInput(&notes_, "Add notes for this call...", notes_opt);
    // An editable Input consumes clicks to place its cursor, so its text can't
    // be drag-selected the way the read-only panes can -- hence a copy button.
    auto copy_notes_btn = Button("Copy Notes", [this] {
        std::lock_guard<std::mutex> lock(detail_mutex_);
        util::copy_to_clipboard(notes_);
    }, button_style(theme::kMuted));
    auto notes_page = Renderer(Container::Vertical({notes_input, copy_notes_btn}),
                               [notes_input, copy_notes_btn] {
                                   return vbox({
                                              notes_input->Render() | flex | borderStyled(ROUNDED, theme::kMuted),
                                              hbox({filler(), copy_notes_btn->Render()}),
                                          }) |
                                          flex;
                               });

    InputOption new_task_opt = styled_input_option();
    new_task_opt.multiline = false;
    new_task_opt.on_enter = [this] { add_task_for_selected(); };
    auto new_task_input = WrappedInput(&new_task_text_, "Add a task for this call...", new_task_opt);
    auto add_task_btn =
        Button("Add Task", [this] { add_task_for_selected(); }, button_style(theme::kAccent));

    auto summary_pane = ScrollableText(
        [this] {
            std::lock_guard<std::mutex> lock(detail_mutex_);
            return summary_;
        },
        "(no summary yet)");

    auto proposed_panel = proposed_tasks_.component();
    auto summary_page =
        Renderer(Container::Vertical({summary_pane, regen_button, copy_summary_btn, proposed_panel, new_task_input,
                                       add_task_btn}),
                 [this, summary_pane, regen_button, copy_summary_btn, proposed_panel, new_task_input, add_task_btn] {
                     if (linked_tasks_dirty_.load()) refresh_linked_tasks();

                     std::string status_copy;
                     bool regen_in_progress;
                     {
                         std::lock_guard<std::mutex> lock(detail_mutex_);
                         status_copy = status_message_;
                         regen_in_progress = regenerating_;
                     }
                     Elements children;
                     children.push_back(summary_pane->Render() | flex);
                     children.push_back(
                         hbox({regen_button->Render(), text(" "), copy_summary_btn->Render()}));
                     if (regen_in_progress) children.push_back(text("Working...") | color(theme::kWarning));
                     if (!status_copy.empty()) children.push_back(text(status_copy) | dim);
                     if (!proposed_tasks_.empty()) {
                         children.push_back(separator() | color(theme::kMuted));
                         children.push_back(proposed_panel->Render());
                     }

                     Elements task_lines;
                     for (auto& t : linked_tasks_) {
                         bool done = t.status == db::kStatusDone;
                         std::string line = (done ? "[x] " : "[ ] ") + t.description;
                         if (t.source == "call") line += "  [from summary]";
                         Element e = text(line);
                         if (done) e |= color(theme::kMuted);
                         task_lines.push_back(e);
                         task_lines.push_back(text("    " + task_meta_line(t)) | color(theme::kMuted));
                     }
                     if (task_lines.empty()) task_lines.push_back(text("(no tasks linked to this call yet)") | dim);

                     children.push_back(separator() | color(theme::kMuted));
                     children.push_back(text("Tasks for this call") | bold | color(theme::kAccent));
                     children.push_back(vbox(task_lines));
                     children.push_back(
                         hbox({new_task_input->Render() | flex, text(" "), add_task_btn->Render()}));

                     return vbox(children) | flex;
                 });

    auto email_pane = ScrollableText(
        [this] {
            std::lock_guard<std::mutex> lock(detail_mutex_);
            return email_;
        },
        "(no email draft yet)");
    auto email_page = Renderer(Container::Vertical({email_pane, copy_email_btn}), [email_pane, copy_email_btn] {
        return vbox({
                   email_pane->Render() | flex,
                   hbox({filler(), copy_email_btn->Render()}),
               }) |
               flex;
    });

    auto inner_menu = Menu(&inner_tab_titles_, &inner_tab_index_, MenuOption::HorizontalAnimated());
    auto inner_tab_container =
        Container::Tab({transcript_page, notes_page, summary_page, email_page}, &inner_tab_index_);

    auto right = Container::Vertical({inner_menu, inner_tab_container});
    auto layout = Container::Horizontal({left, right});

    root_ = Renderer(layout, [this, search_input, search_button, call_menu, inner_menu, inner_tab_container] {
        // Picks up calls (and summaries) written by another screen -- notably
        // the call the Record tab just finished processing.
        if (ctx_.repo->revision() != seen_revision_) refresh_list();

        Element left_pane = vbox({
                                hbox({search_input->Render() | flex, text(" "), search_button->Render()}),
                                section("Calls", call_menu->Render() | frame | flex) | flex,
                            }) |
                            size(WIDTH, EQUAL, 48);

        Element right_pane = vbox({
                                 inner_menu->Render(),
                                 inner_tab_container->Render() | flex,
                             }) |
                             flex;

        return hbox({left_pane, text(" "), right_pane}) | flex;
    });
}

}  // namespace callscribe::ui
