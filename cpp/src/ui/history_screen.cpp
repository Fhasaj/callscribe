#include "ui/history_screen.hpp"

#include <ftxui/dom/elements.hpp>

#include <algorithm>

#include "llm/summarizer.hpp"
#include "util/clipboard.hpp"
#include "util/date_format.hpp"

namespace callscribe::ui {

HistoryScreen::HistoryScreen(AppContext& ctx) : ctx_(ctx) {
    refresh_list();
    build_ui();
}

HistoryScreen::~HistoryScreen() {
    if (regen_thread_.joinable()) regen_thread_.join();
}

void HistoryScreen::refresh_list() {
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
    if (selected_ >= static_cast<int>(call_labels_.size())) {
        selected_ = std::max(0, static_cast<int>(call_labels_.size()) - 1);
    }
    load_detail();
}

void HistoryScreen::load_detail() {
    notes_ = (selected_ >= 0 && selected_ < static_cast<int>(calls_.size())) ? calls_[selected_].manual_notes : "";

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

void HistoryScreen::save_current_notes() {
    if (selected_ < 0 || selected_ >= static_cast<int>(calls_.size())) return;
    calls_[selected_].manual_notes = notes_;
    ctx_.repo->update_manual_notes(calls_[selected_].id, notes_);
}

void HistoryScreen::regenerate() {
    if (selected_ < 0 || selected_ >= static_cast<int>(calls_.size())) return;
    std::string model = ctx_.settings.ollama_model;
    if (model.empty()) {
        std::lock_guard<std::mutex> lock(detail_mutex_);
        status_message_ = "No Ollama model selected -- pick one in Settings.";
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
            auto result = llm::summarize_call(ctx_.settings.ollama_host, model, transcript, call.customer_name,
                                               call.meeting_name, call.participants,
                                               util::iso_date_to_display(call.started_at.substr(0, 10)));
            for (auto& t : result.tasks) ctx_.repo->add_task(t, call.id, call.customer_id, "", "call");
            ctx_.repo->add_note(call.id, result.summary_text, result.email_draft, "[]");
            std::lock_guard<std::mutex> lock(detail_mutex_);
            summary_ = result.summary_text;
            email_ = result.email_draft;
            status_message_ = "Done.";
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

    InputOption search_opt;
    search_opt.multiline = false;
    search_opt.on_enter = [this] { refresh_list(); };
    auto search_input = Input(&search_text_, "Search transcript or customer...", search_opt);
    auto search_button = Button("Search", [this] { refresh_list(); });

    MenuOption menu_opt = MenuOption::Vertical();
    menu_opt.on_change = [this] { load_detail(); };
    auto call_menu = Menu(&call_labels_, &selected_, menu_opt);

    auto regen_button = Button("Re-generate summary", [this] { regenerate(); });
    auto copy_summary_btn = Button("Copy Summary", [this] {
        std::lock_guard<std::mutex> lock(detail_mutex_);
        util::copy_to_clipboard(summary_);
    });
    auto copy_email_btn = Button("Copy Email", [this] {
        std::lock_guard<std::mutex> lock(detail_mutex_);
        util::copy_to_clipboard(email_);
    });

    auto left = Container::Vertical({
        Container::Horizontal({search_input, search_button}),
        call_menu,
    });

    auto transcript_page = Renderer([this] {
        std::string transcript_copy;
        {
            std::lock_guard<std::mutex> lock(detail_mutex_);
            transcript_copy = transcript_;
        }
        return frame(paragraph(transcript_copy.empty() ? "(select a call)" : transcript_copy)) | flex | border;
    });

    InputOption notes_opt;
    notes_opt.multiline = true;
    notes_opt.on_change = [this] { save_current_notes(); };
    auto notes_input = Input(&notes_, "Add notes for this call...", notes_opt);
    auto notes_page = Renderer(notes_input, [notes_input] { return notes_input->Render() | flex | border; });

    auto summary_page = Renderer(Container::Vertical({regen_button, copy_summary_btn}),
                                  [this, regen_button, copy_summary_btn] {
                                      std::string summary_copy, status_copy;
                                      bool regen_in_progress;
                                      {
                                          std::lock_guard<std::mutex> lock(detail_mutex_);
                                          summary_copy = summary_;
                                          status_copy = status_message_;
                                          regen_in_progress = regenerating_;
                                      }
                                      Elements children;
                                      children.push_back(paragraph(summary_copy.empty() ? "(no summary yet)"
                                                                                         : summary_copy) |
                                                          flex | border);
                                      children.push_back(hbox({regen_button->Render(), text(" "),
                                                                copy_summary_btn->Render()}));
                                      if (regen_in_progress)
                                          children.push_back(text("Working...") | color(Color(Color::Yellow)));
                                      if (!status_copy.empty()) children.push_back(text(status_copy) | dim);
                                      return vbox(children) | flex;
                                  });

    auto email_page = Renderer(copy_email_btn, [this, copy_email_btn] {
        std::string email_copy;
        {
            std::lock_guard<std::mutex> lock(detail_mutex_);
            email_copy = email_;
        }
        return vbox({
                   paragraph(email_copy.empty() ? "(no email draft yet)" : email_copy) | flex | border,
                   copy_email_btn->Render(),
               }) |
               flex;
    });

    auto inner_menu = Menu(&inner_tab_titles_, &inner_tab_index_, MenuOption::HorizontalAnimated());
    auto inner_tab_container =
        Container::Tab({transcript_page, notes_page, summary_page, email_page}, &inner_tab_index_);

    auto right = Container::Vertical({inner_menu, inner_tab_container});
    auto layout = Container::Horizontal({left, right});

    root_ = Renderer(layout, [this, search_input, search_button, call_menu, inner_menu, inner_tab_container] {
        Element left_pane = vbox({
                                hbox({search_input->Render() | flex, search_button->Render()}),
                                separator(),
                                call_menu->Render() | frame | flex,
                            }) |
                            flex | size(WIDTH, LESS_THAN, 50);

        Element right_pane = vbox({
                                 inner_menu->Render(),
                                 separator(),
                                 inner_tab_container->Render() | flex,
                             }) |
                             flex;

        return hbox({left_pane, separator(), right_pane}) | flex;
    });
}

}  // namespace callscribe::ui
