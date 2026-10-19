#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "db/repository.hpp"
#include "llm/summarizer.hpp"
#include "util/date_format.hpp"

using namespace callscribe;
using nlohmann::json;

TEST_CASE("task objects keep their due date, priority and customer") {
    json node = json::parse(R"([
        {"description": "Send the pricing sheet", "due_date": "2026-08-14", "priority": "High",
         "customer": "Acme Corp"},
        {"description": "Book the follow-up", "due_date": "", "priority": "low", "customer": ""}
    ])");
    auto tasks = llm::parse_task_items(node, "Default Ltd", "2026-08-13");

    REQUIRE(tasks.size() == 2);
    CHECK(tasks[0].description == "Send the pricing sheet");
    CHECK(tasks[0].due_date == "2026-08-14");
    CHECK(tasks[0].priority == db::kPriorityHigh);
    CHECK(tasks[0].customer == "Acme Corp");
    CHECK(tasks[1].due_date.empty());
    CHECK(tasks[1].priority == db::kPriorityLow);
    CHECK(tasks[1].customer == "Default Ltd");  // falls back to the call's customer
}

TEST_CASE("relative due dates are resolved against the call date") {
    json node = json::parse(R"([
        {"description": "Call Ricky back", "due_date": "Tomorrow", "priority": "high"},
        {"description": "Chase the alarm request", "due_date": "Not specified", "priority": "Medium"}
    ])");
    auto tasks = llm::parse_task_items(node, "Acme", "2026-08-13");

    REQUIRE(tasks.size() == 2);
    CHECK(tasks[0].due_date == "2026-08-14");
    CHECK(tasks[1].due_date.empty());
}

TEST_CASE("a markdown table answer becomes one task per row") {
    // What a model actually returns when it ignores the JSON object schema.
    std::string table =
        "| Task description | Owner | Due date | Priority | Source context |\n"
        "|---|---|---|---|---|\n"
        "| Send the latest report to all stakeholders | Tom | Not specified | Medium | \"right after the call\" |\n"
        "| Schedule a 10 am steering-group call | Tom | Tomorrow | High | \"we'll do a 10am call\" |\n";
    json node = table;
    auto tasks = llm::parse_task_items(node, "Acme", "2026-08-13");

    REQUIRE(tasks.size() == 2);
    CHECK(tasks[0].description == "Send the latest report to all stakeholders");
    CHECK(tasks[0].due_date.empty());
    CHECK(tasks[0].priority == db::kPriorityMedium);
    CHECK(tasks[1].description == "Schedule a 10 am steering-group call");
    CHECK(tasks[1].due_date == "2026-08-14");
    CHECK(tasks[1].priority == db::kPriorityHigh);
    CHECK(tasks[1].customer == "Acme");
}

TEST_CASE("a table hidden inside a task object's description is still split") {
    json node = json::parse(R"([{"description": "| Task | Due date |\n|---|---|\n| Email Emma | 2026-09-01 |\n| Email Sarah | |"}])");
    auto tasks = llm::parse_task_items(node, "Acme", "2026-08-13");

    REQUIRE(tasks.size() == 2);
    CHECK(tasks[0].description == "Email Emma");
    CHECK(tasks[0].due_date == "2026-09-01");
    CHECK(tasks[1].description == "Email Sarah");
}

TEST_CASE("a bullet list crammed into one string becomes separate tasks") {
    json node = json::parse(R"(["- Send the quote\n- **Book the site visit**\n1. Update the CRM"])");
    auto tasks = llm::parse_task_items(node, "", "2026-08-13");

    REQUIRE(tasks.size() == 3);
    CHECK(tasks[0].description == "Send the quote");
    CHECK(tasks[1].description == "Book the site visit");
    CHECK(tasks[2].description == "Update the CRM");
}

TEST_CASE("plain string tasks and empty values are handled") {
    auto tasks = llm::parse_task_items(json::parse(R"(["Send the report"])"), "Acme", "2026-08-13");
    REQUIRE(tasks.size() == 1);
    CHECK(tasks[0].description == "Send the report");
    CHECK(tasks[0].priority == db::kPriorityMedium);

    CHECK(llm::parse_task_items(json::array(), "Acme", "2026-08-13").empty());
    CHECK(llm::parse_task_items(json(nullptr), "Acme", "2026-08-13").empty());
    CHECK(llm::parse_task_items(json(""), "Acme", "2026-08-13").empty());
}

TEST_CASE("priority normalization") {
    CHECK(db::normalize_priority("High") == db::kPriorityHigh);
    CHECK(db::normalize_priority("URGENT") == db::kPriorityHigh);
    CHECK(db::normalize_priority("low") == db::kPriorityLow);
    CHECK(db::normalize_priority("") == db::kPriorityMedium);
    CHECK(db::normalize_priority("whatever") == db::kPriorityMedium);
}

TEST_CASE("due date normalization") {
    CHECK(util::normalize_due_date("2026-08-14", "2026-08-13") == "2026-08-14");
    CHECK(util::normalize_due_date("today", "2026-08-13") == "2026-08-13");
    CHECK(util::normalize_due_date("Tomorrow", "2026-08-13") == "2026-08-14");
    CHECK(util::normalize_due_date("next week", "2026-08-13") == "2026-08-20");
    CHECK(util::normalize_due_date("14 08 2026", "2026-08-13") == "2026-08-14");
    // 2026-08-13 is a Thursday; the next Friday is the 14th.
    CHECK(util::normalize_due_date("friday", "2026-08-13") == "2026-08-14");
    CHECK(util::normalize_due_date("thursday", "2026-08-13") == "2026-08-20");  // "next one", not today
    CHECK(util::normalize_due_date("n/a", "2026-08-13").empty());
    CHECK(util::normalize_due_date("", "2026-08-13").empty());
}

TEST_CASE("the spoken deadline overrides a wrongly computed date") {
    auto j = nlohmann::json::parse(R"([{"description":"Send pricing","due_date":"2026-10-21","due_said":"by Friday"},
                                       {"description":"Book demo","due_date":"","due_said":"next week"},
                                       {"description":"Call back","due_date":"2026-10-30","due_said":""}])");
    auto tasks = callscribe::llm::parse_task_items(j, "Acme", "2026-10-19");
    REQUIRE(tasks.size() == 3);
    CHECK(tasks[0].due_date == "2026-10-23");
    CHECK(tasks[1].due_date.empty());
    CHECK(tasks[2].due_date == "2026-10-30");
}
