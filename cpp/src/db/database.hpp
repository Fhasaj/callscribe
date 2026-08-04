#pragma once
#include <sqlite3.h>

#include <cstdint>
#include <optional>
#include <string>

namespace callscribe::db {

// Opens (creating if needed) the SQLite DB at `path` and applies the schema.
class Database {
public:
    explicit Database(const std::string& path);
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    sqlite3* handle() const { return db_; }

private:
    sqlite3* db_ = nullptr;
};

// RAII prepared-statement wrapper with small bind/step/column helpers so
// repository.cpp doesn't repeat raw sqlite3 C API boilerplate everywhere.
class Stmt {
public:
    Stmt(sqlite3* db, const std::string& sql);
    ~Stmt();
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    Stmt& bind(int idx, const std::string& value);
    Stmt& bind(int idx, int64_t value);
    Stmt& bind(int idx, int value);
    Stmt& bind_null(int idx);
    // Nullable FK: bind int64 if present, else NULL.
    Stmt& bind_opt(int idx, const std::optional<int64_t>& value);
    // Nullable text: bind the string if non-empty, else NULL.
    Stmt& bind_opt_text(int idx, const std::string& value);

    bool step();     // true if a row is available (SQLITE_ROW)
    void execute();  // runs to completion; for INSERT/UPDATE/DELETE

    std::string text(int col) const;    // "" if NULL
    int64_t int64(int col) const;       // 0 if NULL
    int int_(int col) const;            // 0 if NULL
    bool is_null(int col) const;
    int64_t last_insert_rowid() const;

private:
    sqlite3* db_;
    sqlite3_stmt* stmt_ = nullptr;
};

}  // namespace callscribe::db
