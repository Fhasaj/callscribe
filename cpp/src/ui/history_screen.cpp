#include "ui/history_screen.hpp"

#include <ftxui/dom/elements.hpp>

#include <algorithm>

#include "llm/summarizer.hpp"
#include "util/clipboard.hpp"

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
        std::string when = c.started_at.size() >= 16 ? c.started_at.substr(0, 16) : c.started_at;
        for (auto& ch : when)
            if (ch == 'T') ch = ' ';
        call_labels_.push_back(when + " -- " + (c.customer_name.empty() ? "Unknown" : c.customer_name) + " (" +
                                c.status + ")");
    }
    if (selected_ >= static_cast<int>(call_labels_.size())) {
        selected_ = std::max(0, static_cast<int>(call_labels_.size()) - 1);
    }
    load_detail();
}

void HistoryScreen::load_detail() {
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
                                               call.started_at.substr(0, 10));
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
    auto right = Container::Vertical({regen_button, copy_summary_btn, copy_email_btn});
    auto layout = Container::Horizontal({left, right});

    root_ = Renderer(layout, [this, search_input, search_button, call_menu, regen_button, copy_summary_btn,
                               copy_email_btn] {
        std::string transcript_copy, summary_copy, email_copy, status_copy;
        bool regen_in_progress;
        {
            std::lock_guard<std::mutex> lock(detail_mutex_);
            transcript_copy = transcript_;
            summary_copy = summary_;
            email_copy = email_;
            status_copy = status_message_;
            regen_in_progress = regenerating_;
        }

        Element left_pane = vbox({
                                hbox({search_input->Render() | flex, search_button->Render()}),
                                separator(),
                                call_menu->Render() | frame | flex,
                            }) |
                            flex | size(WIDTH, LESS_THAN, 50);

        Elements right_children;
        right_children.push_back(text("Transcript:") | bold);
        right_children.push_back(frame(paragraph(transcript_copy.empty() ? "(select a call)" : transcript_copy)) |
                                  flex | border);
        right_children.push_back(text("Summary:") | bold);
        right_children.push_back(paragraph(summary_copy) | border);
        right_children.push_back(text("Email draft:") | bold);
        right_children.push_back(paragraph(email_copy) | border);
        right_children.push_back(hbox({
            regen_button->Render(),
            text(" "),
            copy_summary_btn->Render(),
            text(" "),
            copy_email_btn->Render(),
        }));
        if (regen_in_progress) right_children.push_back(text("Working...") | color(Color(Color::Yellow)));
        if (!status_copy.empty()) right_children.push_back(text(status_copy) | dim);

        Element right_pane = vbox(right_children) | flex;

        return hbox({left_pane, separator(), right_pane}) | flex;
    });
}

}  // namespace callscribe::ui
