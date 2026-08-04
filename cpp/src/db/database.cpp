#include "db/database.hpp"

#include <filesystem>
#include <stdexcept>

namespace callscribe::db {

namespace {
constexpr const char* kSchema = R"sql(
CREATE TABLE IF NOT EXISTS customers (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL UNIQUE,
    created_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS calls (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    customer_id INTEGER REFERENCES customers(id),
    meeting_name TEXT,
    participants TEXT,
    started_at TEXT NOT NULL,
    ended_at TEXT,
    duration_seconds INTEGER,
    audio_path TEXT,
    status TEXT NOT NULL DEFAULT 'recording',
    manual_notes TEXT
);

CREATE TABLE IF NOT EXISTS transcript_segments (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    call_id INTEGER NOT NULL REFERENCES calls(id),
    seq INTEGER NOT NULL,
    speaker TEXT NOT NULL,
    text TEXT NOT NULL,
    created_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS notes (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    call_id INTEGER NOT NULL REFERENCES calls(id),
    summary TEXT,
    email_draft TEXT,
    key_points_json TEXT,
    created_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS tasks (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    call_id INTEGER REFERENCES calls(id),
    customer_id INTEGER REFERENCES customers(id),
    description TEXT NOT NULL,
    due_date TEXT,
    status TEXT NOT NULL DEFAULT 'open',
    source TEXT NOT NULL DEFAULT 'manual',
    created_at TEXT NOT NULL,
    completed_at TEXT
);

CREATE INDEX IF NOT EXISTS idx_calls_customer ON calls(customer_id);
CREATE INDEX IF NOT EXISTS idx_calls_started ON calls(started_at);
CREATE INDEX IF NOT EXISTS idx_segments_call ON transcript_segments(call_id, seq);
CREATE INDEX IF NOT EXISTS idx_notes_call ON notes(call_id);
CREATE INDEX IF NOT EXISTS idx_tasks_status ON tasks(status);
CREATE INDEX IF NOT EXISTS idx_tasks_customer ON tasks(customer_id);
)sql";
}  // namespace

Database::Database(const std::string& path) {
    std::filesystem::path p(path);
    if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path());
    }
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        std::string msg = db_ ? sqlite3_errmsg(db_) : "unknown error";
        if (db_) sqlite3_close(db_);
        throw std::runtime_error("Failed to open database '" + path + "': " + msg);
    }
    sqlite3_exec(db_, "PRAGMA foreign_keys = ON", nullptr, nullptr, nullptr);

    char* err = nullptr;
    if (sqlite3_exec(db_, kSchema, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "unknown error";
        sqlite3_free(err);
        sqlite3_close(db_);
        throw std::runtime_error("Failed to apply schema: " + msg);
    }

    // Lightweight migration for DBs created before manual_notes existed --
    // CREATE TABLE IF NOT EXISTS above is a no-op on an existing table, so
    // add the column here; ignore the error if it's already present.
    sqlite3_exec(db_, "ALTER TABLE calls ADD COLUMN manual_notes TEXT", nullptr, nullptr, nullptr);
}

Database::~Database() {
    if (db_) sqlite3_close(db_);
}

Stmt::Stmt(sqlite3* db, const std::string& sql) : db_(db) {
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt_, nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("Failed to prepare statement: ") + sqlite3_errmsg(db_));
    }
}

Stmt::~Stmt() {
    if (stmt_) sqlite3_finalize(stmt_);
}

Stmt& Stmt::bind(int idx, const std::string& value) {
    sqlite3_bind_text(stmt_, idx, value.c_str(), -1, SQLITE_TRANSIENT);
    return *this;
}

Stmt& Stmt::bind(int idx, int64_t value) {
    sqlite3_bind_int64(stmt_, idx, value);
    return *this;
}

Stmt& Stmt::bind(int idx, int value) {
    sqlite3_bind_int(stmt_, idx, value);
    return *this;
}

Stmt& Stmt::bind_null(int idx) {
    sqlite3_bind_null(stmt_, idx);
    return *this;
}

Stmt& Stmt::bind_opt(int idx, const std::optional<int64_t>& value) {
    if (value.has_value()) {
        sqlite3_bind_int64(stmt_, idx, *value);
    } else {
        sqlite3_bind_null(stmt_, idx);
    }
    return *this;
}

Stmt& Stmt::bind_opt_text(int idx, const std::string& value) {
    if (value.empty()) {
        sqlite3_bind_null(stmt_, idx);
    } else {
        sqlite3_bind_text(stmt_, idx, value.c_str(), -1, SQLITE_TRANSIENT);
    }
    return *this;
}

bool Stmt::step() {
    int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    throw std::runtime_error(std::string("sqlite3_step failed: ") + sqlite3_errmsg(db_));
}

void Stmt::execute() {
    step();
}

std::string Stmt::text(int col) const {
    const unsigned char* v = sqlite3_column_text(stmt_, col);
    return v ? reinterpret_cast<const char*>(v) : "";
}

int64_t Stmt::int64(int col) const {
    return sqlite3_column_int64(stmt_, col);
}

int Stmt::int_(int col) const {
    return sqlite3_column_int(stmt_, col);
}

bool Stmt::is_null(int col) const {
    return sqlite3_column_type(stmt_, col) == SQLITE_NULL;
}

int64_t Stmt::last_insert_rowid() const {
    return sqlite3_last_insert_rowid(db_);
}

}  // namespace callscribe::db
