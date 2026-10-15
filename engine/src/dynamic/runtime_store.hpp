#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>

#include "core/json.hpp"
#include "db/sqlite.hpp"

namespace proj::dynamic {

struct Retention {
  int64_t max_traces = 5000;    // newest kept
  int64_t max_errors = 1000;    // most recently seen distinct errors kept
  int64_t max_sessions = 200;   // newest kept, with their traces
};

// Runtime observations of one project: agent sessions, the methods they ran, call trees,
// aggregated caller -> callee counts and exceptions. A separate SQLite file next to the static
// index (runtime/project-N.db): analysis and refresh never touch it, and it never writes the
// static index. Observations use runtime identities (class, method, JVM descriptor); they are
// mapped to static symbols when queried, so a re-analysis does not invalidate them.
class RuntimeStore {
 public:
  static constexpr int kSchemaVersion = 1;

  explicit RuntimeStore(const std::filesystem::path& path, Retention retention = {});

  // A session per agent load (one JVM; a re-attach is a new session). Reconnects of the same
  // session reuse it. `context` carries index_identity and source_revision.
  int64_t open_session(const json& hello, const json& context);
  void session_stats(int64_t session, const json& stats);
  void close_session(int64_t session, const std::string& status, const std::string& reason);
  // Sessions a previous service left "capturing" (it stopped or crashed): marked interrupted.
  int64_t mark_interrupted(const std::string& reason);

  void method(int64_t session, const json& def);
  // Stores a trace (an "open" snapshot is replaced by later ones). Returns false when a final
  // trace was already stored (replay): nothing is counted twice.
  bool trace(int64_t session, const json& msg);
  void uncaught(int64_t session, const json& msg);

  void begin();
  void commit();
  bool in_batch() const { return batch_; }
  void retain();

  db::Database& db() { return db_; }

 private:
  db::Database db_;
  Retention retention_;
  bool batch_ = false;
  // Agent method id -> rt_methods.id, per session (agent ids are per JVM).
  std::unordered_map<int64_t, std::unordered_map<int64_t, int64_t>> methods_;

  int64_t method_id(int64_t session, int64_t agent_id);
  json map_site(int64_t session, const json& site);
  json map_error(int64_t session, json error);
  void insert_error(int64_t session, int64_t trace_id, const std::string& thread, const json& error);
};

// Creates or upgrades the runtime schema; readers check runtime_schema_current().
void migrate_runtime(db::Database& db);
bool runtime_schema_current(db::Database& db);

}  // namespace proj::dynamic
