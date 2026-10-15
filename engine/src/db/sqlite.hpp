#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

struct sqlite3;
struct sqlite3_stmt;

// Minimal RAII wrapper over the SQLite C API. All SQL that involves values
// goes through prepared statements with bound parameters.
namespace proj::db {

class Error : public std::runtime_error {
 public:
  Error(int code, const std::string& message) : std::runtime_error(message), code_(code) {}
  int code() const { return code_; }

 private:
  int code_;
};

class Statement {
 public:
  Statement() = default;
  Statement(sqlite3* db, std::string_view sql);
  ~Statement();
  Statement(Statement&& other) noexcept;
  Statement& operator=(Statement&& other) noexcept;
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;

  // Parameter indexes are 1-based, as in SQLite.
  Statement& bind(int index, int64_t value);
  Statement& bind(int index, int value) { return bind(index, static_cast<int64_t>(value)); }
  Statement& bind(int index, uint32_t value) { return bind(index, static_cast<int64_t>(value)); }
  Statement& bind(int index, double value);
  Statement& bind(int index, bool value) { return bind(index, static_cast<int64_t>(value ? 1 : 0)); }
  Statement& bind(int index, std::string_view value);
  Statement& bind(int index, const char* value) { return bind(index, std::string_view(value)); }
  Statement& bind(int index, const std::string& value) { return bind(index, std::string_view(value)); }
  Statement& bind(int index, std::nullptr_t);
  Statement& bind(int index, const std::optional<int64_t>& value);
  Statement& bind(int index, const std::optional<std::string>& value);
  // Binds NULL for an empty string.
  Statement& bind_text_or_null(int index, std::string_view value);
  // Binds NULL for a negative id.
  Statement& bind_id_or_null(int index, int64_t value);

  // Advances the statement. Returns true when a row is available.
  bool step();
  // Steps to completion (for INSERT/UPDATE/DELETE), then resets.
  void run();
  void reset();

  bool is_null(int col) const;
  int64_t int64(int col) const;
  double real(int col) const;
  std::string text(int col) const;
  std::optional<int64_t> opt_int64(int col) const;
  std::optional<std::string> opt_text(int col) const;

  // Rows stepped through in full table scans since the statement was prepared.
  int64_t full_scan_steps() const;

 private:
  sqlite3* db_ = nullptr;
  sqlite3_stmt* stmt_ = nullptr;
  void check(int rc, const char* what) const;
};

enum class OpenMode { ReadOnly, ReadWrite, ReadWriteCreate };

class Database {
 public:
  Database() = default;
  Database(const std::filesystem::path& path, OpenMode mode);
  ~Database();
  Database(Database&& other) noexcept;
  Database& operator=(Database&& other) noexcept;
  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;

  bool is_open() const { return db_ != nullptr; }
  void close();
  void exec(std::string_view sql);
  Statement prepare(std::string_view sql);
  int64_t last_insert_rowid() const;
  int changes() const;
  sqlite3* handle() const { return db_; }
  // Convenience for single-value queries; returns nullopt if no row.
  std::optional<int64_t> query_int(std::string_view sql);

 private:
  sqlite3* db_ = nullptr;
};

// BEGIN IMMEDIATE ... COMMIT; rolls back automatically unless committed.
class Transaction {
 public:
  explicit Transaction(Database& db);
  ~Transaction();
  Transaction(const Transaction&) = delete;
  Transaction& operator=(const Transaction&) = delete;
  void commit();

 private:
  Database& db_;
  bool done_ = false;
};

}  // namespace proj::db
