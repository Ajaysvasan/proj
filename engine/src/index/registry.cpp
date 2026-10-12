#include "index/registry.hpp"

#include "core/sysinfo.hpp"
#include "core/time.hpp"
#include "core/version.hpp"
#include "db/schema.hpp"

namespace proj {
namespace {

constexpr const char* kProjectColumns =
    "id, name, root_path, index_path, created_at, last_analysis_at, last_success_at, status, "
    "index_schema_version, tool_version, last_run_id, options_json";

ProjectRecord read_project(const db::Statement& st) {
  ProjectRecord p;
  p.id = st.int64(0);
  p.name = st.text(1);
  p.root_path = st.text(2);
  p.index_path = st.text(3);
  p.created_at = st.text(4);
  p.last_analysis_at = st.opt_text(5);
  p.last_success_at = st.opt_text(6);
  p.status = st.text(7);
  p.index_schema_version = st.opt_int64(8);
  p.tool_version = st.text(9);
  p.last_run_id = st.opt_int64(10);
  p.options = json::parse(st.text(11), nullptr, false);
  if (p.options.is_discarded()) p.options = json::object();
  return p;
}

json opt(const std::optional<std::string>& v) { return v ? json(*v) : json(nullptr); }
json opt(const std::optional<int64_t>& v) { return v ? json(*v) : json(nullptr); }

json run_row_to_json(const db::Statement& st) {
  json stats = json::parse(st.text(19), nullptr, false);
  if (stats.is_discarded()) stats = json::object();
  return {{"id", st.int64(0)},
          {"project_id", st.int64(1)},
          {"project", st.text(2)},
          {"started_at", st.text(3)},
          {"finished_at", opt(st.opt_text(4))},
          {"duration_ms", st.is_null(5) ? json(nullptr) : json(st.real(5))},
          {"status", st.text(6)},
          {"tool_version", st.text(7)},
          {"index_schema_version", st.int64(8)},
          {"counts",
           {{"files_discovered", st.int64(9)},
            {"files_parsed", st.int64(10)},
            {"files_skipped", st.int64(11)},
            {"symbols", st.int64(12)},
            {"relationships", st.int64(13)},
            {"unresolved_refs", st.int64(14)},
            {"parse_errors", st.int64(15)},
            {"warnings", st.int64(16)},
            {"fatal_errors", st.int64(17)}}},
          {"error_message", opt(st.opt_text(18))},
          {"stats", stats},
          {"diagnostics_total", st.int64(20)},
          {"diagnostics_dropped", st.int64(21)}};
}

constexpr const char* kRunColumns =
    "id, project_id, project_name, started_at, finished_at, duration_ms, status, tool_version, "
    "index_schema_version, files_discovered, files_parsed, files_skipped, symbols, relationships, "
    "unresolved_refs, parse_errors, warnings, fatal_errors, error_message, stats_json, "
    "diagnostics_total, diagnostics_dropped";

}  // namespace

json ProjectRecord::to_json() const {
  return {{"id", id},
          {"name", name},
          {"root_path", root_path},
          {"index_path", index_path},
          {"created_at", created_at},
          {"last_analysis_at", opt(last_analysis_at)},
          {"last_success_at", opt(last_success_at)},
          {"status", status},
          {"index_schema_version", opt(index_schema_version)},
          {"tool_version", tool_version},
          {"last_run_id", opt(last_run_id)},
          {"options", options}};
}

void DiagnosticSink::add(Diagnostic d) {
  ++total_;
  if (d.level == "warn") ++warnings_;
  if (d.level == "error") ++errors_;
  bool important = d.level == "warn" || d.level == "error";
  size_t& used = important ? kept_important_ : kept_minor_;
  size_t cap = important ? capacity_ : capacity_ / 4;
  if (used < cap) {
    kept_.push_back(std::move(d));
    ++used;
  }
}

int64_t DiagnosticSink::count_level(const std::string& level) const {
  if (level == "warn") return warnings_;
  if (level == "error") return errors_;
  int64_t n = 0;
  for (const auto& d : kept_) n += d.level == level;
  return n;
}

Registry::Registry(const std::filesystem::path& data_dir) : data_dir_(data_dir) {
  std::filesystem::create_directories(data_dir_);
  std::filesystem::create_directories(index_dir());
  db_ = db::Database(data_dir_ / "registry.db", db::OpenMode::ReadWriteCreate);
  db::migrate_registry(db_);
}

std::optional<ProjectRecord> Registry::find_by_name(const std::string& name) {
  auto st = db_.prepare(std::string("SELECT ") + kProjectColumns + " FROM projects WHERE name = ?");
  st.bind(1, name);
  if (!st.step()) return std::nullopt;
  return read_project(st);
}

std::optional<ProjectRecord> Registry::find_by_root(const std::string& root) {
  auto st = db_.prepare(std::string("SELECT ") + kProjectColumns + " FROM projects WHERE root_path = ?");
  st.bind(1, root);
  if (!st.step()) return std::nullopt;
  return read_project(st);
}

std::vector<ProjectRecord> Registry::list_projects() {
  auto st = db_.prepare(std::string("SELECT ") + kProjectColumns + " FROM projects ORDER BY name");
  std::vector<ProjectRecord> out;
  while (st.step()) out.push_back(read_project(st));
  return out;
}

ProjectRecord Registry::create_project(const std::string& name, const std::string& root, const json& options) {
  db::Transaction tx(db_);
  auto st = db_.prepare(
      "INSERT INTO projects(name, root_path, index_path, created_at, status, tool_version, options_json) "
      "VALUES (?, ?, '', ?, 'registered', ?, ?)");
  st.bind(1, name).bind(2, root).bind(3, iso_now()).bind(4, kToolVersion).bind(5, options.dump());
  st.run();
  int64_t id = db_.last_insert_rowid();
  // Index files are named by project id so project names never become paths.
  auto index_path = (index_dir() / ("project-" + std::to_string(id) + ".db")).string();
  auto up = db_.prepare("UPDATE projects SET index_path = ? WHERE id = ?");
  up.bind(1, index_path).bind(2, id).run();
  tx.commit();
  return *find_by_name(name);
}

void Registry::update_project_options(int64_t project_id, const json& options) {
  auto st = db_.prepare("UPDATE projects SET options_json = ? WHERE id = ?");
  st.bind(1, options.dump()).bind(2, project_id).run();
}

void Registry::mark_interrupted_runs() {
  auto st = db_.prepare("SELECT id, project_id, pid FROM analysis_runs WHERE status = 'running'");
  std::vector<std::pair<int64_t, int64_t>> stale;
  while (st.step()) {
    if (!process_alive(st.int64(2))) stale.emplace_back(st.int64(0), st.int64(1));
  }
  for (auto [run_id, project_id] : stale) {
    auto up = db_.prepare(
        "UPDATE analysis_runs SET status = 'interrupted', error_message = "
        "'analysis process exited before recording a result (crash or forced termination)' WHERE id = ?");
    up.bind(1, run_id).run();
    auto pp = db_.prepare("UPDATE projects SET status = 'interrupted' WHERE id = ? AND last_run_id = ?");
    pp.bind(1, project_id).bind(2, run_id).run();
  }
}

std::optional<int64_t> Registry::active_run(int64_t project_id) {
  auto st = db_.prepare("SELECT id, pid FROM analysis_runs WHERE project_id = ? AND status = 'running'");
  st.bind(1, project_id);
  while (st.step()) {
    if (process_alive(st.int64(1))) return st.int64(0);
  }
  return std::nullopt;
}

int64_t Registry::start_run(const ProjectRecord& project) {
  db::Transaction tx(db_);
  auto st = db_.prepare(
      "INSERT INTO analysis_runs(project_id, project_name, started_at, status, pid, tool_version, "
      "index_schema_version) VALUES (?, ?, ?, 'running', ?, ?, ?)");
  auto now = iso_now();
  st.bind(1, project.id).bind(2, project.name).bind(3, now).bind(4, current_pid()).bind(5, kToolVersion);
  st.bind(6, static_cast<int64_t>(kIndexSchemaVersion));
  st.run();
  int64_t run_id = db_.last_insert_rowid();
  auto up = db_.prepare(
      "UPDATE projects SET status = 'analyzing', last_analysis_at = ?, last_run_id = ? WHERE id = ?");
  up.bind(1, now).bind(2, run_id).bind(3, project.id).run();
  tx.commit();
  return run_id;
}

void Registry::finish_run(int64_t run_id, int64_t project_id, const std::string& status, double duration_ms,
                          const RunCounts& c, const json& stats, const std::string& error_message,
                          const DiagnosticSink& diagnostics, bool index_replaced) {
  db::Transaction tx(db_);
  auto now = iso_now();
  auto st = db_.prepare(
      "UPDATE analysis_runs SET finished_at = ?, duration_ms = ?, status = ?, files_discovered = ?, "
      "files_parsed = ?, files_skipped = ?, symbols = ?, relationships = ?, unresolved_refs = ?, "
      "parse_errors = ?, warnings = ?, fatal_errors = ?, stats_json = ?, error_message = ?, "
      "diagnostics_total = ?, diagnostics_dropped = ? WHERE id = ?");
  st.bind(1, now).bind(2, duration_ms).bind(3, status);
  st.bind(4, c.files_discovered).bind(5, c.files_parsed).bind(6, c.files_skipped).bind(7, c.symbols);
  st.bind(8, c.relationships).bind(9, c.unresolved_refs).bind(10, c.parse_errors).bind(11, c.warnings);
  st.bind(12, c.fatal_errors).bind(13, stats.dump(-1, ' ', false, json::error_handler_t::replace));
  st.bind_text_or_null(14, error_message);
  st.bind(15, diagnostics.total()).bind(16, diagnostics.dropped()).bind(17, run_id);
  st.run();

  auto ins = db_.prepare(
      "INSERT INTO run_diagnostics(run_id, level, category, code, path, line, message) VALUES (?,?,?,?,?,?,?)");
  for (const auto& d : diagnostics.kept()) {
    ins.bind(1, run_id).bind(2, d.level).bind(3, d.category).bind(4, d.code);
    ins.bind_text_or_null(5, d.path);
    if (d.line > 0) ins.bind(6, d.line); else ins.bind(6, nullptr);
    ins.bind(7, d.message).run();
  }

  if (index_replaced) {
    auto up = db_.prepare(
        "UPDATE projects SET status = ?, last_success_at = ?, index_schema_version = ?, tool_version = ? "
        "WHERE id = ?");
    up.bind(1, status).bind(2, now).bind(3, static_cast<int64_t>(kIndexSchemaVersion)).bind(4, kToolVersion);
    up.bind(5, project_id).run();
  } else {
    auto up = db_.prepare("UPDATE projects SET status = ? WHERE id = ?");
    up.bind(1, status).bind(2, project_id).run();
  }
  tx.commit();
  prune_runs(project_id);
}

void Registry::prune_runs(int64_t project_id) {
  db::Transaction tx(db_);
  auto del_diag = db_.prepare(
      "DELETE FROM run_diagnostics WHERE run_id IN (SELECT id FROM analysis_runs WHERE project_id = ?1 "
      "AND id NOT IN (SELECT id FROM analysis_runs WHERE project_id = ?1 ORDER BY id DESC LIMIT ?2))");
  del_diag.bind(1, project_id).bind(2, static_cast<int64_t>(kRunsKeptPerProject)).run();
  auto del_runs = db_.prepare(
      "DELETE FROM analysis_runs WHERE project_id = ?1 AND id NOT IN "
      "(SELECT id FROM analysis_runs WHERE project_id = ?1 ORDER BY id DESC LIMIT ?2)");
  del_runs.bind(1, project_id).bind(2, static_cast<int64_t>(kRunsKeptPerProject)).run();
  tx.commit();
}

json Registry::list_runs(const std::optional<int64_t>& project_id, int64_t limit) {
  std::string sql = std::string("SELECT ") + kRunColumns + " FROM analysis_runs";
  if (project_id) sql += " WHERE project_id = ?1";
  sql += " ORDER BY id DESC LIMIT ?2";
  auto st = db_.prepare(sql);
  if (project_id) st.bind(1, *project_id);
  st.bind(2, limit);
  json runs = json::array();
  while (st.step()) runs.push_back(run_row_to_json(st));
  return runs;
}

std::optional<json> Registry::get_run(int64_t run_id, int64_t diag_limit, int64_t diag_offset) {
  auto st = db_.prepare(std::string("SELECT ") + kRunColumns + " FROM analysis_runs WHERE id = ?");
  st.bind(1, run_id);
  if (!st.step()) return std::nullopt;
  json run = run_row_to_json(st);
  auto ds = db_.prepare(
      "SELECT level, category, code, path, line, message FROM run_diagnostics WHERE run_id = ? "
      "ORDER BY CASE level WHEN 'error' THEN 0 WHEN 'warn' THEN 1 WHEN 'info' THEN 2 ELSE 3 END, id "
      "LIMIT ? OFFSET ?");
  ds.bind(1, run_id).bind(2, diag_limit).bind(3, diag_offset);
  json diags = json::array();
  while (ds.step()) {
    diags.push_back({{"level", ds.text(0)},
                     {"category", ds.text(1)},
                     {"code", ds.text(2)},
                     {"path", opt(ds.opt_text(3))},
                     {"line", opt(ds.opt_int64(4))},
                     {"message", ds.text(5)}});
  }
  run["diagnostics"] = diags;
  auto counts = db_.prepare(
      "SELECT category, level, COUNT(*) FROM run_diagnostics WHERE run_id = ? GROUP BY category, level "
      "ORDER BY category, level");
  counts.bind(1, run_id);
  json by = json::array();
  while (counts.step()) by.push_back({{"category", counts.text(0)}, {"level", counts.text(1)}, {"count", counts.int64(2)}});
  run["diagnostic_counts"] = by;
  return run;
}

}  // namespace proj
