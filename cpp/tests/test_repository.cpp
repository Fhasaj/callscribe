#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <filesystem>

#include "db/database.hpp"
#include "db/repository.hpp"

using namespace callscribe::db;

namespace {
std::atomic<int> g_counter{0};

std::filesystem::path make_temp_db_dir() {
    auto dir = std::filesystem::temp_directory_path() /
               ("callscribe_cpp_test_" + std::to_string(getpid()) + "_" + std::to_string(g_counter++));
    std::filesystem::create_directories(dir);
    return dir;
}
}  // namespace

TEST_CASE("customer get_or_create is idempotent") {
    auto dir = make_temp_db_dir();
    Database db((dir / "test.db").string());
    Repository repo(db);

    int64_t id1 = repo.get_or_create_customer("Acme Corp");
    int64_t id2 = repo.get_or_create_customer("Acme Corp");
    CHECK(id1 == id2);
    CHECK(repo.list_customers().size() == 1);

    std::filesystem::remove_all(dir);
}

TEST_CASE("call lifecycle, speaker-labeled segments, and note") {
    auto dir = make_temp_db_dir();
    Database db((dir / "test.db").string());
    Repository repo(db);

    int64_t cust_id = repo.get_or_create_customer("Globex");
    int64_t call_id = repo.create_call(cust_id, "Q3 Renewal", "Sarah Jones", "2026-03-01T10:00:00");

    repo.append_segment(call_id, "Fatlum Hasaj", "Hello there.");
    repo.append_segment(call_id, "Sarah Jones", "Hi, thanks for calling.");
    repo.append_segment(call_id, "Fatlum Hasaj", "Let's discuss the contract.");

    repo.finish_call(call_id, 120, "/tmp/x.wav");

    auto call = repo.get_call(call_id);
    REQUIRE(call.has_value());
    CHECK(call->status == "done");
    CHECK(call->customer_name == "Globex");
    CHECK(call->meeting_name == "Q3 Renewal");
    CHECK(call->participants == "Sarah Jones");

    std::string transcript = repo.render_transcript(call_id);
    CHECK(transcript ==
          "Fatlum Hasaj: Hello there.\nSarah Jones: Hi, thanks for calling.\nFatlum Hasaj: Let's discuss the "
          "contract.");

    int64_t note_id = repo.add_note(call_id, "Discussed contract.", "Hi, ...", "[]");
    CHECK(note_id > 0);
    auto note = repo.get_note_for_call(call_id);
    REQUIRE(note.has_value());
    CHECK(note->summary == "Discussed contract.");

    CHECK(call->manual_notes.empty());
    repo.update_manual_notes(call_id, "Remember to follow up on pricing.");
    auto updated_call = repo.get_call(call_id);
    REQUIRE(updated_call.has_value());
    CHECK(updated_call->manual_notes == "Remember to follow up on pricing.");

    std::filesystem::remove_all(dir);
}

TEST_CASE("tasks: extraction from a call and completion") {
    auto dir = make_temp_db_dir();
    Database db((dir / "test.db").string());
    Repository repo(db);

    int64_t cust_id = repo.get_or_create_customer("Initech");
    int64_t call_id = repo.create_call(cust_id, "", "");
    int64_t task_id = repo.add_task("Send updated pricing sheet", call_id, cust_id, "", "call");

    auto open_tasks = repo.list_tasks("open");
    CHECK(open_tasks.size() == 1);
    CHECK(open_tasks[0].source == "call");

    repo.complete_task(task_id);
    CHECK(repo.list_tasks("open").size() == 0);
    CHECK(repo.list_tasks("done").size() == 1);

    std::filesystem::remove_all(dir);
}

TEST_CASE("transcript: segments carry their position in the recording and can be relabelled") {
    auto dir = make_temp_db_dir();
    Database db((dir / "test.db").string());
    Repository repo(db);

    int64_t call_id = repo.create_call(std::nullopt, "Standup", "");
    repo.append_segment(call_id, "Me", "morning all", 0, 1500);
    repo.append_segment(call_id, "Other", "morning", 1600, 2400);
    // Written without times, as everything before diarization existed was.
    repo.append_segment(call_id, "Other", "no timestamps here");

    auto segments = repo.get_segments(call_id);
    REQUIRE(segments.size() == 3);
    CHECK(segments[1].start_ms == 1600);
    CHECK(segments[1].end_ms == 2400);
    CHECK(segments[2].start_ms == 0);
    CHECK(segments[2].end_ms == 0);

    // Diarization renaming a far-end line rewrites only that row's speaker.
    repo.set_segment_speaker(segments[1].id, "Sarah");
    segments = repo.get_segments(call_id);
    CHECK(segments[0].speaker == "Me");
    CHECK(segments[1].speaker == "Sarah");
    CHECK(segments[1].text == "morning");
    CHECK(segments[2].speaker == "Other");

    CHECK(repo.render_transcript(call_id).find("Sarah: morning") != std::string::npos);

    std::filesystem::remove_all(dir);
}

TEST_CASE("tasks: proposed tasks stay out of every list until they are confirmed") {
    auto dir = make_temp_db_dir();
    Database db((dir / "test.db").string());
    Repository repo(db);

    int64_t call_id = repo.create_call(std::nullopt, "Quarterly review", "");
    int64_t proposed_id = repo.add_task("Send the revised quote", call_id, std::nullopt, "2020-01-01", "call",
                                         kPriorityHigh, kStatusProposed);
    repo.add_task("Something I typed myself", call_id);

    // Invisible to the Tasks tab (both the "open" filter and the unfiltered
    // listing behind "show completed"), and to the daily reminder sweep.
    CHECK(repo.list_tasks("open").size() == 1);
    CHECK(repo.list_tasks().size() == 1);
    CHECK(repo.due_tasks("2099-01-01").size() == 1);

    // Visible to the call it came from, which is where it gets reviewed.
    auto for_call = repo.list_tasks_for_call(call_id);
    CHECK(for_call.size() == 2);

    repo.confirm_task(proposed_id);
    CHECK(repo.list_tasks("open").size() == 2);
    CHECK(repo.list_tasks(kStatusProposed).empty());

    // Confirming twice must not resurrect a task that was since completed.
    repo.complete_task(proposed_id);
    repo.confirm_task(proposed_id);
    CHECK(repo.list_tasks("done").size() == 1);

    std::filesystem::remove_all(dir);
}

TEST_CASE("tasks: priority defaults to medium, and editing rewrites every field") {
    auto dir = make_temp_db_dir();
    Database db((dir / "test.db").string());
    Repository repo(db);

    int64_t task_id = repo.add_task("Chase the alarm request");
    auto tasks = repo.list_tasks("open");
    REQUIRE(tasks.size() == 1);
    CHECK(tasks[0].priority == kPriorityMedium);
    CHECK(tasks[0].due_date.empty());
    CHECK(tasks[0].customer_name.empty());

    int64_t cust_id = repo.get_or_create_customer("Kingston");
    repo.update_task(task_id, "Chase the alarm request with Emma", "2026-09-01", "High", cust_id);

    tasks = repo.list_tasks("open");
    REQUIRE(tasks.size() == 1);
    CHECK(tasks[0].description == "Chase the alarm request with Emma");
    CHECK(tasks[0].due_date == "2026-09-01");
    CHECK(tasks[0].priority == kPriorityHigh);
    CHECK(tasks[0].customer_name == "Kingston");

    // Clearing the due date is an edit like any other.
    repo.update_task(task_id, "Chase the alarm request", "", "low", std::nullopt);
    tasks = repo.list_tasks("open");
    CHECK(tasks[0].due_date.empty());
    CHECK(tasks[0].priority == kPriorityLow);
    CHECK(tasks[0].customer_name.empty());

    std::filesystem::remove_all(dir);
}

TEST_CASE("tasks: same due date sorts high priority first") {
    auto dir = make_temp_db_dir();
    Database db((dir / "test.db").string());
    Repository repo(db);

    repo.add_task("Low one", std::nullopt, std::nullopt, "2026-09-01", "manual", "low");
    repo.add_task("High one", std::nullopt, std::nullopt, "2026-09-01", "manual", "high");
    repo.add_task("Medium one", std::nullopt, std::nullopt, "2026-09-01", "manual", "medium");

    auto tasks = repo.list_tasks("open");
    REQUIRE(tasks.size() == 3);
    CHECK(tasks[0].description == "High one");
    CHECK(tasks[1].description == "Medium one");
    CHECK(tasks[2].description == "Low one");

    std::filesystem::remove_all(dir);
}

TEST_CASE("due_tasks includes overdue and no-due-date, excludes future") {
    auto dir = make_temp_db_dir();
    Database db((dir / "test.db").string());
    Repository repo(db);

    repo.add_task("No due date task");
    repo.add_task("Overdue task", std::nullopt, std::nullopt, "2020-01-01");
    repo.add_task("Future task", std::nullopt, std::nullopt, "2099-01-01");

    auto due = repo.due_tasks("2026-07-28");
    bool has_no_due = false, has_overdue = false, has_future = false;
    for (auto& t : due) {
        if (t.description == "No due date task") has_no_due = true;
        if (t.description == "Overdue task") has_overdue = true;
        if (t.description == "Future task") has_future = true;
    }
    CHECK(has_no_due);
    CHECK(has_overdue);
    CHECK_FALSE(has_future);

    std::filesystem::remove_all(dir);
}

TEST_CASE("yearly stats aggregate calls, tasks, and summaries") {
    auto dir = make_temp_db_dir();
    Database db((dir / "test.db").string());
    Repository repo(db);

    int64_t cust_id = repo.get_or_create_customer("Umbrella");
    int64_t call_id = repo.create_call(cust_id, "", "", "2026-05-01T09:00:00");
    repo.finish_call(call_id, 600, "");
    repo.add_note(call_id, "Great call.", "Email body", "[]");
    int64_t task_id = repo.add_task("Follow up", std::nullopt, cust_id);
    repo.complete_task(task_id);

    auto stats = repo.yearly_stats(2026);
    CHECK(stats.call_count == 1);
    CHECK(stats.total_seconds == 600);
    CHECK(stats.tasks_completed == 1);
    REQUIRE(stats.top_customers.size() == 1);
    CHECK(stats.top_customers[0].first == "Umbrella");
    CHECK(std::find(stats.summaries.begin(), stats.summaries.end(), "Great call.") != stats.summaries.end());

    auto stats_other_year = repo.yearly_stats(2020);
    CHECK(stats_other_year.call_count == 0);

    std::filesystem::remove_all(dir);
}
