#include <doctest/doctest.h>
#include <unistd.h>

#include <filesystem>

#include "db/database.hpp"
#include "db/repository.hpp"
#include "llm/evidence.hpp"
#include "llm/prompts.hpp"
#include "llm/summarizer.hpp"
#include "tasks/day_digest.hpp"
#include "util/time.hpp"

using namespace callscribe;

namespace {
std::filesystem::path temp_db_dir(const char* name) {
    auto dir = std::filesystem::temp_directory_path() / ("callscribe_review_" + std::to_string(getpid()) + "_" + name);
    std::filesystem::create_directories(dir);
    return dir;
}
}  // namespace

TEST_CASE("evidence: keywords drop filler and stem") {
    auto k = llm::keywords("What are some of my top accomplishments improving onboarding?");
    CHECK(std::find(k.begin(), k.end(), "what") == k.end());
    CHECK(std::find(k.begin(), k.end(), "onboard") != k.end());
    CHECK(std::find(k.begin(), k.end(), "accomplish") != k.end());
}

TEST_CASE("evidence: pinned first, then relevant, within budget, returned oldest first") {
    std::vector<llm::EvidenceItem> items = {
        {"2026-01-05", "Call", "Weekly sync, nothing much", false},
        {"2026-02-10", "Call", "Fixed the onboarding import for Acme", false},
        {"2026-03-01", "Win", "Shipped the parser", true},
        {"2026-04-01", "Call", "Pricing chat", false},
    };
    auto chosen = llm::select_evidence(items, "How did you improve onboarding?", 120);
    REQUIRE(chosen.size() == 2);
    CHECK(chosen[0].text.find("onboarding") != std::string::npos);
    CHECK(chosen[1].kind == "Win");
}

TEST_CASE("evidence: with nothing relevant, picks spread across months") {
    std::vector<llm::EvidenceItem> items;
    for (int m = 1; m <= 6; ++m)
        for (int d = 1; d <= 3; ++d)
            items.push_back({"2026-0" + std::to_string(m) + "-0" + std::to_string(d), "Call", "x", false});
    auto line = llm::render_evidence_line(items[0]).size() + 1;
    auto chosen = llm::select_evidence(items, "What do I do well?", line * 6);
    REQUIRE(chosen.size() == 6);
    for (int m = 0; m < 6; ++m) CHECK(chosen[static_cast<size_t>(m)].date.substr(5, 2) == "0" + std::to_string(m + 1));
}

TEST_CASE("review questions split on blank lines or one-per-line lists") {
    auto a = llm::split_questions("What went well?\n\nHow could I improve,\nconsidering X?\n");
    REQUIRE(a.size() == 2);
    CHECK(a[1] == "How could I improve,\nconsidering X?");
    auto b = llm::split_questions("What went well?\nWhat didn't?\nWhat next?");
    CHECK(b.size() == 3);
    CHECK(llm::split_questions("  \n ").empty());
}

TEST_CASE("review prompt states facts, evidence and the no-invention rule") {
    llm::ReviewFacts f;
    f.period = "2026";
    f.call_count = 12;
    f.call_seconds = 3 * 3600 + 600;
    f.wins = 2;
    f.focus_seconds_by_tag = {{"Indica", 7200}};
    std::vector<llm::EvidenceItem> ev = {{"2026-03-01", "Win", "Shipped the parser", true}};
    auto p = llm::build_review_report_prompt("Sam", "annual", {"What do I do well?"}, f, ev, 40);
    CHECK(p.find("Calls: 12, totalling 3h 10m") != std::string::npos);
    CHECK(p.find("Indica 2h") != std::string::npos);
    CHECK(p.find("2026-03-01 · Win · Shipped the parser") != std::string::npos);
    CHECK(p.find("the 1 most relevant of 40") != std::string::npos);
    CHECK(p.find("Never invent") != std::string::npos);
    CHECK(p.find("Evidence gap:") != std::string::npos);
}

TEST_CASE("task breakdown steps parse from the usual reply shapes") {
    auto plain = llm::parse_steps(nlohmann::json::parse(R"({"steps":["Draft the email","- List owners",""]})"));
    CHECK(plain == std::vector<std::string>{"Draft the email", "List owners"});
    auto objs = llm::parse_steps(nlohmann::json::parse(R"({"steps":[{"step":"A"},{"description":"B"},{"x":1}]})"));
    CHECK(objs == std::vector<std::string>{"A", "B"});
    auto blob = llm::parse_steps(nlohmann::json::parse(R"({"steps":"1. One\n2. Two\n\n"})"));
    CHECK(blob == std::vector<std::string>{"One", "Two"});
}

TEST_CASE("wins, parking lot, daily logs and the day digest") {
    auto dir = temp_db_dir("misc");
    {
        db::Database database((dir / "t.db").string());
        db::Repository repo(database);

        repo.add_win("Closed the Acme renewal", "Indica");
        const std::string today = util::today_iso();
        auto wins = repo.list_wins(today, today + "~");
        REQUIRE(wins.size() == 1);
        CHECK(wins[0].tag == "Indica");

        auto id = repo.park("email Sam");
        repo.park("look up that library");
        CHECK(repo.list_parked().size() == 2);
        repo.resolve_parked(id);
        auto parked = repo.list_parked();
        REQUIRE(parked.size() == 1);
        CHECK(parked[0].text == "look up that library");

        repo.save_daily_log(today, "digest v1", "good day");
        repo.save_daily_log(today, "digest v2", "great day");
        auto log = repo.get_daily_log(today);
        REQUIRE(log.has_value());
        CHECK(log->digest == "digest v2");
        CHECK(log->note == "great day");

        db::FocusSession fs;
        fs.tag = "Indica";
        fs.started_at = today + "T10:00:00";
        fs.ended_at = today + "T10:25:00";
        fs.duration_seconds = 1500;
        repo.add_focus_session(fs);
        auto digest = tasks::build_day_digest(repo, today);
        CHECK(digest.find("Calls: 0") != std::string::npos);
        CHECK(digest.find("Focus: Indica 25m") != std::string::npos);
        CHECK(digest.find("[Indica] Closed the Acme renewal") != std::string::npos);
        CHECK(digest.find("Still parked: 1") != std::string::npos);

        auto material = llm::collect_review_material(repo, util::current_year());
        CHECK(material.facts.wins == 1);
        CHECK(material.evidence.size() == 2);  // the win and the day's note; the focus round had no notes
    }
    std::filesystem::remove_all(dir);
}
