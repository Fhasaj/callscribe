#pragma once
#include "db/database.hpp"

#include <array>
#include <atomic>
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
    // Position of this line within the recording. Both 0 for lines written
    // before timestamps were kept, and for anything that never came from the
    // audio pipeline.
    int64_t start_ms = 0;
    int64_t end_ms = 0;
};

struct Note {
    int64_t id = 0;
    int64_t call_id = 0;
    std::string summary;
    std::string email_draft;
    std::string key_points_json;
    std::string created_at;
};

// Priority is stored lower-case ("low" / "medium" / "high"); use
// normalize_priority() on anything that came from a user or a model.
inline constexpr const char* kPriorityLow = "low";
inline constexpr const char* kPriorityMedium = "medium";
inline constexpr const char* kPriorityHigh = "high";

// Task lifecycle. "proposed" is what the summarizer files its extracted action
// items as: they belong to the call and show up under its summary for review,
// but they are deliberately NOT real tasks yet -- they stay out of the Tasks
// tab, out of the due-date reminders, and out of every count until you confirm
// them. Confirming flips one to "open"; deleting drops it.
inline constexpr const char* kStatusProposed = "proposed";
inline constexpr const char* kStatusOpen = "open";
inline constexpr const char* kStatusDone = "done";

// Maps free text ("High", "urgent", "P1", "") onto one of the three stored
// values; anything unrecognized becomes "medium".
std::string normalize_priority(const std::string& text);

struct Task {
    int64_t id = 0;
    std::optional<int64_t> call_id;
    std::optional<int64_t> customer_id;
    std::string customer_name;
    std::string description;
    std::string due_date;
    std::string priority = kPriorityMedium;
    std::string status;
    std::string source;
    std::string created_at;
    std::string completed_at;
};

struct FocusSession {
    int64_t id = 0;
    std::string tag;
    std::string next_step;      // what you meant to do, asked when the round starts
    std::string progress_note;  // where you got to, asked when it ends
    std::string started_at;
    std::string ended_at;
    int duration_seconds = 0;   // time actually focused, pauses excluded
};

struct Win {
    int64_t id = 0;
    std::string text;
    std::string tag;
    std::string created_at;
};

struct ParkedItem {
    int64_t id = 0;
    std::string text;
    std::string created_at;
};

struct DailyLog {
    std::string day;     // YYYY-MM-DD
    std::string digest;  // generated: calls, tasks done, focus time, wins
    std::string note;    // the user's own line about the day
    std::string updated_at;
};

// A call that has notes, flattened for the review evidence.
struct CallNote {
    std::string started_at;
    std::string customer_name;
    std::string meeting_name;
    std::string summary;
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
    void append_segment(int64_t call_id, const std::string& speaker, const std::string& text,
                         int64_t start_ms = 0, int64_t end_ms = 0);
    // Renames the speaker on one transcript line. This is how diarization
    // corrects the best-effort labels applied live once the call is over.
    void set_segment_speaker(int64_t segment_id, const std::string& speaker);
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

    // `status` is kStatusOpen for anything the user asked for directly, and
    // kStatusProposed for model-extracted items awaiting review.
    int64_t add_task(const std::string& description, std::optional<int64_t> call_id = std::nullopt,
                      std::optional<int64_t> customer_id = std::nullopt, const std::string& due_date = "",
                      const std::string& source = "manual", const std::string& priority = kPriorityMedium,
                      const std::string& status = kStatusOpen);
    // Edits the user-facing fields of an existing task; status/source/call
    // linkage are left alone. An empty due_date clears the due date.
    void update_task(int64_t task_id, const std::string& description, const std::string& due_date,
                      const std::string& priority, std::optional<int64_t> customer_id);
    // An empty `status` means "every real task" -- proposed ones are excluded
    // unless asked for by name, so they can't leak into the Tasks tab or a count.
    std::vector<Task> list_tasks(const std::string& status = "", const std::string& customer_name = "");
    // Returns confirmed and proposed tasks alike; callers split on Task::status.
    std::vector<Task> list_tasks_for_call(int64_t call_id);
    // Promotes a proposed task to a real open one. No-op on a task that is
    // already open or done.
    void confirm_task(int64_t task_id);
    void complete_task(int64_t task_id);
    void reopen_task(int64_t task_id);
    void delete_task(int64_t task_id);
    std::vector<Task> due_tasks(const std::string& on_or_before);

    YearlyStats yearly_stats(int year);

    int64_t add_focus_session(const FocusSession& session);
    // Newest first.
    std::vector<FocusSession> list_focus_sessions(int limit = 50);

    // Date ranges below are half-open, [from, to), as ISO dates or
    // timestamps -- they compare as strings against the stored ISO values.
    std::vector<FocusSession> list_focus_sessions_between(const std::string& from, const std::string& to);
    // Total focused seconds per tag, largest first.
    std::vector<std::pair<std::string, int>> focus_seconds_by_tag(const std::string& from, const std::string& to);

    int64_t add_win(const std::string& text, const std::string& tag);
    void delete_win(int64_t id);
    // Newest first.
    std::vector<Win> list_wins(const std::string& from, const std::string& to);

    int64_t park(const std::string& text);
    // Unresolved ones, oldest first -- the order they were parked in.
    std::vector<ParkedItem> list_parked();
    void resolve_parked(int64_t id);

    // Inserts or replaces the log for `day`.
    void save_daily_log(const std::string& day, const std::string& digest, const std::string& note);
    std::optional<DailyLog> get_daily_log(const std::string& day);
    std::vector<DailyLog> list_daily_logs(const std::string& from, const std::string& to);

    // Review evidence: calls with notes, and tasks completed, in a range.
    std::vector<CallNote> list_call_notes(const std::string& from, const std::string& to);
    std::vector<Task> list_completed_tasks(const std::string& from, const std::string& to);
    // Calls started in the range (any status), oldest first.
    std::vector<Call> list_calls_between(const std::string& from, const std::string& to);

    // Bumped by every write that changes what another screen might be
    // showing -- a finished call, a new/edited/completed task, a summary. The
    // screens are independent components that each cache their own query
    // results, so they compare this against what they last saw and reload
    // when it moves; that is how the Tasks and History tabs pick up a call
    // the Record tab just summarized.
    //
    // Deliberately NOT bumped by append_segment() or update_manual_notes():
    // those fire per transcribed chunk and per keystroke, and would have the
    // other screens rebuilding themselves continuously mid-call.
    uint64_t revision() const { return revision_.load(); }

private:
    sqlite3* db_;
    std::mutex mutex_;
    std::atomic<uint64_t> revision_{0};

    void bump() { revision_.fetch_add(1); }
};

}  // namespace callscribe::db
