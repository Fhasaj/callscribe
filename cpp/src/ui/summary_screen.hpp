#pragma once
#include <ftxui/component/component.hpp>

#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "context.hpp"
#include "db/repository.hpp"

namespace callscribe::ui {

class SummaryScreen {
public:
    explicit SummaryScreen(AppContext& ctx);
    ~SummaryScreen();
    SummaryScreen(const SummaryScreen&) = delete;
    SummaryScreen& operator=(const SummaryScreen&) = delete;

    ftxui::Component component() { return root_; }

private:
    AppContext& ctx_;
    std::string year_text_;
    db::YearlyStats stats_;

    std::mutex mutex_;
    std::string report_;         // long-form report (what the PDF gets)
    std::string email_subject_;  // email-ready version of the same report
    std::string email_body_;
    std::string status_message_;
    bool generating_ = false;
    std::thread worker_;

    // Report wizard: step 0 picks the cycle, step 1 is the KPI list the report
    // is built around -- the user's own KPIs, added and removed here and
    // persisted to Settings so the next report starts from the same list.
    bool wizard_visible_ = false;
    int wizard_step_ = 0;
    int cycle_index_ = 0;  // 0 = semi-annual, 1 = annual
    std::vector<std::string> kpis_;
    std::string kpi_input_;
    int pending_remove_ = -1;  // KPI index a row's Remove button asked to drop

    // Output tabs: 0 = report, 1 = email-ready version.
    int output_tab_ = 0;
    std::vector<std::string> output_tab_titles_ = {"Report", "Email"};

    ftxui::Component root_;
    ftxui::Component kpi_rows_;  // rebuilt whenever kpis_ changes

    void load_stats();
    void add_kpi();
    void rebuild_kpi_rows();
    void save_kpis();
    void generate_report();
    void export_pdf();
    void copy_current_tab();
    int selected_year();
    void build_ui();
};

}  // namespace callscribe::ui
