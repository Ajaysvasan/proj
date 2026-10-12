#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/json.hpp"
#include "db/sqlite.hpp"

namespace proj {

struct ProjectRecord {
  int64_t id = 0;
  std::string name;
  std::string root_path;
  std::string index_path;
  std::string created_at;
  std::optional<std::string> last_analysis_at;
  std::optional<std::string> last_success_at;
  std::string status;
  std::optional<int64_t> index_schema_version;
  std::string tool_version;
  std::optional<int64_t> last_run_id;
  json options = json::object();

  json to_json() const;
};

struct Diagnostic {
  std::string level;     // debug | info | warn | error
  std::string category;  // source | resource | config | internal
  std::string code;      // stable machine-readable code, e.g. "syntax_error"
  std::string path;      // project-relative path, may be empty
  int64_t line = 0;      // 0 when not applicable
  std::string message;
};

// Collects diagnostics for one run while keeping memory bounded: at most
// `capacity` warnings/errors and capacity/4 debug/info entries are kept, but
// every entry is counted.
class DiagnosticSink {
 public:
  explicit DiagnosticSink(size_t capacity) : capacity_(capacity) {}
  void add(Diagnostic d);
  const std::vector<Diagnostic>& kept() const { return kept_; }
  int64_t total() const { return total_; }
  int64_t dropped() const { return total_ - static_cast<int64_t>(kept_.size()); }
  int64_t count_level(const std::string& level) const;

 private:
  size_t capacity_;
  std::vector<Diagnostic> kept_;
  size_t kept_important_ = 0;
  size_t kept_minor_ = 0;
  int64_t total_ = 0;
  int64_t warnings_ = 0;
  int64_t errors_ = 0;
};

struct RunCounts {
  int64_t files_discovered = 0;
  int64_t files_parsed = 0;
  int64_t files_skipped = 0;
  int64_t symbols = 0;
  int64_t relationships = 0;
  int64_t unresolved_refs = 0;
  int64_t parse_errors = 0;
  int64_t warnings = 0;
  int64_t fatal_errors = 0;
};

// Shared registry database: projects, analysis runs and run diagnostics.
class Registry {
 public:
  static constexpr int kRunsKeptPerProject = 50;

  explicit Registry(const std::filesystem::path& data_dir);

  const std::filesystem::path& data_dir() const { return data_dir_; }
  std::filesystem::path index_dir() const { return data_dir_ / "indexes"; }

  std::optional<ProjectRecord> find_by_name(const std::string& name);
  std::optional<ProjectRecord> find_by_root(const std::string& root);
  std::vector<ProjectRecord> list_projects();
  ProjectRecord create_project(const std::string& name, const std::string& root, const json& options);
  void update_project_options(int64_t project_id, const json& options);

  // Marks runs left in "running" state by processes that no longer exist.
  void mark_interrupted_runs();
  // Returns the id of a live run for the project, if one is in progress.
  std::optional<int64_t> active_run(int64_t project_id);

  int64_t start_run(const ProjectRecord& project);
  void finish_run(int64_t run_id, int64_t project_id, const std::string& status, double duration_ms,
                  const RunCounts& counts, const json& stats, const std::string& error_message,
                  const DiagnosticSink& diagnostics, bool index_replaced);

  json list_runs(const std::optional<int64_t>& project_id, int64_t limit);
  std::optional<json> get_run(int64_t run_id, int64_t diag_limit, int64_t diag_offset);

 private:
  std::filesystem::path data_dir_;
  db::Database db_;

  void prune_runs(int64_t project_id);
};

}  // namespace proj
