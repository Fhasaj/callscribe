#include "ui/summary_screen.hpp"

#include <ftxui/dom/elements.hpp>

#include <cstdio>

#include "llm/summarizer.hpp"
#include "util/clipboard.hpp"
#include "util/time.hpp"

namespace callscribe::ui {

namespace {
std::string format_hours(double hours) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", hours);
    return buf;
}
}  // namespace

SummaryScreen::SummaryScreen(AppContext& ctx) : ctx_(ctx) {
    year_text_ = std::to_string(util::current_year());
    build_ui();
    load_stats();
}

SummaryScreen::~SummaryScreen() {
    if (worker_.joinable()) worker_.join();
}

void SummaryScreen::load_stats() {
    int year;
    try {
        year = std::stoi(year_text_);
    } catch (...) {
        year = util::current_year();
    }
    stats_ = ctx_.repo->yearly_stats(year);
}

void SummaryScreen::generate_narrative() {
    std::string model = ctx_.settings.ollama_model;
    if (model.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_message_ = "No Ollama model selected -- pick one in Settings.";
        return;
    }
    if (stats_.summaries.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_message_ = "No call notes recorded for this year yet.";
        return;
    }
    int year;
    try {
        year = std::stoi(year_text_);
    } catch (...) {
        year = util::current_year();
    }

    if (worker_.joinable()) worker_.join();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        generating_ = true;
        status_message_ = "Generating...";
    }
    db::YearlyStats stats_copy = stats_;
    worker_ = std::thread([this, model, year, stats_copy] {
        std::string result_text, error_text;
        try {
            result_text = llm::generate_yearly_narrative(ctx_.settings.ollama_host, model, year, stats_copy);
        } catch (const std::exception& e) {
            error_text = e.what();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (error_text.empty()) {
            narrative_ = result_text;
            status_message_.clear();
        } else {
            status_message_ = "Failed: " + error_text;
        }
        generating_ = false;
    });
}

void SummaryScreen::build_ui() {
    using namespace ftxui;

    InputOption year_opt;
    year_opt.multiline = false;
    year_opt.on_enter = [this] { load_stats(); };
    auto year_input = Input(&year_text_, "Year", year_opt);
    auto load_button = Button("Load stats", [this] { load_stats(); });
    auto narrative_button = Button("Generate narrative recap (uses local LLM)", [this] { generate_narrative(); });
    auto copy_button = Button("Copy recap", [this] {
        std::lock_guard<std::mutex> lock(mutex_);
        util::copy_to_clipboard(narrative_);
    });

    auto layout = Container::Vertical({
        Container::Horizontal({year_input, load_button}),
        narrative_button,
        copy_button,
    });

    root_ = Renderer(layout, [this, year_input, load_button, narrative_button, copy_button] {
        std::string narrative_copy, status_copy;
        bool generating_now;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            narrative_copy = narrative_;
            status_copy = status_message_;
            generating_now = generating_;
        }

        double hours = stats_.total_seconds / 3600.0;
        std::string top_customers;
        for (size_t i = 0; i < stats_.top_customers.size() && i < 5; ++i) {
            if (i) top_customers += ", ";
            top_customers += stats_.top_customers[i].first + " (" + std::to_string(stats_.top_customers[i].second) +
                              ")";
        }
        if (top_customers.empty()) top_customers = "n/a";

        Elements children;
        children.push_back(text("CallScribe -- Yearly Summary") | bold);
        children.push_back(separator());
        children.push_back(hbox({
            text("Year: "),
            year_input->Render() | size(WIDTH, EQUAL, 8),
            text(" "),
            load_button->Render(),
        }));
        children.push_back(text("Calls: " + std::to_string(stats_.call_count) +
                                 "    Total call time: " + format_hours(hours) +
                                 "h    Tasks completed: " + std::to_string(stats_.tasks_completed)));
        children.push_back(text("Top customers: " + top_customers));
        children.push_back(separator());
        children.push_back(narrative_button->Render());
        if (generating_now) children.push_back(text("Working...") | color(Color(Color::Yellow)));
        if (!status_copy.empty()) children.push_back(text(status_copy) | dim);
        children.push_back(frame(paragraph(narrative_copy.empty() ? "(not generated yet)" : narrative_copy)) |
                            flex | border);
        children.push_back(copy_button->Render());

        return vbox(children) | flex;
    });
}

}  // namespace callscribe::ui
