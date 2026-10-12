#include "db/sqlite.hpp"

#include <sqlite3.h>

#include <utility>

namespace proj::db {

Statement::Statement(sqlite3* db, std::string_view sql) : db_(db) {
  int rc = sqlite3_prepare_v3(db, sql.data(), static_cast<int>(sql.size()), SQLITE_PREPARE_PERSISTENT,
                              &stmt_, nullptr);
  if (rc != SQLITE_OK) {
    std::string msg = "SQL prepare failed: ";
    msg += sqlite3_errmsg(db);
    throw Error(rc, msg);
  }
}

Statement::~Statement() {
  if (stmt_) sqlite3_finalize(stmt_);
}

Statement::Statement(Statement&& other) noexcept
    : db_(std::exchange(other.db_, nullptr)), stmt_(std::exchange(other.stmt_, nullptr)) {}

Statement& Statement::operator=(Statement&& other) noexcept {
  if (this != &other) {
    if (stmt_) sqlite3_finalize(stmt_);
    db_ = std::exchange(other.db_, nullptr);
    stmt_ = std::exchange(other.stmt_, nullptr);
  }
  return *this;
}

void Statement::check(int rc, const char* what) const {
  if (rc != SQLITE_OK) {
    std::string msg = std::string(what) + ": " + sqlite3_errmsg(db_);
    throw Error(rc, msg);
  }
}

Statement& Statement::bind(int index, int64_t value) {
  check(sqlite3_bind_int64(stmt_, index, value), "bind int");
  return *this;
}

Statement& Statement::bind(int index, double value) {
  check(sqlite3_bind_double(stmt_, index, value), "bind double");
  return *this;
}

Statement& Statement::bind(int index, std::string_view value) {
  check(sqlite3_bind_text64(stmt_, index, value.data(), value.size(), SQLITE_TRANSIENT, SQLITE_UTF8),
        "bind text");
  return *this;
}

Statement& Statement::bind(int index, std::nullptr_t) {
  check(sqlite3_bind_null(stmt_, index), "bind null");
  return *this;
}

Statement& Statement::bind(int index, const std::optional<int64_t>& value) {
  return value ? bind(index, *value) : bind(index, nullptr);
}

Statement& Statement::bind(int index, const std::optional<std::string>& value) {
  return value ? bind(index, std::string_view(*value)) : bind(index, nullptr);
}

Statement& Statement::bind_text_or_null(int index, std::string_view value) {
  return value.empty() ? bind(index, nullptr) : bind(index, value);
}

Statement& Statement::bind_id_or_null(int index, int64_t value) {
  return value < 0 ? bind(index, nullptr) : bind(index, value);
}

bool Statement::step() {
  int rc = sqlite3_step(stmt_);
  if (rc == SQLITE_ROW) return true;
  if (rc == SQLITE_DONE) return false;
  std::string msg = std::string("SQL step failed: ") + sqlite3_errmsg(db_);
  sqlite3_reset(stmt_);
  throw Error(rc, msg);
}

void Statement::run() {
  while (step()) {
  }
  reset();
}

void Statement::reset() {
  sqlite3_reset(stmt_);
  sqlite3_clear_bindings(stmt_);
}

bool Statement::is_null(int col) const { return sqlite3_column_type(stmt_, col) == SQLITE_NULL; }

int64_t Statement::int64(int col) const { return sqlite3_column_int64(stmt_, col); }

double Statement::real(int col) const { return sqlite3_column_double(stmt_, col); }

std::string Statement::text(int col) const {
  const unsigned char* p = sqlite3_column_text(stmt_, col);
  if (!p) return {};
  return std::string(reinterpret_cast<const char*>(p), static_cast<size_t>(sqlite3_column_bytes(stmt_, col)));
}

std::optional<int64_t> Statement::opt_int64(int col) const {
  if (is_null(col)) return std::nullopt;
  return int64(col);
}

std::optional<std::string> Statement::opt_text(int col) const {
  if (is_null(col)) return std::nullopt;
  return text(col);
}

Database::Database(const std::filesystem::path& path, OpenMode mode) {
  int flags = SQLITE_OPEN_NOMUTEX;
  switch (mode) {
    case OpenMode::ReadOnly: flags |= SQLITE_OPEN_READONLY; break;
    case OpenMode::ReadWrite: flags |= SQLITE_OPEN_READWRITE; break;
    case OpenMode::ReadWriteCreate: flags |= SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE; break;
  }
  int rc = sqlite3_open_v2(path.string().c_str(), &db_, flags, nullptr);
  if (rc != SQLITE_OK) {
    std::string msg = "cannot open database " + path.string() + ": " +
                      (db_ ? sqlite3_errmsg(db_) : sqlite3_errstr(rc));
    if (db_) sqlite3_close(db_);
    db_ = nullptr;
    throw Error(rc, msg);
  }
  sqlite3_extended_result_codes(db_, 1);
  sqlite3_busy_timeout(db_, 5000);
}

Database::~Database() { close(); }

Database::Database(Database&& other) noexcept : db_(std::exchange(other.db_, nullptr)) {}

Database& Database::operator=(Database&& other) noexcept {
  if (this != &other) {
    close();
    db_ = std::exchange(other.db_, nullptr);
  }
  return *this;
}

void Database::close() {
  if (db_) {
    sqlite3_close_v2(db_);
    db_ = nullptr;
  }
}

void Database::exec(std::string_view sql) {
  std::string s(sql);
  char* err = nullptr;
  int rc = sqlite3_exec(db_, s.c_str(), nullptr, nullptr, &err);
  if (rc != SQLITE_OK) {
    std::string msg = std::string("SQL exec failed: ") + (err ? err : sqlite3_errmsg(db_));
    sqlite3_free(err);
    throw Error(rc, msg);
  }
}

Statement Database::prepare(std::string_view sql) { return Statement(db_, sql); }

int64_t Database::last_insert_rowid() const { return sqlite3_last_insert_rowid(db_); }

int Database::changes() const { return sqlite3_changes(db_); }

std::optional<int64_t> Database::query_int(std::string_view sql) {
  Statement st(db_, sql);
  if (!st.step() || st.is_null(0)) return std::nullopt;
  return st.int64(0);
}

Transaction::Transaction(Database& db) : db_(db) { db_.exec("BEGIN IMMEDIATE"); }

Transaction::~Transaction() {
  if (!done_) {
    try {
      db_.exec("ROLLBACK");
    } catch (...) {
      // Rollback failure during unwinding cannot be reported further; the
      // original exception is already propagating.
    }
  }
}

void Transaction::commit() {
  db_.exec("COMMIT");
  done_ = true;
}

}  // namespace proj::db
