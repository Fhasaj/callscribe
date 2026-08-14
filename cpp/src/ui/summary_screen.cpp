#include "ui/summary_screen.hpp"

#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <cstdio>

#include "config.hpp"
#include "llm/summarizer.hpp"
#include "ui/scroll_pane.hpp"
#include "ui/styled_input.hpp"
#include "ui/theme.hpp"
#include "ui/wrapped_input.hpp"
#include "util/clipboard.hpp"
#include "util/pdf_writer.hpp"
#include "util/time.hpp"

namespace callscribe::ui {

namespace {
std::string format_hours(double hours) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", hours);
    return buf;
}

std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}
}  // namespace

SummaryScreen::SummaryScreen(AppContext& ctx) : ctx_(ctx) {
    year_text_ = std::to_string(util::current_year());
    kpis_ = ctx_.settings.report_kpis;
    build_ui();
    load_stats();
}

SummaryScreen::~SummaryScreen() {
    if (worker_.joinable()) worker_.join();
}

int SummaryScreen::selected_year() {
    try {
        return std::stoi(year_text_);
    } catch (...) {
        return util::current_year();
    }
}

void SummaryScreen::load_stats() { stats_ = ctx_.repo->yearly_stats(selected_year()); }

void SummaryScreen::add_kpi() {
    std::string kpi = trim(kpi_input_);
    if (kpi.empty()) return;
    if (std::find(kpis_.begin(), kpis_.end(), kpi) == kpis_.end()) kpis_.push_back(kpi);
    kpi_input_.clear();
    rebuild_kpi_rows();
    save_kpis();
}

void SummaryScreen::save_kpis() {
    ctx_.settings.report_kpis = kpis_;
    ctx_.save_settings();
}

void SummaryScreen::rebuild_kpi_rows() {
    using namespace ftxui;
    kpi_rows_->DetachAllChildren();
    for (size_t i = 0; i < kpis_.size(); ++i) {
        int index = static_cast<int>(i);
        // Removal is deferred to the next render: tearing a child out of the
        // container from inside its own callback would pull the ground out
        // from under the event still being dispatched.
        auto remove = Button(
            "Remove", [this, index] { pending_remove_ = index; }, ButtonOption::Ascii());
        std::string label = kpis_[i];
        kpi_rows_->Add(Renderer(remove, [remove, label] {
            return hbox({text("- "), text(label) | flex, text(" "), remove->Render() | color(theme::kDanger)});
        }));
    }
}

void SummaryScreen::generate_report() {
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

    if (worker_.joinable()) worker_.join();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        generating_ = true;
        status_message_ = "Generating...";
    }
    int year = selected_year();
    std::string cycle_label = cycle_index_ == 1 ? "annual" : "semi-annual";
    std::vector<std::string> kpis = kpis_;
    db::YearlyStats stats_copy = stats_;
    worker_ = std::thread([this, model, year, cycle_label, kpis, stats_copy] {
        llm::KpiReport result;
        std::string error_text;
        try {
            result = llm::generate_kpi_report(ctx_.settings.ollama_host, model, year, cycle_label, kpis, stats_copy);
        } catch (const std::exception& e) {
            error_text = e.what();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (error_text.empty()) {
            report_ = result.report_text;
            email_subject_ = result.email_subject;
            email_body_ = result.email_body;
            status_message_.clear();
        } else {
            status_message_ = "Failed: " + error_text;
        }
        generating_ = false;
    });
}

void SummaryScreen::copy_current_tab() {
    std::string text_to_copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (output_tab_ == 1) {
            if (!email_body_.empty()) text_to_copy = "Subject: " + email_subject_ + "\n\n" + email_body_;
        } else {
            text_to_copy = report_;
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (text_to_copy.empty()) {
        status_message_ = "Nothing to copy yet -- generate a report first.";
        return;
    }
    status_message_ = util::copy_to_clipboard(text_to_copy)
                          ? (output_tab_ == 1 ? "Email copied to clipboard." : "Report copied to clipboard.")
                          : "No clipboard tool found (install wl-clipboard or xclip).";
}

void SummaryScreen::export_pdf() {
    std::string body, subject;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        body = output_tab_ == 1 ? email_body_ : report_;
        subject = email_subject_;
    }
    if (body.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_message_ = "Nothing to export yet -- generate a report first.";
        return;
    }

    int year = selected_year();
    bool annual = cycle_index_ == 1;
    bool email_version = output_tab_ == 1;
    std::string title = std::to_string(year) + (annual ? " Annual" : " Semi-annual") + " Performance Report";
    std::string subtitle = ctx_.settings.user_display_name;
    if (email_version) {
        title = subject.empty() ? title : subject;
        subtitle = subtitle.empty() ? "" : "From: " + subtitle;
    }

    std::filesystem::path path = data_dir() / "reports" /
                                  ("callscribe-report-" + std::to_string(year) +
                                   (annual ? "-annual" : "-semi-annual") + (email_version ? "-email" : "") + ".pdf");

    bool ok = util::write_text_pdf(path, title, subtitle, body);
    std::lock_guard<std::mutex> lock(mutex_);
    status_message_ = ok ? "Saved PDF to " + path.string() : "Could not write " + path.string();
}

void SummaryScreen::build_ui() {
    using namespace ftxui;

    InputOption year_opt = styled_input_option();
    year_opt.multiline = false;
    year_opt.on_enter = [this] { load_stats(); };
    auto year_input = WrappedInput(&year_text_, "Year", year_opt);
    auto load_button = Button("Load stats", [this] { load_stats(); });
    auto report_button = Button(
        "Generate report (uses local LLM)",
        [this] {
            wizard_step_ = 0;
            wizard_visible_ = true;
        },
        ButtonOption::Animated(theme::kAccent));
    auto copy_button = Button("Copy", [this] { copy_current_tab(); });
    auto pdf_button = Button(
        "Export PDF", [this] { export_pdf(); }, ButtonOption::Animated(theme::kSuccess));

    auto output_menu = Menu(&output_tab_titles_, &output_tab_, MenuOption::HorizontalAnimated());

    auto report_pane = ScrollableText(
        [this] {
            std::lock_guard<std::mutex> lock(mutex_);
            return report_;
        },
        "(not generated yet)");
    auto email_pane = ScrollableText(
        [this] {
            std::lock_guard<std::mutex> lock(mutex_);
            if (email_body_.empty()) return std::string();
            return "Subject: " + email_subject_ + "\n\n" + email_body_;
        },
        "(not generated yet)");
    auto output_panes = Container::Tab({report_pane, email_pane}, &output_tab_);

    auto layout = Container::Vertical({
        Container::Horizontal({year_input, load_button}),
        report_button,
        output_menu,
        output_panes,
        Container::Horizontal({copy_button, pdf_button}),
    });

    // --- Report wizard ----------------------------------------------------
    // Step 0: which review cycle. Step 1: the KPIs to report against. Both
    // steps live in one modal so Back/Next just flip wizard_step_.
    static const std::vector<std::string> cycle_labels = {"Semi-annual report", "Annual report"};
    auto cycle_radio = Radiobox(&cycle_labels, &cycle_index_);

    kpi_rows_ = Container::Vertical({});
    rebuild_kpi_rows();

    InputOption kpi_opt = styled_input_option();
    kpi_opt.multiline = false;
    kpi_opt.on_enter = [this] { add_kpi(); };
    auto kpi_input = WrappedInput(&kpi_input_, "e.g. Renewals secured", kpi_opt);
    auto kpi_add = Button(
        "Add KPI", [this] { add_kpi(); }, ButtonOption::Animated(theme::kAccent));
    auto kpi_reset = Button("Reset to defaults", [this] {
        kpis_ = default_report_kpis();
        rebuild_kpi_rows();
        save_kpis();
    });

    auto wizard_next = Button(
        "Next >", [this] { wizard_step_ = 1; }, ButtonOption::Animated(theme::kAccent));
    auto wizard_back = Button("< Back", [this] { wizard_step_ = 0; });
    auto wizard_generate = Button(
        "Generate",
        [this] {
            wizard_visible_ = false;
            save_kpis();
            generate_report();
        },
        ButtonOption::Animated(theme::kSuccess));
    auto wizard_cancel = Button("Cancel", [this] { wizard_visible_ = false; });

    auto step0_buttons = Container::Horizontal({wizard_next, wizard_cancel});
    auto step1_buttons = Container::Horizontal({wizard_generate, wizard_back, wizard_cancel});
    auto kpi_entry = Container::Horizontal({kpi_input, kpi_add, kpi_reset});
    auto step0 = Container::Vertical({cycle_radio, step0_buttons});
    auto step1 = Container::Vertical({kpi_entry, kpi_rows_, step1_buttons});
    auto wizard_container = Container::Tab({step0, step1}, &wizard_step_);

    auto wizard = Renderer(wizard_container, [this, cycle_radio, kpi_input, kpi_add, kpi_reset, wizard_next,
                                              wizard_back, wizard_generate, wizard_cancel] {
        // A Remove button clicked during the last event pass takes effect
        // here, before anything is drawn.
        if (pending_remove_ >= 0) {
            if (pending_remove_ < static_cast<int>(kpis_.size())) {
                kpis_.erase(kpis_.begin() + pending_remove_);
                rebuild_kpi_rows();
                save_kpis();
            }
            pending_remove_ = -1;
        }

        Elements body;
        body.push_back(text("Report wizard") | bold | color(theme::kAccent));
        body.push_back(separator() | color(theme::kMuted));
        if (wizard_step_ == 0) {
            body.push_back(text("Step 1 of 2 -- which reporting cycle is this for?"));
            body.push_back(cycle_radio->Render());
            body.push_back(separator() | color(theme::kMuted));
            body.push_back(hbox({wizard_next->Render(), text("  "), wizard_cancel->Render()}) | hcenter);
        } else {
            body.push_back(text("Step 2 of 2 -- which KPIs should the report track?"));
            body.push_back(text("The report covers these, in this order, using your call history as "
                                 "the evidence.") |
                           dim);
            body.push_back(hbox({
                kpi_input->Render() | flex,
                text(" "),
                kpi_add->Render(),
                text(" "),
                kpi_reset->Render(),
            }));
            if (kpis_.empty()) {
                body.push_back(text("No KPIs yet -- add one above, or generate a general report.") | dim);
            } else {
                body.push_back(kpi_rows_->Render() | vscroll_indicator | frame | size(HEIGHT, LESS_THAN, 12));
            }
            body.push_back(separator() | color(theme::kMuted));
            body.push_back(hbox({wizard_generate->Render(), text("  "), wizard_back->Render(), text("  "),
                                  wizard_cancel->Render()}) |
                           hcenter);
        }
        return vbox(body) | size(WIDTH, EQUAL, 76) | borderStyled(ROUNDED, theme::kAccent) | bgcolor(Color::Black) |
               clear_under | center;
    });

    root_ = Renderer(layout, [this, year_input, load_button, report_button, output_menu, output_panes, copy_button,
                              pdf_button] {
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
        children.push_back(output_menu->Render());
        children.push_back(output_panes->Render() | flex);
        children.push_back(hbox({copy_button->Render(), text(" "), pdf_button->Render()}));

        return vbox(children) | flex;
    });

    root_ |= Modal(wizard, &wizard_visible_);
}

}  // namespace callscribe::ui
