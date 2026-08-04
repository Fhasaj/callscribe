#include "ui/summary_screen.hpp"

#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <cstdio>

#include "llm/summarizer.hpp"
#include "ui/scroll_pane.hpp"
#include "ui/styled_input.hpp"
#include "ui/theme.hpp"
#include "ui/wrapped_input.hpp"
#include "util/clipboard.hpp"
#include "util/time.hpp"

namespace callscribe::ui {

namespace {
std::string format_hours(double hours) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", hours);
    return buf;
}

// The review questions, split by cycle. Kept verbatim so the generated report
// echoes back the exact wording the review form asks for.
const std::vector<std::string>& semi_annual_questions() {
    static const std::vector<std::string> q = {
        "Since your formal annual review, what are some updates you have on current goals?",
        "Since your annual review, what updates do you have on any follow-up items from the previous review?",
    };
    return q;
}

const std::vector<std::string>& annual_questions() {
    static const std::vector<std::string> q = {
        "What are some things I do well?",
        "How could I improve?",
        "What are some of my top accomplishments during this review cycle?",
    };
    return q;
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

void SummaryScreen::generate_review_report() {
    bool annual = cycle_index_ == 1;
    const auto& all = annual ? annual_questions() : semi_annual_questions();
    const bool* selected = annual ? annual_selected_ : semi_selected_;

    std::vector<std::string> questions;
    for (size_t i = 0; i < all.size(); ++i) {
        if (selected[i]) questions.push_back(all[i]);
    }
    if (questions.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_message_ = "Pick at least one question to answer.";
        return;
    }

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
    std::string cycle_label = annual ? "annual" : "semi-annual";
    db::YearlyStats stats_copy = stats_;
    worker_ = std::thread([this, model, year, cycle_label, questions, stats_copy] {
        std::string result_text, error_text;
        try {
            result_text =
                llm::generate_review_report(ctx_.settings.ollama_host, model, year, cycle_label, questions, stats_copy);
        } catch (const std::exception& e) {
            error_text = e.what();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (error_text.empty()) {
            report_ = result_text;
            status_message_.clear();
        } else {
            status_message_ = "Failed: " + error_text;
        }
        generating_ = false;
    });
}

void SummaryScreen::build_ui() {
    using namespace ftxui;

    InputOption year_opt = styled_input_option();
    year_opt.multiline = false;
    year_opt.on_enter = [this] { load_stats(); };
    auto year_input = WrappedInput(&year_text_, "Year", year_opt);
    auto load_button = Button("Load stats", [this] { load_stats(); });
    auto report_button = Button(
        "Generate review report (uses local LLM)",
        [this] {
            wizard_step_ = 0;
            wizard_visible_ = true;
        },
        ButtonOption::Animated(theme::kAccent));
    auto copy_button = Button("Copy report", [this] {
        std::lock_guard<std::mutex> lock(mutex_);
        util::copy_to_clipboard(report_);
    });

    auto report_pane = ScrollableText(
        [this] {
            std::lock_guard<std::mutex> lock(mutex_);
            return report_;
        },
        "(not generated yet)");

    auto layout = Container::Vertical({
        Container::Horizontal({year_input, load_button}),
        report_button,
        report_pane,
        copy_button,
    });

    // --- Review report wizard -------------------------------------------
    // Step 0: which review cycle. Step 1: which of that cycle's questions to
    // answer. Both steps live in one modal so Back/Next just flip wizard_step_.
    static const std::vector<std::string> cycle_labels = {"Semi-annual review", "Annual review"};
    auto cycle_radio = Radiobox(&cycle_labels, &cycle_index_);

    auto semi_checks = Container::Vertical({});
    for (int i = 0; i < kSemiQuestionCount; ++i)
        semi_checks->Add(Checkbox(semi_annual_questions()[static_cast<size_t>(i)], &semi_selected_[i]));

    auto annual_checks = Container::Vertical({});
    for (int i = 0; i < kAnnualQuestionCount; ++i)
        annual_checks->Add(Checkbox(annual_questions()[static_cast<size_t>(i)], &annual_selected_[i]));

    // Only the checkbox list matching the chosen cycle is active/rendered.
    auto question_checks = Container::Tab({semi_checks, annual_checks}, &cycle_index_);

    auto wizard_next = Button(
        "Next >", [this] { wizard_step_ = 1; }, ButtonOption::Animated(theme::kAccent));
    auto wizard_back = Button("< Back", [this] { wizard_step_ = 0; });
    auto wizard_generate = Button(
        "Generate",
        [this] {
            wizard_visible_ = false;
            generate_review_report();
        },
        ButtonOption::Animated(theme::kSuccess));
    auto wizard_cancel = Button("Cancel", [this] { wizard_visible_ = false; });

    auto step0_buttons = Container::Horizontal({wizard_next, wizard_cancel});
    auto step1_buttons = Container::Horizontal({wizard_generate, wizard_back, wizard_cancel});
    auto step0 = Container::Vertical({cycle_radio, step0_buttons});
    auto step1 = Container::Vertical({question_checks, step1_buttons});
    auto wizard_container = Container::Tab({step0, step1}, &wizard_step_);

    auto wizard = Renderer(wizard_container, [this, cycle_radio, question_checks, wizard_next, wizard_back,
                                              wizard_generate, wizard_cancel] {
        Elements body;
        body.push_back(text("Review report wizard") | bold | color(theme::kAccent));
        body.push_back(separator() | color(theme::kMuted));
        if (wizard_step_ == 0) {
            body.push_back(text("Step 1 of 2 -- which review is this for?"));
            body.push_back(cycle_radio->Render());
            body.push_back(separator() | color(theme::kMuted));
            body.push_back(hbox({wizard_next->Render(), text("  "), wizard_cancel->Render()}) | hcenter);
        } else {
            body.push_back(text("Step 2 of 2 -- which questions should it answer?"));
            body.push_back(question_checks->Render());
            body.push_back(separator() | color(theme::kMuted));
            body.push_back(hbox({wizard_generate->Render(), text("  "), wizard_back->Render(), text("  "),
                                  wizard_cancel->Render()}) |
                           hcenter);
        }
        return vbox(body) | size(WIDTH, EQUAL, 76) | borderStyled(ROUNDED, theme::kAccent) | bgcolor(Color::Black) |
               clear_under | center;
    });

    root_ = Renderer(layout, [this, year_input, load_button, report_button, report_pane, copy_button] {
        std::string status_copy;
        bool generating_now;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            status_copy = status_message_;
            generating_now = generating_;
        }

        double hours = stats_.total_seconds / 3600.0;

        Elements children;
        children.push_back(text("CallScribe -- Yearly Summary") | bold | color(theme::kAccent));
        children.push_back(separator() | color(theme::kMuted));
        children.push_back(hbox({
            text("Year: "),
            year_input->Render() | size(WIDTH, EQUAL, 8),
            text(" "),
            load_button->Render(),
        }));
        children.push_back(text("Calls: " + std::to_string(stats_.call_count) +
                                 "    Total call time: " + format_hours(hours) +
                                 "h    Tasks completed: " + std::to_string(stats_.tasks_completed)));

        children.push_back(separator() | color(theme::kMuted));
        children.push_back(text("Calls per month:") | bold | color(theme::kAccent));
        children.push_back(graph([this](int width, int height) {
                                std::vector<int> out(static_cast<size_t>(width), 0);
                                int max_count = 0;
                                for (int c : stats_.monthly_call_counts) max_count = std::max(max_count, c);
                                if (max_count == 0 || width <= 0) return out;
                                for (int i = 0; i < width; ++i) {
                                    int month = std::min(11, i * 12 / width);
                                    out[static_cast<size_t>(i)] =
                                        stats_.monthly_call_counts[static_cast<size_t>(month)] * height / max_count;
                                }
                                return out;
                            }) |
                            color(theme::kAccent) | size(HEIGHT, EQUAL, 6) | borderStyled(ROUNDED, theme::kMuted));

        children.push_back(separator() | color(theme::kMuted));
        children.push_back(text("Top customers:") | bold | color(theme::kAccent));
        {
            int max_customer = 0;
            for (auto& c : stats_.top_customers) max_customer = std::max(max_customer, c.second);
            Elements customer_rows;
            for (size_t i = 0; i < stats_.top_customers.size() && i < 5; ++i) {
                const auto& [name, count] = stats_.top_customers[i];
                float progress = max_customer > 0 ? static_cast<float>(count) / static_cast<float>(max_customer) : 0.0f;
                customer_rows.push_back(hbox({
                    text(name) | size(WIDTH, EQUAL, 22),
                    gauge(progress) | color(theme::kAccent) | flex,
                    text("  " + std::to_string(count)),
                }));
            }
            if (customer_rows.empty()) customer_rows.push_back(text("n/a") | dim);
            children.push_back(vbox(customer_rows));
        }
        children.push_back(separator() | color(theme::kMuted));
        children.push_back(report_button->Render());
        if (generating_now) children.push_back(text("Working...") | color(theme::kWarning));
        if (!status_copy.empty()) children.push_back(text(status_copy) | dim);
        children.push_back(report_pane->Render() | flex);
        children.push_back(copy_button->Render());

        return vbox(children) | flex;
    });

    root_ |= Modal(wizard, &wizard_visible_);
}

}  // namespace callscribe::ui
