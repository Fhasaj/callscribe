#include <doctest/doctest.h>

#include "llm/prompts.hpp"

using namespace callscribe::llm;

TEST_CASE("summary prompt includes context and JSON instructions") {
    std::string prompt =
        build_summary_prompt("We discussed pricing.", "Acme Corp", "Q3 Renewal", "Sarah Jones", "2026-07-28");
    CHECK(prompt.find("Acme Corp") != std::string::npos);
    CHECK(prompt.find("Q3 Renewal") != std::string::npos);
    CHECK(prompt.find("Sarah Jones") != std::string::npos);
    CHECK(prompt.find("2026-07-28") != std::string::npos);
    CHECK(prompt.find("We discussed pricing.") != std::string::npos);
    CHECK(prompt.find("\"summary\"") != std::string::npos);
    CHECK(prompt.find("\"customer_email\"") != std::string::npos);
    CHECK(prompt.find("\"tasks\"") != std::string::npos);
}

TEST_CASE("summary prompt handles empty transcript/customer") {
    std::string prompt = build_summary_prompt("", "", "", "", "2026-07-28");
    CHECK(prompt.find("no speech detected") != std::string::npos);
    CHECK(prompt.find("Unknown") != std::string::npos);
}

TEST_CASE("yearly prompt includes stats and summaries") {
    std::vector<std::pair<std::string, int>> top = {{"Acme", 4}};
    std::string prompt = build_yearly_narrative_prompt(2026, {"Discussed renewal."}, 12, 7200, 5, top);
    CHECK(prompt.find("2026") != std::string::npos);
    CHECK(prompt.find("Total calls: 12") != std::string::npos);
    CHECK(prompt.find("2 hours") != std::string::npos);
    CHECK(prompt.find("Acme") != std::string::npos);
    CHECK(prompt.find("Discussed renewal.") != std::string::npos);
}

TEST_CASE("proofread prompt includes the original email and asks for text-only output") {
    std::string prompt = build_proofread_prompt("hey can u send me teh contract");
    CHECK(prompt.find("hey can u send me teh contract") != std::string::npos);
    CHECK(prompt.find("ONLY the corrected email text") != std::string::npos);
}

TEST_CASE("KPI report prompt lists the user's KPIs and asks for report + email JSON") {
    std::vector<std::pair<std::string, int>> top = {{"Acme", 4}};
    std::string prompt = build_kpi_report_prompt(2026, "annual", {"Renewals secured", "Escalations resolved"},
                                                  {"Discussed renewal."}, 12, 7200, 5, top);
    CHECK(prompt.find("annual") != std::string::npos);
    CHECK(prompt.find("1. Renewals secured") != std::string::npos);
    CHECK(prompt.find("2. Escalations resolved") != std::string::npos);
    CHECK(prompt.find("Total calls: 12") != std::string::npos);
    CHECK(prompt.find("Discussed renewal.") != std::string::npos);
    CHECK(prompt.find("\"report\"") != std::string::npos);
    CHECK(prompt.find("\"email_subject\"") != std::string::npos);
    CHECK(prompt.find("\"email_body\"") != std::string::npos);
}

TEST_CASE("KPI report prompt still works with no KPIs") {
    std::string prompt = build_kpi_report_prompt(2026, "semi-annual", {}, {"Discussed renewal."}, 3, 3600, 1, {});
    CHECK(prompt.find("No specific KPIs were requested") != std::string::npos);
    CHECK(prompt.find("\"report\"") != std::string::npos);
}
