#include "index/refresh.hpp"

#include <cctype>
#include <deque>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/log.hpp"
#include "core/time.hpp"
#include "index/resolver.hpp"
#include "index/store.hpp"

namespace proj {
namespace {

const char* kTempSchema = R"SQL(
CREATE TEMP TABLE refresh_seen(id INTEGER PRIMARY KEY);
CREATE TEMP TABLE refresh_touched(id INTEGER PRIMARY KEY);
CREATE TEMP TABLE refresh_removed(id INTEGER PRIMARY KEY);
CREATE TEMP TABLE refresh_deleted_symbols(id INTEGER PRIMARY KEY);
CREATE TEMP TABLE refresh_old_sig(file_id INTEGER, name TEXT, sig TEXT);
CREATE TEMP TABLE refresh_new_sig(file_id INTEGER, name TEXT, sig TEXT);
CREATE TEMP TABLE refresh_old_iface(file_id INTEGER, item TEXT);
CREATE TEMP TABLE refresh_new_iface(file_id INTEGER, item TEXT);
CREATE TEMP TABLE refresh_names(name TEXT PRIMARY KEY);
CREATE TEMP TABLE refresh_affected(id INTEGER PRIMARY KEY);
CREATE TEMP TABLE refresh_reset_edges(id INTEGER PRIMARY KEY);
CREATE TEMP TABLE refresh_seeds(id INTEGER PRIMARY KEY);
CREATE TEMP TABLE refresh_old_deps(id INTEGER PRIMARY KEY, src INTEGER, dst INTEGER, status TEXT);
CREATE INDEX temp.refresh_old_deps_dst ON refresh_old_deps(dst);
CREATE TEMP TABLE refresh_old_bindings(id INTEGER PRIMARY KEY, file INTEGER, kind TEXT, dst INTEGER);
)SQL";

std::vector<std::string> identifier_tokens(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$') {
      cur += c;
    } else if (!cur.empty()) {
      out.push_back(cur);
      cur.clear();
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

bool any_token_in(const std::string& text, const std::unordered_set<std::string>& names) {
  for (const auto& tok : identifier_tokens(text)) {
    if (names.count(tok)) return true;
  }
  return false;
}

}  // namespace

IncrementalUpdate::IncrementalUpdate(db::Database& db) : db_(db) {
  db_.exec(kTempSchema);
  lookup_ = db_.prepare(
      "SELECT id, status, COALESCE(language,''), COALESCE(fingerprint,''), size_bytes, COALESCE(mtime_s,0), "
      "COALESCE(extractor_version,0) FROM files WHERE path = ?");
  seen_ = db_.prepare("INSERT OR IGNORE INTO refresh_seen(id) VALUES (?)");
}

std::optional<IncrementalUpdate::Existing> IncrementalUpdate::lookup(const std::string& path) {
  lookup_.bind(1, path);
  std::optional<Existing> out;
  if (lookup_.step()) {
    out = Existing{lookup_.int64(0), lookup_.text(1), lookup_.text(2), lookup_.text(3),
                   lookup_.int64(4), lookup_.int64(5), static_cast<int>(lookup_.int64(6))};
  }
  lookup_.reset();
  return out;
}

void IncrementalUpdate::mark_seen(int64_t file_id) { seen_.bind(1, file_id).run(); }

void IncrementalUpdate::snapshot_interface(int64_t file_id, const char* which) {
  std::string t(which);
  auto run = [&](const std::string& sql) {
    auto st = db_.prepare(sql);
    st.bind(1, file_id).run();
  };
  // Symbol signatures (including declared return types): a difference means
  // references to that name may now resolve differently anywhere.
  run("INSERT INTO refresh_" + t + "_sig(file_id, name, sig) SELECT s.file_id, s.name, s.kind || '|' || "
      "s.qualified_name || '|' || IFNULL(s.param_count, -1) || '|' || IFNULL(s.param_types, '') || '|' || "
      "s.is_definition || '|' || s.exported || '|' || "
      "s.is_static || '|' || IFNULL(s.container, '') || '|' || IFNULL(p.qualified_name, '') || '|' || "
      "IFNULL(s.visibility, '') || '|' || IFNULL((SELECT h.type_name FROM type_hints h WHERE h.scope_symbol_id = s.id "
      "AND h.name = '#return'), '') FROM symbols s LEFT JOIN symbols p ON p.id = s.parent_id "
      "WHERE s.file_id = ?1 AND s.kind != 'module'");
  // Base classes: a class whose bases changed changes method lookups through it.
  run("INSERT INTO refresh_" + t + "_sig(file_id, name, sig) SELECT e.file_id, s.name, 'inherit|' || "
      "s.qualified_name || '|' || e.raw_kind || '|' || IFNULL(e.target_text, '') FROM edges e "
      "JOIN symbols s ON s.id = e.src_symbol_id WHERE e.file_id = ?1 AND e.raw_kind IN ('extends','implements')");
  // Exported interface: exports, module-level value types, re-exportable imports, includes.
  run("INSERT INTO refresh_" + t + "_iface(file_id, item) SELECT x.file_id, 'export|' || x.exported_name || '|' || "
      "IFNULL(x.local_name, '') || '|' || x.kind || '|' || IFNULL(d.specifier, '') FROM exports x "
      "LEFT JOIN file_deps d ON d.id = x.dep_id WHERE x.file_id = ?1");
  run("INSERT INTO refresh_" + t + "_iface(file_id, item) SELECT h.file_id, 'hint|' || h.name || '|' || h.type_name "
      "FROM type_hints h JOIN symbols s ON s.id = h.scope_symbol_id WHERE h.file_id = ?1 AND s.kind = 'module'");
  run("INSERT INTO refresh_" + t + "_iface(file_id, item) SELECT b.file_id, 'bind|' || b.local_name || '|' || "
      "b.raw_imported || '|' || b.raw_kind || '|' || d.specifier FROM bindings b JOIN file_deps d ON d.id = b.raw_dep_id "
      "JOIN files f ON f.id = b.file_id WHERE b.file_id = ?1 AND f.language IN ('javascript','typescript','tsx','python')");
  run("INSERT INTO refresh_" + t + "_iface(file_id, item) SELECT d.src_file_id, 'inc|' || d.specifier FROM file_deps d "
      "WHERE d.src_file_id = ?1 AND d.kind = 'include'");
}

void IncrementalUpdate::forget_facts(int64_t file_id, bool removed) {
  snapshot_interface(file_id, "old");
  auto run = [&](const char* sql) {
    auto st = db_.prepare(sql);
    st.bind(1, file_id).run();
  };
  run("INSERT OR IGNORE INTO refresh_touched(id) VALUES (?1)");
  if (removed) run("INSERT OR IGNORE INTO refresh_removed(id) VALUES (?1)");
  // All old symbols count as deleted until record_new_facts() reuses their ids.
  run("INSERT OR IGNORE INTO refresh_deleted_symbols(id) SELECT id FROM symbols WHERE file_id = ?1");
  reuse_ids_.clear();
  if (!removed) {
    auto st = db_.prepare(
        "SELECT id, kind, qualified_name, IFNULL(param_count, -1) FROM symbols WHERE file_id = ? "
        "ORDER BY start_line, start_col, id");
    st.bind(1, file_id);
    std::unordered_map<std::string, int> occurrences;
    while (st.step()) {
      std::string kind = st.text(1), qn = st.text(2);
      int pc = static_cast<int>(st.int64(3));
      int ordinal = occurrences[kind + "|" + qn + "|" + std::to_string(pc)]++;
      reuse_ids_[FactWriter::symbol_key(kind, qn, pc, ordinal)] = st.int64(0);
    }
  }
  run("DELETE FROM edge_candidates WHERE edge_id IN (SELECT id FROM edges WHERE file_id = ?1)");
  run("DELETE FROM edges WHERE file_id = ?1");
  run("DELETE FROM bindings WHERE file_id = ?1");
  run("DELETE FROM exports WHERE file_id = ?1");
  run("DELETE FROM file_deps WHERE src_file_id = ?1");
  run("DELETE FROM type_hints WHERE file_id = ?1");
  run("DELETE FROM route_middleware WHERE route_id IN (SELECT id FROM routes WHERE file_id = ?1)");
  run("DELETE FROM routes WHERE file_id = ?1");
  run("DELETE FROM mounts WHERE file_id = ?1");
  run("DELETE FROM routers WHERE file_id = ?1");
  run("DELETE FROM exceptions WHERE file_id = ?1");
  run("DELETE FROM entry_points WHERE file_id = ?1");
  run("DELETE FROM symbols WHERE file_id = ?1");
}

void IncrementalUpdate::record_new_facts(int64_t file_id, const std::vector<int64_t>& reused_ids) {
  auto touched = db_.prepare("INSERT OR IGNORE INTO refresh_touched(id) VALUES (?)");
  touched.bind(1, file_id).run();
  auto undelete = db_.prepare("DELETE FROM refresh_deleted_symbols WHERE id = ?");
  for (auto id : reused_ids) undelete.bind(1, id).run();
  reused_symbols_ += static_cast<int64_t>(reused_ids.size());
  reuse_ids_.clear();
  snapshot_interface(file_id, "new");
}

int64_t IncrementalUpdate::remove_unseen_files() {
  std::vector<int64_t> gone;
  auto st = db_.prepare("SELECT id FROM files WHERE id NOT IN (SELECT id FROM refresh_seen) ORDER BY id");
  while (st.step()) gone.push_back(st.int64(0));
  st.reset();
  auto del = db_.prepare("DELETE FROM files WHERE id = ?");
  for (auto id : gone) {
    forget_facts(id, true);
    del.bind(1, id).run();
  }
  removed_ = static_cast<int64_t>(gone.size());
  return removed_;
}

void IncrementalUpdate::prepare(Resolver& resolver, bool files_added_or_removed, int64_t source_files,
                                double max_affected_fraction) {
  Stopwatch step;
  auto lap = [&](const char* name) {
    timings_[name] = step.elapsed_ms();
    step = Stopwatch();
  };
  // 1. Derived rows are regenerated from scratch; snapshot dependency targets first.
  db_.exec("INSERT INTO refresh_old_deps(id, src, dst, status) SELECT id, src_file_id, dst_file_id, status "
           "FROM file_deps WHERE generated = 0 AND status != 'pending'");
  // Only unchanged files: changed and added files are re-resolved anyway, and
  // whether their importers are affected is decided by their interface diff.
  db_.exec("INSERT INTO refresh_old_bindings(id, file, kind, dst) SELECT b.id, b.file_id, b.kind, d.dst_file_id "
           "FROM bindings b JOIN file_deps d ON d.id = b.dep_id WHERE b.file_id NOT IN (SELECT id FROM refresh_touched)");
  db_.exec("DELETE FROM edge_candidates WHERE edge_id IN (SELECT id FROM edges WHERE generated = 1)");
  db_.exec("DELETE FROM edges WHERE generated = 1");
  db_.exec("UPDATE bindings SET dep_id = raw_dep_id, kind = raw_kind, imported_name = raw_imported "
           "WHERE dep_id != raw_dep_id OR kind != raw_kind OR imported_name != raw_imported");
  db_.exec("DELETE FROM file_deps WHERE generated = 1");
  db_.exec("DELETE FROM route_middleware WHERE scope != 'route' OR route_id IN "
           "(SELECT id FROM routes WHERE origin_route_id IS NOT NULL)");
  db_.exec("DELETE FROM routes WHERE origin_route_id IS NOT NULL");
  db_.exec("UPDATE route_middleware SET status = 'pending', symbol_id = NULL WHERE ref_kind != 'inline'");
  db_.exec("UPDATE routes SET method = raw_method WHERE method != raw_method");
  db_.exec("UPDATE routes SET full_path = path, mount_prefix = NULL, mount_status = 'pending', mount_id = NULL, "
           "router_id = NULL");
  db_.exec("UPDATE routes SET handler_symbol_id = NULL, handler_status = 'pending', handler_note = raw_handler_note "
           "WHERE handler_kind IS NOT NULL AND handler_kind != 'inline'");
  db_.exec("UPDATE mounts SET status = 'pending', target_router_id = NULL, note = raw_note, "
           "target_symbol_id = CASE WHEN target_kind = 'inline' THEN target_symbol_id ELSE NULL END");
  db_.exec("DELETE FROM entry_points WHERE kind IN ('package_main','package_bin','package_script')");
  lap("derived_reset");

  // 2. File dependencies. Added or removed files can change where any import
  //    resolves, so all are re-resolved and compared with the snapshot.
  if (files_added_or_removed) {
    db_.exec("UPDATE file_deps SET status = 'pending', dst_file_id = NULL, note = raw_note WHERE status != 'pending'");
  }
  resolver.run_dependencies();
  db_.exec("INSERT OR IGNORE INTO refresh_seeds(id) SELECT o.src FROM refresh_old_deps o JOIN file_deps d ON d.id = o.id "
           "WHERE (IFNULL(d.dst_file_id, -1) != IFNULL(o.dst, -1) OR d.status != o.status) "
           "AND o.src NOT IN (SELECT id FROM refresh_touched)");
  db_.exec("INSERT OR IGNORE INTO refresh_seeds(id) SELECT o.file FROM refresh_old_bindings o "
           "JOIN bindings b ON b.id = o.id JOIN file_deps d ON d.id = b.dep_id "
           "WHERE b.kind != o.kind OR IFNULL(d.dst_file_id, -1) != IFNULL(o.dst, -1)");
  lap("dependencies");

  // 3. Which files and relationships must be re-resolved.
  compute_affected();
  lap("affected");
  int64_t affected = db_.query_int("SELECT COUNT(*) FROM refresh_affected").value_or(0);
  int64_t edge_level = db_.query_int("SELECT COUNT(*) FROM refresh_reset_edges").value_or(0);
  int64_t total_edges = db_.query_int("SELECT COUNT(*) FROM edges WHERE generated = 0").value_or(0);
  if ((source_files > 0 && affected > static_cast<int64_t>(max_affected_fraction * static_cast<double>(source_files))) ||
      (total_edges > 0 && edge_level > static_cast<int64_t>(max_affected_fraction * static_cast<double>(total_edges)))) {
    full_reresolve_ = true;
    db_.exec("INSERT OR IGNORE INTO refresh_affected(id) SELECT id FROM files");
    affected = db_.query_int("SELECT COUNT(*) FROM refresh_affected").value_or(0);
  }
  affected_files_ = affected;

  // 4. Reset those relationships to their extracted state.
  db_.exec("UPDATE edges SET status = 'pending', dst_symbol_id = NULL, note = NULL, kind = raw_kind "
           "WHERE generated = 0 AND status != 'pending' AND file_id IN (SELECT id FROM refresh_affected)");
  reset_edges_ = db_.changes();
  db_.exec("UPDATE edges SET status = 'pending', dst_symbol_id = NULL, note = NULL, kind = raw_kind "
           "WHERE generated = 0 AND status != 'pending' AND id IN (SELECT id FROM refresh_reset_edges)");
  reset_edges_ += db_.changes();
  db_.exec("DELETE FROM edge_candidates WHERE edge_id IN (SELECT id FROM edges WHERE status = 'pending')");
  lap("edge_reset");
  log::info("refresh", std::to_string(affected) + " files and " + std::to_string(edge_level) +
                           " individual relationships affected; " + std::to_string(reset_edges_) + " reset" +
                           (full_reresolve_ ? " (full re-resolution)" : ""));
}

void IncrementalUpdate::compute_affected() {
  auto affected_count = [&] { return db_.query_int("SELECT COUNT(*) FROM refresh_affected").value_or(0); };
  Stopwatch rule_watch;
  auto note_rule = [&](const char* rule) {
    breakdown_[rule] = {{"affected_files", affected_count()},
                        {"edge_level", db_.query_int("SELECT COUNT(*) FROM refresh_reset_edges").value_or(0)},
                        {"ms", rule_watch.elapsed_ms()}};
    rule_watch = Stopwatch();
  };
  db_.exec("INSERT OR IGNORE INTO refresh_affected(id) SELECT id FROM refresh_touched "
           "WHERE id NOT IN (SELECT id FROM refresh_removed)");
  // Every relationship in a file that referenced a symbol which no longer exists.
  db_.exec("INSERT OR IGNORE INTO refresh_affected(id) SELECT DISTINCT e.file_id FROM edges e "
           "JOIN refresh_deleted_symbols d ON d.id = e.dst_symbol_id WHERE e.generated = 0");
  db_.exec("INSERT OR IGNORE INTO refresh_affected(id) SELECT DISTINCT e.file_id FROM edge_candidates c "
           "JOIN refresh_deleted_symbols d ON d.id = c.symbol_id JOIN edges e ON e.id = c.edge_id");
  note_rule("1_touched_and_deleted_targets");

  // Names whose definitions were added, removed or changed signature.
  db_.exec("INSERT OR IGNORE INTO refresh_names(name) SELECT name FROM "
           "(SELECT file_id, name, sig FROM refresh_old_sig EXCEPT SELECT file_id, name, sig FROM refresh_new_sig)");
  db_.exec("INSERT OR IGNORE INTO refresh_names(name) SELECT name FROM "
           "(SELECT file_id, name, sig FROM refresh_new_sig EXCEPT SELECT file_id, name, sig FROM refresh_old_sig)");
  std::unordered_set<std::string> names;
  {
    auto st = db_.prepare("SELECT name FROM refresh_names");
    while (st.step()) names.insert(st.text(0));
  }
  // Closure: a changed class changes lookups through its subclasses, and a
  // changed type changes what functions declared to return it yield.
  {
    auto add = db_.prepare("INSERT OR IGNORE INTO refresh_names(name) VALUES (?)");
    auto subclasses = db_.prepare(
        "SELECT DISTINCT s.name FROM edges e JOIN symbols s ON s.id = e.src_symbol_id JOIN symbols b ON b.id = e.dst_symbol_id "
        "WHERE e.kind IN ('extends','implements') AND e.dst_symbol_id IS NOT NULL AND b.name = ?");
    auto deleted_bases = db_.prepare(
        "SELECT DISTINCT s.name FROM edges e JOIN symbols s ON s.id = e.src_symbol_id "
        "JOIN refresh_deleted_symbols d ON d.id = e.dst_symbol_id WHERE e.kind IN ('extends','implements')");
    std::deque<std::string> queue(names.begin(), names.end());
    while (deleted_bases.step()) {
      std::string n = deleted_bases.text(0);
      if (names.insert(n).second) queue.push_back(n);
    }
    deleted_bases.reset();
    auto returns_mentioning = [&](std::vector<std::string>& found) {
      auto st = db_.prepare(
          "SELECT s.name, h.type_name FROM type_hints h JOIN symbols s ON s.id = h.scope_symbol_id WHERE h.name = '#return'");
      while (st.step()) {
        std::string fn = st.text(0);
        if (!names.count(fn) && any_token_in(st.text(1), names)) found.push_back(fn);
      }
    };
    for (int round = 0; round < 10; ++round) {
      while (!queue.empty() && names.size() < 100000) {
        std::string n = queue.front();
        queue.pop_front();
        subclasses.bind(1, n);
        std::vector<std::string> subs;
        while (subclasses.step()) subs.push_back(subclasses.text(0));
        subclasses.reset();
        for (auto& s : subs) {
          if (names.insert(s).second) queue.push_back(s);
        }
      }
      std::vector<std::string> found;
      returns_mentioning(found);
      if (found.empty()) break;
      for (auto& f : found) {
        if (names.insert(f).second) queue.push_back(f);
      }
    }
    for (const auto& n : names) add.bind(1, n).run();
  }
  changed_names_ = static_cast<int64_t>(names.size());
  note_rule("2_name_closure");

  if (!names.empty()) {
    // Individual relationships naming a changed definition (callee or receiver).
    db_.exec("INSERT OR IGNORE INTO refresh_reset_edges(id) SELECT e.id FROM edges e "
             "JOIN refresh_names n ON n.name = e.target_name WHERE e.generated = 0");
    auto mark_edge = db_.prepare("INSERT OR IGNORE INTO refresh_reset_edges(id) VALUES (?)");
    {
      auto st = db_.prepare("SELECT id, receiver FROM edges WHERE generated = 0 AND receiver IS NOT NULL AND receiver != ''");
      std::vector<int64_t> hits;
      while (st.step()) {
        if (any_token_in(st.text(1), names)) hits.push_back(st.int64(0));
      }
      st.reset();
      for (auto id : hits) mark_edge.bind(1, id).run();
    }
    note_rule("3_edges_by_name");
    // Declared or inferred types naming a changed definition affect every use
    // of the variable, so the whole file is re-resolved.
    auto mark_file = db_.prepare("INSERT OR IGNORE INTO refresh_affected(id) VALUES (?)");
    auto st = db_.prepare("SELECT DISTINCT file_id, type_name FROM type_hints WHERE type_name != '?'");
    std::vector<int64_t> hits;
    while (st.step()) {
      if (any_token_in(st.text(1), names)) hits.push_back(st.int64(0));
    }
    st.reset();
    for (auto id : hits) mark_file.bind(1, id).run();
    note_rule("4_files_by_type_reference");
  }

  // Files whose exported interface changed, removed files and files whose
  // import resolution changed propagate to everything that imports them.
  db_.exec("INSERT OR IGNORE INTO refresh_seeds(id) SELECT file_id FROM "
           "(SELECT file_id, item FROM refresh_old_iface EXCEPT SELECT file_id, item FROM refresh_new_iface)");
  db_.exec("INSERT OR IGNORE INTO refresh_seeds(id) SELECT file_id FROM "
           "(SELECT file_id, item FROM refresh_new_iface EXCEPT SELECT file_id, item FROM refresh_old_iface)");
  db_.exec("INSERT OR IGNORE INTO refresh_seeds(id) SELECT id FROM refresh_removed");
  db_.exec("INSERT OR IGNORE INTO refresh_affected(id) SELECT id FROM refresh_seeds "
           "WHERE id NOT IN (SELECT id FROM refresh_removed)");
  std::deque<int64_t> queue;
  std::unordered_set<int64_t> visited;
  {
    auto st = db_.prepare("SELECT id FROM refresh_seeds");
    while (st.step()) {
      queue.push_back(st.int64(0));
      visited.insert(st.int64(0));
    }
  }
  auto importers = db_.prepare("SELECT DISTINCT src_file_id FROM file_deps WHERE dst_file_id = ?");
  auto old_importers = db_.prepare("SELECT DISTINCT src FROM refresh_old_deps WHERE dst = ?");
  auto mark = db_.prepare("INSERT OR IGNORE INTO refresh_affected(id) VALUES (?)");
  while (!queue.empty()) {
    int64_t f = queue.front();
    queue.pop_front();
    std::vector<int64_t> next;
    importers.bind(1, f);
    while (importers.step()) next.push_back(importers.int64(0));
    importers.reset();
    old_importers.bind(1, f);
    while (old_importers.step()) next.push_back(old_importers.int64(0));
    old_importers.reset();
    for (auto n : next) {
      if (!visited.insert(n).second) continue;
      mark.bind(1, n).run();
      queue.push_back(n);
    }
  }
  db_.exec("DELETE FROM refresh_affected WHERE id NOT IN (SELECT id FROM files)");
  note_rule("5_import_closure");

  // Chained calls depend on the call that produces their receiver.
  db_.exec("INSERT OR IGNORE INTO refresh_reset_edges(id) SELECT id FROM edges WHERE generated = 0 AND file_id IN "
           "(SELECT id FROM refresh_affected)");
  for (int i = 0; i < 64; ++i) {
    db_.exec("INSERT OR IGNORE INTO refresh_reset_edges(id) SELECT e.id FROM edges e JOIN refresh_reset_edges r "
             "ON r.id = e.receiver_edge_id WHERE e.id NOT IN (SELECT id FROM refresh_reset_edges)");
    if (db_.changes() == 0) break;
  }
  db_.exec("DELETE FROM refresh_reset_edges WHERE id IN (SELECT e.id FROM edges e WHERE e.file_id IN "
           "(SELECT id FROM refresh_affected))");
}

json IncrementalUpdate::stats() const {
  json timing = json::object();
  for (const auto& [k, v] : timings_) timing[k] = v;
  return {{"unchanged_files", unchanged_},
          {"changed_files", changed_},
          {"added_files", added_},
          {"removed_files", removed_},
          {"reused_symbol_ids", reused_symbols_},
          {"changed_names", changed_names_},
          {"affected_files", affected_files_},
          {"relationships_reset", reset_edges_},
          {"full_reresolve", full_reresolve_},
          {"timing_ms", timing},
          {"affected_breakdown", breakdown_}};
}

}  // namespace proj
