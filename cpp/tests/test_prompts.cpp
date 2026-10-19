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

TEST_CASE("summary prompt scopes tasks to the user and lists dates to look up") {
    std::string prompt = build_summary_prompt("x", "Acme", "Sync", "Jane", "19 10 2026", "", "", "", "Fatlum Hasaj");
    CHECK(prompt.find("Fatlum Hasaj's own to-do items") != std::string::npos);
    CHECK(prompt.find("labelled \"Fatlum Hasaj:\"") != std::string::npos);
    CHECK(prompt.find("someone else will do") != std::string::npos);
    CHECK(prompt.find("Monday 2026-10-19 (call day)") != std::string::npos);
    CHECK(prompt.find("Tuesday 2026-10-20 (tomorrow)") != std::string::npos);
    CHECK(prompt.find("Friday 2026-10-23") != std::string::npos);
    // ISO dates work too, and the table runs two weeks.
    CHECK(build_summary_prompt("x", "", "", "", "2026-10-19").find("Sunday 2026-11-01") != std::string::npos);
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
