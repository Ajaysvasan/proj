#include "index/indexer.hpp"

#include <algorithm>
#include <cstdio>
#include <optional>
#include <fstream>
#include <stdexcept>

#include "core/hash.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "core/sysinfo.hpp"
#include "core/time.hpp"
#include "core/version.hpp"
#include "db/schema.hpp"
#include "index/refresh.hpp"
#include "index/resolver.hpp"
#include "index/store.hpp"
#include "parse/extractors.hpp"
#include "parse/parser.hpp"

namespace proj {
namespace fs = std::filesystem;
namespace {

constexpr int kFilesPerTransaction = 256;
constexpr size_t kMaxManifests = 500;
constexpr size_t kBinaryProbeBytes = 8192;

// Reads a whole (size-limited) file. Returns false on I/O errors.
bool read_file(const fs::path& path, uint64_t expected, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  out.clear();
  out.reserve(static_cast<size_t>(expected));
  char buf[65536];
  while (in.read(buf, sizeof(buf)) || in.gcount() > 0) out.append(buf, static_cast<size_t>(in.gcount()));
  return !in.bad();
}

bool looks_binary(const std::string& content) {
  size_t n = std::min(content.size(), kBinaryProbeBytes);
  for (size_t i = 0; i < n; ++i) {
    if (content[i] == '\0') return true;
  }
  return false;
}

std::vector<std::string> string_list(const json& j, const char* key) {
  std::vector<std::string> out;
  if (!j.contains(key)) return out;
  if (!j[key].is_array()) throw std::invalid_argument(std::string(key) + " must be an array of strings");
  for (const auto& v : j[key]) {
    if (!v.is_string()) throw std::invalid_argument(std::string(key) + " must be an array of strings");
    out.push_back(v.get<std::string>());
  }
  return out;
}

}  // namespace

AnalysisOptions AnalysisOptions::from_json(const json& j) {
  AnalysisOptions o;
  if (j.is_null()) return o;
  if (!j.is_object()) throw std::invalid_argument("options must be an object");
  o.scan.extra_excludes = string_list(j, "exclude");
  o.scan.include_defaults = string_list(j, "include_default_excluded");
  if (j.contains("default_excludes")) o.scan.default_excludes = j.at("default_excludes").get<bool>();
  if (j.contains("use_gitignore")) o.scan.use_gitignore = j.at("use_gitignore").get<bool>();
  if (j.contains("max_file_size")) {
    auto v = j.at("max_file_size").get<int64_t>();
    if (v < 1024 || v > 64LL * 1024 * 1024) throw std::invalid_argument("max_file_size must be between 1 KiB and 64 MiB");
    o.scan.max_file_size = static_cast<uint64_t>(v);
  }
  if (j.contains("max_files")) {
    auto v = j.at("max_files").get<int64_t>();
    if (v < 1 || v > 5'000'000) throw std::invalid_argument("max_files must be between 1 and 5000000");
    o.scan.max_files = v;
  }
  if (j.contains("parse_timeout_ms")) {
    auto v = j.at("parse_timeout_ms").get<double>();
    if (v < 10 || v > 600000) throw std::invalid_argument("parse_timeout_ms must be between 10 and 600000");
    o.parse_timeout_ms = v;
  }
  return o;
}

json AnalysisOptions::to_json() const {
  return {{"exclude", scan.extra_excludes},
          {"include_default_excluded", scan.include_defaults},
          {"default_excludes", scan.default_excludes},
          {"use_gitignore", scan.use_gitignore},
          {"max_file_size", scan.max_file_size},
          {"max_files", scan.max_files},
          {"parse_timeout_ms", parse_timeout_ms}};
}

void remove_stale_build_files(const fs::path& index_path) {
  std::error_code ec;
  std::string prefix = index_path.filename().string() + ".building-";
  for (const auto& entry : fs::directory_iterator(index_path.parent_path(), ec)) {
    std::string name = entry.path().filename().string();
    if (name.rfind(prefix, 0) != 0) continue;
    int64_t pid = 0;
    try {
      pid = std::stoll(name.substr(prefix.size()));
    } catch (...) {
      continue;
    }
    if (!process_alive(pid)) fs::remove(entry.path(), ec);
  }
}

Indexer::Indexer(Registry& registry, ProjectRecord project, AnalysisOptions options)
    : registry_(registry), project_(std::move(project)), options_(std::move(options)) {}

AnalysisOutcome Indexer::run() { return execute(false); }

AnalysisOutcome Indexer::refresh(double max_affected_fraction) {
  max_affected_fraction_ = max_affected_fraction;
  return execute(true);
}

namespace {

// Resolution statistics computed from the finished index, so full analyses
// and refreshes report the same totals for the whole project.
json resolution_stats(db::Database& db) {
  // Counts rows per status in a single pass. GROUP BY would sort every row
  // first, and SQLite's sorter buffers up to the page-cache size in memory,
  // which made peak memory grow with the number of edges.
  auto by_status = [&](const std::string& from, std::initializer_list<const char*> statuses) {
    std::string sql = "SELECT COUNT(*)";
    for (const char* s : statuses) sql += std::string(", COALESCE(SUM(status = '") + s + "'), 0)";
    auto st = db.prepare(sql + " FROM " + from);
    json out = json::object();
    st.step();
    out["total"] = st.int64(0);
    int col = 1;
    for (const char* s : statuses) out[s] = st.int64(col++);
    return out;
  };
  json calls = by_status("edges WHERE raw_kind IN ('calls','instantiates')",
                         {"resolved", "ambiguous", "heuristic", "unresolved", "external"});
  json deps_raw = by_status("file_deps", {"resolved", "heuristic", "external", "unresolved"});
  int64_t deps_resolved = deps_raw.value("resolved", 0) + deps_raw.value("heuristic", 0);
  int64_t deps_external = deps_raw.value("external", 0);
  int64_t deps_unresolved = deps_raw.value("unresolved", 0);
  json inherit = by_status("edges WHERE raw_kind IN ('extends','implements')", {"resolved"});
  auto count = [&](const char* sql) { return db.query_int(sql).value_or(0); };
  json mounts = by_status("mounts", {"router", "middleware", "unresolved"});
  return {{"dependencies",
           {{"total", deps_raw["total"]},
            {"resolved", deps_resolved},
            {"external", deps_external},
            {"unresolved", deps_unresolved},
            {"other", deps_raw["total"].get<int64_t>() - deps_resolved - deps_external - deps_unresolved}}},
          {"calls", calls},
          {"inheritance", {{"resolved", inherit.value("resolved", 0)}, {"unresolved", inherit["total"].get<int64_t>() - inherit.value("resolved", 0)}}},
          {"override_edges", count("SELECT COUNT(*) FROM edges WHERE kind = 'overridden_by'")},
          {"module_load_edges", count("SELECT COUNT(*) FROM edges WHERE kind = 'loads_module'")},
          {"anonymous_class_edges", count("SELECT COUNT(*) FROM edges WHERE raw_kind = 'anonymous_class'")},
          {"thread_start_edges", count("SELECT COUNT(*) FROM edges WHERE raw_kind = 'thread_start'")},
          {"routes",
           {{"total", count("SELECT COUNT(*) FROM routes")},
            {"handlers_resolved", count("SELECT COUNT(*) FROM routes WHERE handler_symbol_id IS NOT NULL")},
            {"handlers_unresolved", count("SELECT COUNT(*) FROM routes WHERE handler_symbol_id IS NULL")}}},
          {"mounts",
           {{"router", mounts.value("router", 0)},
            {"middleware", mounts.value("middleware", 0)},
            {"unresolved", mounts.value("unresolved", 0)}}},
          {"manifest_entry_points",
           count("SELECT COUNT(*) FROM entry_points WHERE kind IN ('package_main','package_bin','package_script')")}};
}

constexpr const char* kNestingNote = "syntax tree nesting limit reached; deeper code was not analyzed";

}  // namespace

AnalysisOutcome Indexer::execute(bool incremental) {
  Stopwatch total;
  DiagnosticSink diags(options_.diagnostics_capacity);
  AnalysisOutcome outcome;
  outcome.run_id = registry_.start_run(project_);
  fs::path index_path = project_.index_path;
  fs::path build_path = index_path.string() + ".building-" + std::to_string(current_pid());
  const char* what = incremental ? "refresh" : "analysis";
  log::info("indexer", std::string(what) + " started for project '" + project_.name + "' (run " +
                           std::to_string(outcome.run_id) + ")");

  RunCounts counts;
  json stats = json::object();
  double scan_parse_ms = 0, resolve_ms = 0, finalize_ms = 0;
  ScanStats sstats;
  bool replaced = false;
  json refresh_stats = nullptr;

  try {
    remove_stale_build_files(index_path);
    std::error_code ec;
    fs::remove(build_path, ec);
    if (incremental) {
      // The refresh is applied to a private copy; the live index stays untouched
      // until the copy is complete and renamed over it.
      fs::copy_file(index_path, build_path, fs::copy_options::overwrite_existing);
    }
    {
      db::Database db(build_path, db::OpenMode::ReadWriteCreate);
      db::configure_index_build(db);
      if (incremental) db::check_index_schema(db);
      else db::create_index_schema(db);
      FactWriter writer(db);
      ParserManager parser(options_.parse_timeout_ms);
      Scanner scanner(project_.root_path, options_.scan, diags);
      std::optional<IncrementalUpdate> inc;
      if (incremental) inc.emplace(db);
      auto touch_mtime = db.prepare("UPDATE files SET mtime_s = ? WHERE id = ?");
      std::vector<std::string> manifests;
      std::string content;
      int in_tx = 0;
      bool rows_added = false;
      Stopwatch scan_watch;
      db.exec("BEGIN");
      scanner.scan([&](const ScanEntry& e) {
        FileRecord rec;
        rec.rel_path = e.rel_path;
        rec.size = e.size;
        rec.mtime_s = e.mtime_s;
        rec.status = e.status;
        rec.note = e.note;
        if (e.language != Language::None) rec.language = language_name(e.language);
        if (paths::base_name(e.rel_path) == "package.json" && e.status != file_status::kExcluded &&
            e.size < 1024 * 1024 && manifests.size() < kMaxManifests) {
          manifests.push_back(e.rel_path);
        }
        std::optional<IncrementalUpdate::Existing> existing;
        if (inc) existing = inc->lookup(e.rel_path);
        bool had_facts = existing && (existing->status == file_status::kParsed || existing->status == file_status::kSyntaxErrors);
        auto next = [&] {
          if (++in_tx >= kFilesPerTransaction) {
            db.exec("COMMIT");
            db.exec("BEGIN");
            in_tx = 0;
          }
        };
        ParseOutcome parsed;
        bool have_facts = false;
        if (e.status == file_status::kPending) {
          if (!read_file(e.abs_path, e.size, content)) {
            rec.status = file_status::kUnreadable;
            diags.add({"warn", "source", "file_unreadable", e.rel_path, 0, "file could not be read"});
          } else if (looks_binary(content)) {
            rec.status = file_status::kBinary;
            rec.note = "contains NUL bytes; not treated as source";
            diags.add({"info", "source", "binary_file", e.rel_path, 0, "file has a source extension but binary content"});
          } else {
            Fnv1a64 h;
            h.update(content);
            rec.fingerprint = h.hex();
            rec.size = content.size();
            rec.extractor_version = extractor_version(e.language);
            // Unchanged: same content fingerprint and size, language and extractor.
            if (had_facts && existing->fingerprint == rec.fingerprint &&
                existing->size == static_cast<int64_t>(rec.size) && existing->language == rec.language &&
                existing->extractor_version == rec.extractor_version) {
              inc->mark_seen(existing->id);
              inc->count_unchanged();
              if (existing->mtime_s != rec.mtime_s) touch_mtime.bind(1, rec.mtime_s).bind(2, existing->id).run();
              content.clear();
              next();
              return;
            }
            parsed = parser.parse(e.language, e.rel_path, content);
            rec.status = parsed.status;
            rec.error_count = parsed.error_count;
            rec.first_error_line = parsed.first_error_line;
            rec.line_count = parsed.line_count;
            rec.module_name = parsed.facts.module_name;
            // Configuration files (often templated) never make an analysis partial.
            bool config = is_config_language(e.language);
            if (parsed.status == "parse_failed") {
              rec.note = parsed.message;
              diags.add({config ? "info" : "error", "source", "parse_failed", e.rel_path, 0, parsed.message});
            } else {
              have_facts = true;
              if (parsed.status == "syntax_errors") {
                diags.add({config ? "info" : "warn", "source", "syntax_error", e.rel_path, parsed.first_error_line,
                           std::to_string(parsed.error_count) + " syntax error(s); facts were extracted best-effort"});
              }
              if (parsed.facts.depth_limited) {
                rec.note = kNestingNote;
                diags.add({"warn", "resource", "nesting_limit", e.rel_path, 0,
                           "syntax tree nesting exceeds the traversal limit; deeper code was not analyzed"});
              }
              for (const auto& note : parsed.facts.notes) {
                diags.add({"info", "source", "unsupported_construct", e.rel_path, 0, note});
              }
            }
          }
          if (content.capacity() > 4 * 1024 * 1024) content.shrink_to_fit();
          content.clear();
        }
        int64_t file_id;
        if (existing) {
          if (had_facts) inc->forget_facts(existing->id, false);
          writer.update_file(existing->id, rec);
          file_id = existing->id;
          inc->mark_seen(file_id);
        } else {
          file_id = writer.insert_file(rec);
          rows_added = true;
          if (inc) inc->mark_seen(file_id);
        }
        if (have_facts) {
          if (inc) {
            std::vector<int64_t> reused;
            writer.write_facts(file_id, rec.language, parsed.facts, &inc->reuse_ids(), &reused);
            inc->record_new_facts(file_id, reused);
          } else {
            writer.write_facts(file_id, rec.language, parsed.facts);
          }
        }
        if (inc && (had_facts || have_facts)) inc->count_changed(!existing);
        next();
      });
      db.exec("COMMIT");
      sstats = scanner.stats();
      bool rows_removed = false;
      if (inc) {
        db.exec("BEGIN");
        rows_removed = inc->remove_unseen_files() > 0;
        db.exec("COMMIT");
      }
      scan_parse_ms = scan_watch.elapsed_ms();
      log::info("indexer", "scan finished in " + std::to_string(static_cast<int64_t>(scan_parse_ms)) + " ms");

      Stopwatch resolve_watch;
      Resolver resolver(db, project_.root_path, diags, manifests);
      if (inc) {
        int64_t source_files =
            db.query_int("SELECT COUNT(*) FROM files WHERE status IN ('parsed','syntax_errors')").value_or(0);
        inc->prepare(resolver, rows_added || rows_removed, source_files, max_affected_fraction_);
        resolver.run_relationships();
        refresh_stats = inc->stats();
      } else {
        resolver.run();
      }
      resolve_ms = resolve_watch.elapsed_ms();

      Stopwatch finalize_watch;
      db.exec("INSERT INTO symbols_fts(symbols_fts) VALUES('rebuild')");
      log::debug("indexer", "finished search index");
      auto count = [&](const char* sql) { return db.query_int(sql).value_or(0); };
      json by_status = json::object();
      json by_language = json::object();
      {
        auto st = db.prepare("SELECT status, COUNT(*) FROM files GROUP BY status ORDER BY status");
        while (st.step()) by_status[st.text(0)] = st.int64(1);
        auto sl = db.prepare(
            "SELECT language, COUNT(*) FROM files WHERE language IS NOT NULL AND status != 'excluded' "
            "GROUP BY language ORDER BY language");
        while (sl.step()) by_language[sl.text(0)] = sl.int64(1);
      }
      int64_t files_total = count("SELECT COUNT(*) FROM files");
      int64_t source_attempted =
          count("SELECT COUNT(*) FROM files WHERE status IN ('parsed','syntax_errors','parse_failed','binary','unreadable')");
      int64_t partial_extractions = count("SELECT COUNT(*) FROM files WHERE note = 'syntax tree nesting limit reached; deeper code was not analyzed'");
      int64_t symbols = count("SELECT COUNT(*) FROM symbols WHERE kind != 'module'");  // excludes per-file module symbols
      int64_t call_edges = count("SELECT COUNT(*) FROM edges WHERE kind IN ('calls','instantiates')");
      int64_t all_edges = count("SELECT COUNT(*) FROM edges");
      int64_t deps = count("SELECT COUNT(*) FROM file_deps");
      int64_t routes = count("SELECT COUNT(*) FROM routes");
      int64_t throw_sites = count("SELECT COUNT(*) FROM exceptions WHERE kind IN ('throw','rethrow')");
      int64_t handler_sites = count("SELECT COUNT(*) FROM exceptions WHERE kind = 'catch'");
      int64_t entries = count("SELECT COUNT(*) FROM entry_points");
      int64_t unresolved_edges = count("SELECT COUNT(*) FROM edges WHERE status = 'unresolved'");
      int64_t ambiguous_edges = count("SELECT COUNT(*) FROM edges WHERE status = 'ambiguous'");
      int64_t unresolved_deps = count("SELECT COUNT(*) FROM file_deps WHERE status IN ('unresolved','ambiguous')");
      int64_t pending = count("SELECT (SELECT COUNT(*) FROM edges WHERE status='pending') + "
                              "(SELECT COUNT(*) FROM file_deps WHERE status='pending') + "
                              "(SELECT COUNT(*) FROM routes WHERE handler_status='pending' OR mount_status='pending') + "
                              "(SELECT COUNT(*) FROM mounts WHERE status='pending')");
      if (pending > 0) {
        diags.add({"error", "internal", "unresolved_pending_rows", "", 0,
                   std::to_string(pending) + " relationships were left in the pending state"});
      }

      // Configuration files (XML, YAML) are counted separately: their syntax
      // errors and skipped files are reported but do not make a run partial.
      json config_status = json::object();
      {
        auto st = db.prepare("SELECT status, COUNT(*) FROM files WHERE language IN ('xml','yaml') GROUP BY status");
        while (st.step()) config_status[st.text(0)] = st.int64(1);
      }
      auto code = [&](const char* status) { return by_status.value(status, 0) - config_status.value(status, 0); };
      int64_t parsed_ok = code("parsed");
      int64_t syntax = code("syntax_errors");
      int64_t failed = code("parse_failed");
      int64_t skipped = code("too_large") + code("binary") + code("unreadable") + code("symlink");
      int64_t config_parsed = config_status.value("parsed", 0) + config_status.value("syntax_errors", 0);
      source_attempted -= config_status.value("parsed", 0) + config_status.value("syntax_errors", 0) +
                          config_status.value("parse_failed", 0) + config_status.value("binary", 0) +
                          config_status.value("unreadable", 0);
      counts.files_discovered = files_total;
      counts.files_parsed = parsed_ok + syntax + config_parsed;
      counts.files_skipped = skipped;
      counts.symbols = symbols;
      counts.relationships = all_edges + deps;
      counts.unresolved_refs = unresolved_edges + ambiguous_edges + unresolved_deps;
      counts.parse_errors = syntax + failed;

      if (source_attempted == 0 || parsed_ok + syntax == 0) {
        outcome.status = "no_supported_files";
      } else if (syntax > 0 || failed > 0 || skipped > 0 || sstats.file_limit_reached || sstats.depth_limit_reached ||
                 sstats.dirs_unreadable > 0 || partial_extractions > 0 || pending > 0) {
        outcome.status = "partial";
      } else {
        outcome.status = "success";
      }

      stats = {{"mode", incremental ? "refresh" : "full"},
               {"files",
                {{"discovered", files_total},
                 {"source_candidates", source_attempted},
                 {"parsed", parsed_ok},
                 {"with_syntax_errors", syntax},
                 {"parse_failed", failed},
                 {"skipped", skipped},
                 {"unsupported", by_status.value("unsupported", 0)},
                 {"excluded", by_status.value("excluded", 0)},
                 {"config",
                  {{"parsed", config_status.value("parsed", 0)},
                   {"with_syntax_errors", config_status.value("syntax_errors", 0)},
                   {"skipped", config_status.value("too_large", 0) + config_status.value("binary", 0) +
                                   config_status.value("unreadable", 0) + config_status.value("parse_failed", 0)}}},
                 {"by_status", by_status},
                 {"by_language", by_language}}},
               {"directories",
                {{"excluded", sstats.dirs_excluded},
                 {"unreadable", sstats.dirs_unreadable},
                 {"symlinks_not_followed", sstats.dirs_symlink}}},
               {"limits",
                {{"file_limit_reached", sstats.file_limit_reached},
                 {"depth_limit_reached", sstats.depth_limit_reached},
                 {"files_with_nesting_limit", partial_extractions}}},
               {"symbols", symbols},
               {"call_edges", call_edges},
               {"graph_edges", all_edges},
               {"dependencies", deps},
               {"routes", routes},
               {"exception_sites", throw_sites},
               {"exception_handlers", handler_sites},
               {"entry_points", entries},
               {"unresolved_references", counts.unresolved_refs},
               {"resolution", resolution_stats(db)}};
      if (incremental) stats["refresh"] = refresh_stats;
      log::debug("indexer", "finished statistics");

      auto meta = db.prepare("INSERT OR REPLACE INTO index_meta(key, value) VALUES (?, ?)");
      meta.bind(1, "project_name").bind(2, project_.name).run();
      meta.bind(1, "root_path").bind(2, project_.root_path).run();
      meta.bind(1, "run_id").bind(2, std::to_string(outcome.run_id)).run();
      meta.bind(1, "created_at").bind(2, iso_now()).run();
      meta.bind(1, "analysis_status").bind(2, outcome.status).run();
      meta.bind(1, "stats").bind(2, stats.dump()).run();
      meta.bind(1, "options").bind(2, options_.to_json().dump()).run();
      meta.bind(1, "tool_version").bind(2, kToolVersion).run();
      if (!incremental) db.exec("ANALYZE");
      db.exec("PRAGMA optimize");
      log::debug("indexer", "finished query planner statistics");
      auto quick = db.prepare("PRAGMA quick_check");
      std::string check = quick.step() ? quick.text(0) : "no result";
      quick.reset();
      if (check != "ok") throw std::runtime_error("index integrity check failed: " + check);
      log::debug("indexer", "finished integrity check");
      if (incremental) {
        // Reclaim space only when deleted facts left many free pages; freed
        // pages are otherwise reused by later refreshes.
        int64_t free_pages = db.query_int("PRAGMA freelist_count").value_or(0);
        int64_t pages = db.query_int("PRAGMA page_count").value_or(1);
        if (free_pages * 4 > pages) db.exec("VACUUM");
      }
      finalize_ms = finalize_watch.elapsed_ms();
    }  // database closed here

    // Atomic replacement of the previous index (same directory, same file system).
    fs::rename(build_path, index_path);
    replaced = true;
    counts.warnings = diags.count_level("warn");
    counts.fatal_errors = 0;
  } catch (const std::exception& e) {
    outcome.status = "failed";
    outcome.error = e.what();
    counts.fatal_errors = 1;
    counts.warnings = diags.count_level("warn");
    diags.add({"error", "internal", incremental ? "refresh_failed" : "analysis_failed", "", 0, e.what()});
    log::error("indexer", std::string(what) + " failed for project '" + project_.name + "': " + e.what());
    std::error_code ec;
    fs::remove(build_path, ec);  // our private build file only; the previous index stays
  }

  double duration = total.elapsed_ms();
  std::error_code ec;
  int64_t index_size = replaced ? static_cast<int64_t>(fs::file_size(index_path, ec)) : 0;
  auto rss = peak_rss_kib();
  if (!stats.contains("mode")) stats["mode"] = incremental ? "refresh" : "full";
  stats["timing_ms"] = {{"scan_and_parse", scan_parse_ms}, {"resolve", resolve_ms}, {"finalize", finalize_ms}, {"total", duration}};
  stats["index_size_bytes"] = index_size;
  stats["peak_rss_kib"] = rss ? json(*rss) : json(nullptr);
  stats["diagnostics"] = {{"total", diags.total()},
                          {"warnings", diags.count_level("warn")},
                          {"errors", diags.count_level("error")},
                          {"stored", diags.kept().size()},
                          {"dropped", diags.dropped()}};
  registry_.finish_run(outcome.run_id, project_.id, outcome.status, duration, counts, stats, outcome.error, diags,
                       replaced);
  log::info("indexer", std::string(what) + " of '" + project_.name + "' finished with status " + outcome.status);

  outcome.summary = {{"project", project_.name},
                     {"root_path", project_.root_path},
                     {"status", outcome.status},
                     {"mode", incremental ? "refresh" : "full"},
                     {"run_id", outcome.run_id},
                     {"duration_ms", duration},
                     {"index_path", index_path.string()},
                     {"index_replaced", replaced},
                     {"previous_index_kept", !replaced && fs::exists(index_path, ec)},
                     {"counts",
                      {{"files_discovered", counts.files_discovered},
                       {"files_parsed", counts.files_parsed},
                       {"files_skipped", counts.files_skipped},
                       {"symbols", counts.symbols},
                       {"relationships", counts.relationships},
                       {"unresolved_refs", counts.unresolved_refs},
                       {"parse_errors", counts.parse_errors},
                       {"warnings", counts.warnings},
                       {"fatal_errors", counts.fatal_errors}}},
                     {"stats", stats},
                     {"error", outcome.error.empty() ? json(nullptr) : json(outcome.error)}};
  return outcome;
}

}  // namespace proj
