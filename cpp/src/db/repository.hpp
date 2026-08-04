#pragma once
#include "db/database.hpp"

#include <array>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace callscribe::db {

struct Customer {
    int64_t id = 0;
    std::string name;
    std::string created_at;
};

struct Call {
    int64_t id = 0;
    std::optional<int64_t> customer_id;
    std::string customer_name;
    std::string meeting_name;
    std::string participants;
    std::string started_at;
    std::string ended_at;
    int duration_seconds = 0;
    std::string audio_path;
    std::string status;
    std::string manual_notes;
};

struct TranscriptSegment {
    int64_t id = 0;
    int64_t call_id = 0;
    int64_t seq = 0;
    std::string speaker;
    std::string text;
    std::string created_at;
};

struct Note {
    int64_t id = 0;
    int64_t call_id = 0;
    std::string summary;
    std::string email_draft;
    std::string key_points_json;
    std::string created_at;
};

struct Task {
    int64_t id = 0;
    std::optional<int64_t> call_id;
    std::optional<int64_t> customer_id;
    std::string customer_name;
    std::string description;
    std::string due_date;
    std::string status;
    std::string source;
    std::string created_at;
    std::string completed_at;
};

struct YearlyStats {
    int call_count = 0;
    long long total_seconds = 0;
    int tasks_completed = 0;
    std::vector<std::pair<std::string, int>> top_customers;
    std::vector<std::string> summaries;
    std::array<int, 12> monthly_call_counts{};  // index 0 = January
};

// Thread-safe CRUD access to the SQLite DB. Plain parameterized SQL, no
// ORM -- mirrors app/db/repository.py's shape, plus transcript_segments
// (speaker-labeled lines) instead of a single flattened transcript column.
class Repository {
public:
    explicit Repository(Database& db);

    int64_t get_or_create_customer(const std::string& name);
    std::vector<Customer> list_customers();

    int64_t create_call(std::optional<int64_t> customer_id, const std::string& meeting_name,
                         const std::string& participants, const std::string& started_at = "");
    void append_segment(int64_t call_id, const std::string& speaker, const std::string& text);
    void finish_call(int64_t call_id, int duration_seconds, const std::string& audio_path,
                      const std::string& status = "done");
    void set_call_status(int64_t call_id, const std::string& status);
    void update_manual_notes(int64_t call_id, const std::string& notes);
    std::optional<Call> get_call(int64_t call_id);
    std::vector<Call> list_calls(const std::string& customer_name = "", const std::string& search = "");
    std::vector<TranscriptSegment> get_segments(int64_t call_id);
    std::string render_transcript(int64_t call_id);

    int64_t add_note(int64_t call_id, const std::string& summary, const std::string& email_draft,
                      const std::string& key_points_json);
    std::optional<Note> get_note_for_call(int64_t call_id);

    int64_t add_task(const std::string& description, std::optional<int64_t> call_id = std::nullopt,
                      std::optional<int64_t> customer_id = std::nullopt, const std::string& due_date = "",
                      const std::string& source = "manual");
    std::vector<Task> list_tasks(const std::string& status = "", const std::string& customer_name = "");
    std::vector<Task> list_tasks_for_call(int64_t call_id);
    void complete_task(int64_t task_id);
    void reopen_task(int64_t task_id);
    void delete_task(int64_t task_id);
    std::vector<Task> due_tasks(const std::string& on_or_before);

    YearlyStats yearly_stats(int year);

private:
    sqlite3* db_;
    std::mutex mutex_;
};

}  // namespace callscribe::db
