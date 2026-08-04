#include "db/repository.hpp"

#include "util/time.hpp"

namespace callscribe::db {

Repository::Repository(Database& db) : db_(db.handle()) {}

// -- customers ------------------------------------------------------------

int64_t Repository::get_or_create_customer(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    {
        Stmt s(db_, "SELECT id FROM customers WHERE name = ?");
        s.bind(1, name);
        if (s.step()) return s.int64(0);
    }
    Stmt ins(db_, "INSERT INTO customers (name, created_at) VALUES (?, ?)");
    ins.bind(1, name).bind(2, util::now_iso());
    ins.execute();
    return ins.last_insert_rowid();
}

std::vector<Customer> Repository::list_customers() {
    std::lock_guard<std::mutex> lock(mutex_);
    Stmt s(db_, "SELECT id, name, created_at FROM customers ORDER BY name COLLATE NOCASE");
    std::vector<Customer> out;
    while (s.step()) {
        out.push_back(Customer{s.int64(0), s.text(1), s.text(2)});
    }
    return out;
}

// -- calls ------------------------------------------------------------------

int64_t Repository::create_call(std::optional<int64_t> customer_id, const std::string& meeting_name,
                                 const std::string& participants, const std::string& started_at) {
    std::lock_guard<std::mutex> lock(mutex_);
    Stmt s(db_,
           "INSERT INTO calls (customer_id, meeting_name, participants, started_at, status) "
           "VALUES (?, ?, ?, ?, 'recording')");
    s.bind_opt(1, customer_id);
    s.bind_opt_text(2, meeting_name);
    s.bind_opt_text(3, participants);
    s.bind(4, started_at.empty() ? util::now_iso() : started_at);
    s.execute();
    return s.last_insert_rowid();
}

void Repository::append_segment(int64_t call_id, const std::string& speaker, const std::string& text) {
    if (text.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t next_seq = 0;
    {
        Stmt s(db_, "SELECT COALESCE(MAX(seq), -1) + 1 FROM transcript_segments WHERE call_id = ?");
        s.bind(1, call_id);
        if (s.step()) next_seq = s.int64(0);
    }
    Stmt ins(db_,
             "INSERT INTO transcript_segments (call_id, seq, speaker, text, created_at) "
             "VALUES (?, ?, ?, ?, ?)");
    ins.bind(1, call_id).bind(2, next_seq).bind(3, speaker).bind(4, text).bind(5, util::now_iso());
    ins.execute();
}

void Repository::finish_call(int64_t call_id, int duration_seconds, const std::string& audio_path,
                              const std::string& status) {
    std::lock_guard<std::mutex> lock(mutex_);
    Stmt s(db_, "UPDATE calls SET ended_at = ?, duration_seconds = ?, audio_path = ?, status = ? WHERE id = ?");
    s.bind(1, util::now_iso()).bind(2, duration_seconds).bind_opt_text(3, audio_path).bind(4, status).bind(5, call_id);
    s.execute();
}

void Repository::set_call_status(int64_t call_id, const std::string& status) {
    std::lock_guard<std::mutex> lock(mutex_);
    Stmt s(db_, "UPDATE calls SET status = ? WHERE id = ?");
    s.bind(1, status).bind(2, call_id);
    s.execute();
}

void Repository::update_manual_notes(int64_t call_id, const std::string& notes) {
    std::lock_guard<std::mutex> lock(mutex_);
    Stmt s(db_, "UPDATE calls SET manual_notes = ? WHERE id = ?");
    s.bind_opt_text(1, notes).bind(2, call_id);
    s.execute();
}

namespace {
Call read_call_row(Stmt& s) {
    Call c;
    c.id = s.int64(0);
    if (!s.is_null(1)) c.customer_id = s.int64(1);
    c.customer_name = s.text(2);
    c.meeting_name = s.text(3);
    c.participants = s.text(4);
    c.started_at = s.text(5);
    c.ended_at = s.text(6);
    c.duration_seconds = s.int_(7);
    c.audio_path = s.text(8);
    c.status = s.text(9);
    c.manual_notes = s.text(10);
    return c;
}
constexpr const char* kCallColumns =
    "calls.id, calls.customer_id, customers.name, calls.meeting_name, calls.participants, "
    "calls.started_at, calls.ended_at, calls.duration_seconds, calls.audio_path, calls.status, "
    "calls.manual_notes ";
}  // namespace

std::optional<Call> Repository::get_call(int64_t call_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    Stmt s(db_, std::string("SELECT ") + kCallColumns +
                    "FROM calls LEFT JOIN customers ON customers.id = calls.customer_id WHERE calls.id = ?");
    s.bind(1, call_id);
    if (!s.step()) return std::nullopt;
    return read_call_row(s);
}

std::vector<Call> Repository::list_calls(const std::string& customer_name, const std::string& search) {
    std::string sql = std::string("SELECT ") + kCallColumns +
                       "FROM calls LEFT JOIN customers ON customers.id = calls.customer_id WHERE 1=1";
    if (!customer_name.empty()) sql += " AND customers.name = ?";
    if (!search.empty()) {
        sql +=
            " AND (customers.name LIKE ? OR calls.meeting_name LIKE ? OR EXISTS ("
            "SELECT 1 FROM transcript_segments ts WHERE ts.call_id = calls.id AND ts.text LIKE ?))";
    }
    sql += " ORDER BY calls.started_at DESC";

    std::lock_guard<std::mutex> lock(mutex_);
    Stmt s(db_, sql);
    int idx = 1;
    if (!customer_name.empty()) s.bind(idx++, customer_name);
    if (!search.empty()) {
        std::string like = "%" + search + "%";
        s.bind(idx++, like);
        s.bind(idx++, like);
        s.bind(idx++, like);
    }
    std::vector<Call> out;
    while (s.step()) out.push_back(read_call_row(s));
    return out;
}

std::vector<TranscriptSegment> Repository::get_segments(int64_t call_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    Stmt s(db_,
           "SELECT id, call_id, seq, speaker, text, created_at FROM transcript_segments "
           "WHERE call_id = ? ORDER BY seq");
    s.bind(1, call_id);
    std::vector<TranscriptSegment> out;
    while (s.step()) {
        out.push_back(TranscriptSegment{s.int64(0), s.int64(1), s.int64(2), s.text(3), s.text(4), s.text(5)});
    }
    return out;
}

std::string Repository::render_transcript(int64_t call_id) {
    auto segments = get_segments(call_id);
    std::string out;
    for (const auto& seg : segments) {
        if (!out.empty()) out += "\n";
        out += seg.speaker + ": " + seg.text;
    }
    return out;
}

// -- notes --------------------------------------------------------------

int64_t Repository::add_note(int64_t call_id, const std::string& summary, const std::string& email_draft,
                              const std::string& key_points_json) {
    std::lock_guard<std::mutex> lock(mutex_);
    Stmt s(db_,
           "INSERT INTO notes (call_id, summary, email_draft, key_points_json, created_at) "
           "VALUES (?, ?, ?, ?, ?)");
    s.bind(1, call_id).bind(2, summary).bind(3, email_draft).bind(4, key_points_json).bind(5, util::now_iso());
    s.execute();
    return s.last_insert_rowid();
}

std::optional<Note> Repository::get_note_for_call(int64_t call_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    Stmt s(db_,
           "SELECT id, call_id, summary, email_draft, key_points_json, created_at FROM notes "
           "WHERE call_id = ? ORDER BY id DESC LIMIT 1");
    s.bind(1, call_id);
    if (!s.step()) return std::nullopt;
    Note n;
    n.id = s.int64(0);
    n.call_id = s.int64(1);
    n.summary = s.text(2);
    n.email_draft = s.text(3);
    n.key_points_json = s.text(4);
    n.created_at = s.text(5);
    return n;
}

// -- tasks ----------------------------------------------------------------

int64_t Repository::add_task(const std::string& description, std::optional<int64_t> call_id,
                              std::optional<int64_t> customer_id, const std::string& due_date,
                              const std::string& source) {
    std::lock_guard<std::mutex> lock(mutex_);
    Stmt s(db_,
           "INSERT INTO tasks (call_id, customer_id, description, due_date, status, source, created_at) "
           "VALUES (?, ?, ?, ?, 'open', ?, ?)");
    s.bind_opt(1, call_id)
        .bind_opt(2, customer_id)
        .bind(3, description)
        .bind_opt_text(4, due_date)
        .bind(5, source)
        .bind(6, util::now_iso());
    s.execute();
    return s.last_insert_rowid();
}

namespace {
Task read_task_row(Stmt& s) {
    Task t;
    t.id = s.int64(0);
    if (!s.is_null(1)) t.call_id = s.int64(1);
    if (!s.is_null(2)) t.customer_id = s.int64(2);
    t.customer_name = s.text(3);
    t.description = s.text(4);
    t.due_date = s.text(5);
    t.status = s.text(6);
    t.source = s.text(7);
    t.created_at = s.text(8);
    t.completed_at = s.text(9);
    return t;
}
constexpr const char* kTaskColumns =
    "tasks.id, tasks.call_id, tasks.customer_id, customers.name, tasks.description, "
    "tasks.due_date, tasks.status, tasks.source, tasks.created_at, tasks.completed_at ";
}  // namespace

std::vector<Task> Repository::list_tasks(const std::string& status, const std::string& customer_name) {
    std::string sql = std::string("SELECT ") + kTaskColumns +
                       "FROM tasks LEFT JOIN customers ON customers.id = tasks.customer_id WHERE 1=1";
    if (!status.empty()) sql += " AND tasks.status = ?";
    if (!customer_name.empty()) sql += " AND customers.name = ?";
    sql += " ORDER BY (tasks.due_date IS NULL), tasks.due_date, tasks.created_at DESC";

    std::lock_guard<std::mutex> lock(mutex_);
    Stmt s(db_, sql);
    int idx = 1;
    if (!status.empty()) s.bind(idx++, status);
    if (!customer_name.empty()) s.bind(idx++, customer_name);
    std::vector<Task> out;
    while (s.step()) out.push_back(read_task_row(s));
    return out;
}

std::vector<Task> Repository::list_tasks_for_call(int64_t call_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    Stmt s(db_, std::string("SELECT ") + kTaskColumns +
                    "FROM tasks LEFT JOIN customers ON customers.id = tasks.customer_id "
                    "WHERE tasks.call_id = ? ORDER BY tasks.created_at");
    s.bind(1, call_id);
    std::vector<Task> out;
    while (s.step()) out.push_back(read_task_row(s));
    return out;
}

void Repository::complete_task(int64_t task_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    Stmt s(db_, "UPDATE tasks SET status = 'done', completed_at = ? WHERE id = ?");
    s.bind(1, util::now_iso()).bind(2, task_id);
    s.execute();
}

void Repository::reopen_task(int64_t task_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    Stmt s(db_, "UPDATE tasks SET status = 'open', completed_at = NULL WHERE id = ?");
    s.bind(1, task_id);
    s.execute();
}

void Repository::delete_task(int64_t task_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    Stmt s(db_, "DELETE FROM tasks WHERE id = ?");
    s.bind(1, task_id);
    s.execute();
}

std::vector<Task> Repository::due_tasks(const std::string& on_or_before) {
    std::lock_guard<std::mutex> lock(mutex_);
    Stmt s(db_, std::string("SELECT ") + kTaskColumns +
                    "FROM tasks LEFT JOIN customers ON customers.id = tasks.customer_id "
                    "WHERE tasks.status = 'open' AND (tasks.due_date IS NULL OR tasks.due_date <= ?) "
                    "ORDER BY tasks.due_date IS NULL, tasks.due_date");
    s.bind(1, on_or_before);
    std::vector<Task> out;
    while (s.step()) out.push_back(read_task_row(s));
    return out;
}

// -- yearly summary ---------------------------------------------------------

YearlyStats Repository::yearly_stats(int year) {
    std::string start = std::to_string(year) + "-01-01";
    std::string end = std::to_string(year + 1) + "-01-01";

    std::lock_guard<std::mutex> lock(mutex_);
    YearlyStats stats;
    {
        Stmt s(db_,
               "SELECT COUNT(*), COALESCE(SUM(duration_seconds), 0) FROM calls "
               "WHERE started_at >= ? AND started_at < ?");
        s.bind(1, start).bind(2, end);
        if (s.step()) {
            stats.call_count = s.int_(0);
            stats.total_seconds = s.int64(1);
        }
    }
    {
        Stmt s(db_,
               "SELECT customers.name, COUNT(*) AS n FROM calls "
               "JOIN customers ON customers.id = calls.customer_id "
               "WHERE calls.started_at >= ? AND calls.started_at < ? "
               "GROUP BY customers.id ORDER BY n DESC LIMIT 10");
        s.bind(1, start).bind(2, end);
        while (s.step()) stats.top_customers.emplace_back(s.text(0), s.int_(1));
    }
    {
        Stmt s(db_, "SELECT COUNT(*) FROM tasks WHERE status = 'done' AND completed_at >= ? AND completed_at < ?");
        s.bind(1, start).bind(2, end);
        if (s.step()) stats.tasks_completed = s.int_(0);
    }
    {
        Stmt s(db_,
               "SELECT CAST(strftime('%m', started_at) AS INTEGER), COUNT(*) FROM calls "
               "WHERE started_at >= ? AND started_at < ? GROUP BY 1");
        s.bind(1, start).bind(2, end);
        while (s.step()) {
            int month = s.int_(0);  // 1-12
            if (month >= 1 && month <= 12) stats.monthly_call_counts[static_cast<size_t>(month - 1)] = s.int_(1);
        }
    }
    {
        Stmt s(db_,
               "SELECT notes.summary FROM notes JOIN calls ON calls.id = notes.call_id "
               "WHERE calls.started_at >= ? AND calls.started_at < ? ORDER BY calls.started_at");
        s.bind(1, start).bind(2, end);
        while (s.step()) {
            std::string summary = s.text(0);
            if (!summary.empty()) stats.summaries.push_back(summary);
        }
    }
    return stats;
}

}  // namespace callscribe::db
