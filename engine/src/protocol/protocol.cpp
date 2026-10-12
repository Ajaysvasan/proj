#include "protocol/protocol.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <regex>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

#include "core/log.hpp"
#include "core/paths.hpp"
#include "core/version.hpp"
#include "db/schema.hpp"
#include "index/indexer.hpp"
#include "index/registry.hpp"
#include "query/query.hpp"
#include "scan/language.hpp"

namespace proj::protocol {
namespace fs = std::filesystem;
namespace {

std::string require_string(const json& args, const char* key) {
  if (!args.contains(key) || !args[key].is_string() || args[key].get<std::string>().empty()) {
    throw ProtocolError(status::kInvalidArguments, "missing_argument", std::string("argument '") + key + "' is required");
  }
  return args[key].get<std::string>();
}

int64_t get_int(const json& args, const char* key, int64_t def) {
  if (!args.contains(key) || args[key].is_null()) return def;
  if (!args[key].is_number_integer()) {
    throw ProtocolError(status::kInvalidArguments, "invalid_argument", std::string("argument '") + key + "' must be an integer");
  }
  return args[key].get<int64_t>();
}

bool get_bool(const json& args, const char* key, bool def) {
  if (!args.contains(key) || args[key].is_null()) return def;
  if (!args[key].is_boolean()) {
    throw ProtocolError(status::kInvalidArguments, "invalid_argument", std::string("argument '") + key + "' must be a boolean");
  }
  return args[key].get<bool>();
}

std::vector<std::string> get_strings(const json& args, const char* key) {
  std::vector<std::string> out;
  if (!args.contains(key) || args[key].is_null()) return out;
  if (!args[key].is_array()) {
    throw ProtocolError(status::kInvalidArguments, "invalid_argument", std::string("argument '") + key + "' must be an array");
  }
  for (const auto& v : args[key]) {
    if (!v.is_string()) {
      throw ProtocolError(status::kInvalidArguments, "invalid_argument", std::string("argument '") + key + "' must contain strings");
    }
    out.push_back(v.get<std::string>());
  }
  return out;
}

void validate_project_name(const std::string& name) {
  static const std::regex kName("^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$");
  if (!std::regex_match(name, kName)) {
    throw ProtocolError(status::kInvalidArguments, "invalid_project_name",
                        "project names must be 1-64 characters: letters, digits, '.', '_' or '-', starting with a "
                        "letter or digit");
  }
}

fs::path validate_project_dir(const std::string& raw) {
  std::error_code ec;
  fs::path p(raw);
  auto st = fs::status(p, ec);
  if (ec || !fs::exists(st)) {
    throw ProtocolError(status::kInvalidArguments, "path_not_found", "directory does not exist: " + raw);
  }
  if (!fs::is_directory(st)) {
    throw ProtocolError(status::kInvalidArguments, "not_a_directory", "not a directory: " + raw);
  }
  fs::path canon = fs::canonical(p, ec);
  if (ec) throw ProtocolError(status::kInvalidArguments, "path_unresolvable", "cannot resolve path " + raw + ": " + ec.message());
  DIR* d = opendir(canon.c_str());
  if (!d) throw ProtocolError(status::kInvalidArguments, "path_unreadable", "directory is not readable: " + canon.string());
  closedir(d);
  return canon;
}

// Hands memory freed after an analysis back to the operating system, so a
// long-lived --serve process (the interactive UI) idles at its query footprint
// rather than at the analysis peak. glibc otherwise keeps freed heap pages.
struct ReleaseFreedMemory {
  ReleaseFreedMemory() = default;
  ReleaseFreedMemory(const ReleaseFreedMemory&) = delete;
  ReleaseFreedMemory& operator=(const ReleaseFreedMemory&) = delete;
  ~ReleaseFreedMemory() {
#if defined(__GLIBC__)
    malloc_trim(0);
#endif
  }
};

}  // namespace

json make_response(const json& id, const std::string& op, const std::string& st, json result, json diagnostics,
                   json error) {
  return {{"protocol", kProtocolVersion},
          {"id", id},
          {"op", op},
          {"status", st},
          {"result", std::move(result)},
          {"diagnostics", diagnostics.is_null() ? json::array() : std::move(diagnostics)},
          {"error", std::move(error)}};
}

struct Engine::CachedIndex {
  std::unique_ptr<IndexReader> reader;
  fs::file_time_type mtime;
  uintmax_t size = 0;
};

Engine::Engine() = default;
Engine::~Engine() = default;

Registry& Engine::registry(const fs::path& data_dir) {
  if (!registry_ || registry_dir_ != data_dir) {
    registry_ = std::make_unique<Registry>(data_dir);
    registry_dir_ = data_dir;
    indexes_.clear();
  }
  return *registry_;
}

IndexReader& Engine::open_index(const fs::path& data_dir, const json& args) {
  std::string name = require_string(args, "project");
  auto project = registry(data_dir).find_by_name(name);
  if (!project) {
    throw ProtocolError(status::kNotFound, "unknown_project", "no project named '" + name + "' is registered",
                        {{"project", name}});
  }
  fs::path index = project->index_path;
  std::error_code ec;
  if (!fs::exists(index, ec)) {
    throw ProtocolError(status::kNotFound, "index_missing",
                        "project '" + name + "' has no index (last status: " + project->status +
                            "). Run `proj add " + name + " " + project->root_path + " --reanalyze`",
                        {{"project", name}, {"status", project->status}});
  }
  auto mtime = fs::last_write_time(index, ec);
  auto size = fs::file_size(index, ec);
  auto& slot = indexes_[name];
  if (!slot || slot->mtime != mtime || slot->size != size) {
    auto fresh = std::make_unique<CachedIndex>();
    try {
      fresh->reader = std::make_unique<IndexReader>(index);
    } catch (const db::SchemaError& e) {
      throw ProtocolError(status::kIndexError, "index_incompatible", e.what(), {{"project", name}});
    } catch (const db::Error& e) {
      throw ProtocolError(status::kIndexError, "index_corrupt",
                          std::string("index for '") + name + "' cannot be opened: " + e.what() +
                              ". Re-run the analysis with --reanalyze",
                          {{"project", name}});
    }
    fresh->mtime = mtime;
    fresh->size = size;
    slot = std::move(fresh);
  }
  return *slot->reader;
}

json Engine::handle_text(const std::string& line) {
  json request = json::parse(line, nullptr, false);
  if (request.is_discarded() || !request.is_object()) {
    return make_response(nullptr, "", status::kInvalidArguments, nullptr, nullptr,
                         {{"code", "malformed_request"}, {"message", "request is not a valid JSON object"}});
  }
  return handle(request);
}

json Engine::handle(const json& request) {
  json id = request.contains("id") ? request["id"] : json(nullptr);
  std::string op = request.contains("op") && request["op"].is_string() ? request["op"].get<std::string>() : "";
  json diagnostics = json::array();
  try {
    if (request.contains("protocol") && request["protocol"] != kProtocolVersion) {
      throw ProtocolError(status::kInvalidArguments, "unsupported_protocol_version",
                          "engine speaks protocol version " + std::to_string(kProtocolVersion));
    }
    if (op.empty()) throw ProtocolError(status::kInvalidArguments, "missing_op", "request has no 'op'");
    json args = request.value("args", json::object());
    json options = request.value("options", json::object());
    if (!args.is_object() || !options.is_object()) {
      throw ProtocolError(status::kInvalidArguments, "invalid_request", "'args' and 'options' must be objects");
    }
    fs::path data_dir = options.contains("data_dir") && options["data_dir"].is_string() &&
                                !options["data_dir"].get<std::string>().empty()
                            ? fs::path(options["data_dir"].get<std::string>())
                            : paths::default_data_dir();
    if (data_dir.is_relative()) data_dir = fs::absolute(data_dir);
    auto level = log::parse_level(options.value("log_level", std::string("warn"))).value_or(log::Level::Warn);
    log::configure(level, data_dir / "logs" / "proj.log");
    std::string st = status::kOk;
    json result = dispatch(op, args, data_dir, diagnostics, st);
    return make_response(id, op, st, std::move(result), std::move(diagnostics), nullptr);
  } catch (const ProtocolError& e) {
    json err = {{"code", e.code()}, {"message", e.what()}};
    if (!e.details().is_null()) err["details"] = e.details();
    return make_response(id, op, e.status(), nullptr, std::move(diagnostics), std::move(err));
  } catch (const db::SchemaError& e) {
    log::error("engine", std::string("schema error during ") + op + ": " + e.what());
    return make_response(id, op, status::kIndexError, nullptr, std::move(diagnostics),
                         {{"code", "schema_error"}, {"message", e.what()}});
  } catch (const db::Error& e) {
    log::error("engine", std::string("SQLite error during ") + op + ": " + e.what());
    return make_response(id, op, status::kInternalError, nullptr, std::move(diagnostics),
                         {{"code", "sqlite_error"}, {"message", e.what()}});
  } catch (const std::exception& e) {
    log::error("engine", std::string("internal error during ") + op + ": " + e.what());
    return make_response(id, op, status::kInternalError, nullptr, std::move(diagnostics),
                         {{"code", "internal_error"}, {"message", e.what()}});
  }
}

json Engine::dispatch(const std::string& op, const json& args, const fs::path& data_dir, json& diagnostics,
                      std::string& st) {
  if (op == "info") {
    json languages = json::array();
    for (auto l : {Language::JavaScript, Language::TypeScript, Language::Tsx, Language::Python, Language::Java,
                   Language::C, Language::Cpp}) {
      languages.push_back(language_name(l));
    }
    return {{"tool_version", kToolVersion},
            {"protocol_version", kProtocolVersion},
            {"index_schema_version", kIndexSchemaVersion},
            {"registry_schema_version", kRegistrySchemaVersion},
            {"data_dir", data_dir.string()},
            {"languages", languages}};
  }

  if (op == "add_project") {
    std::string name = require_string(args, "name");
    validate_project_name(name);
    fs::path root = validate_project_dir(require_string(args, "path"));
    bool reanalyze = get_bool(args, "reanalyze", false);
    Registry& reg = registry(data_dir);
    reg.mark_interrupted_runs();
    auto by_name = reg.find_by_name(name);
    auto by_root = reg.find_by_root(root.string());
    if (by_name && !reanalyze) {
      throw ProtocolError(status::kConflict, "project_exists",
                          "project '" + name + "' is already registered for " + by_name->root_path +
                              "; use --reanalyze to analyze it again",
                          {{"project", name}, {"root_path", by_name->root_path}});
    }
    if (by_name && by_name->root_path != root.string()) {
      throw ProtocolError(status::kConflict, "path_mismatch",
                          "project '" + name + "' is registered for " + by_name->root_path + ", not " + root.string(),
                          {{"project", name}, {"root_path", by_name->root_path}});
    }
    if (by_root && by_root->name != name) {
      throw ProtocolError(status::kConflict, "path_registered",
                          root.string() + " is already registered as project '" + by_root->name + "'",
                          {{"project", by_root->name}, {"root_path", root.string()}});
    }
    json requested = args.value("options", json(nullptr));
    AnalysisOptions options;
    try {
      if (by_name && requested.is_null()) options = AnalysisOptions::from_json(by_name->options);
      else options = AnalysisOptions::from_json(requested);
    } catch (const std::exception& e) {
      throw ProtocolError(status::kInvalidArguments, "invalid_options", e.what());
    }
    ProjectRecord project = by_name ? *by_name : reg.create_project(name, root.string(), options.to_json());
    if (by_name) reg.update_project_options(project.id, options.to_json());
    if (auto active = reg.active_run(project.id)) {
      throw ProtocolError(status::kConflict, "analysis_in_progress",
                          "another analysis of '" + name + "' is running (run " + std::to_string(*active) + ")");
    }
    indexes_.erase(name);
    ReleaseFreedMemory release;  // declared first: runs after the indexer is destroyed
    Indexer indexer(reg, project, options);
    AnalysisOutcome outcome = indexer.run();
    if (outcome.status == "failed") {
      throw ProtocolError(status::kAnalysisFailed, "analysis_failed", "analysis failed: " + outcome.error,
                          outcome.summary);
    }
    if (outcome.status != "success") st = status::kPartial;
    if (outcome.status == "no_supported_files") {
      diagnostics.push_back({{"level", "warn"},
                             {"code", "no_supported_files"},
                             {"message", "no files in a supported language were found; the index contains file "
                                         "metadata only"}});
    }
    return outcome.summary;
  }

  if (op == "refresh_project") {
    std::string name = require_string(args, "project");
    Registry& reg = registry(data_dir);
    reg.mark_interrupted_runs();
    auto project = reg.find_by_name(name);
    if (!project) {
      throw ProtocolError(status::kNotFound, "unknown_project", "no project named '" + name + "' is registered",
                          {{"project", name}});
    }
    std::error_code ec;
    if (!fs::is_directory(project->root_path, ec)) {
      throw ProtocolError(status::kInvalidArguments, "path_not_found",
                          "the project directory no longer exists: " + project->root_path,
                          {{"project", name}, {"root_path", project->root_path}});
    }
    if (!fs::exists(project->index_path, ec)) {
      throw ProtocolError(status::kNotFound, "index_missing",
                          "project '" + name + "' has no index to refresh (last status: " + project->status +
                              "). Run `proj add " + name + " " + project->root_path + " --reanalyze`",
                          {{"project", name}, {"status", project->status}});
    }
    {
      // Refuse indexes from other schema versions before starting a run.
      db::Database probe(project->index_path, db::OpenMode::ReadOnly);
      try {
        db::check_index_schema(probe);
      } catch (const db::SchemaError& e) {
        throw ProtocolError(status::kIndexError, "index_incompatible",
                            std::string(e.what()) + ". A refresh cannot upgrade it; run `proj add " + name + " " +
                                project->root_path + " --reanalyze`",
                            {{"project", name}});
      }
    }
    if (auto active = reg.active_run(project->id)) {
      throw ProtocolError(status::kConflict, "analysis_in_progress",
                          "another analysis of '" + name + "' is running (run " + std::to_string(*active) + ")");
    }
    AnalysisOptions options;
    try {
      options = AnalysisOptions::from_json(project->options);
    } catch (const std::exception& e) {
      throw ProtocolError(status::kInternalError, "invalid_stored_options", e.what());
    }
    double fraction = 0.4;
    if (args.contains("max_affected_fraction")) {
      if (!args["max_affected_fraction"].is_number() || args["max_affected_fraction"].get<double>() <= 0 ||
          args["max_affected_fraction"].get<double>() > 1) {
        throw ProtocolError(status::kInvalidArguments, "invalid_argument", "max_affected_fraction must be in (0, 1]");
      }
      fraction = args["max_affected_fraction"].get<double>();
    }
    indexes_.erase(name);
    ReleaseFreedMemory release;  // declared first: runs after the indexer is destroyed
    Indexer indexer(reg, *project, options);
    AnalysisOutcome outcome = indexer.refresh(fraction);
    if (outcome.status == "failed") {
      throw ProtocolError(status::kAnalysisFailed, "refresh_failed", "refresh failed: " + outcome.error, outcome.summary);
    }
    if (outcome.status != "success") st = status::kPartial;
    return outcome.summary;
  }

  if (op == "list_projects") {
    Registry& reg = registry(data_dir);
    reg.mark_interrupted_runs();
    json rows = json::array();
    std::error_code ec;
    for (const auto& p : reg.list_projects()) {
      json j = p.to_json();
      j["index_exists"] = fs::exists(p.index_path, ec);
      rows.push_back(std::move(j));
    }
    return {{"projects", rows}, {"data_dir", data_dir.string()}};
  }

  if (op == "project_status") {
    std::string name = require_string(args, "project");
    Registry& reg = registry(data_dir);
    reg.mark_interrupted_runs();
    auto project = reg.find_by_name(name);
    if (!project) {
      throw ProtocolError(status::kNotFound, "unknown_project", "no project named '" + name + "' is registered");
    }
    json result = {{"project", project->to_json()}};
    result["index"] = open_index(data_dir, args).summary();
    return result;
  }

  if (op == "list_runs") {
    Registry& reg = registry(data_dir);
    reg.mark_interrupted_runs();
    std::optional<int64_t> project_id;
    if (args.contains("project") && args["project"].is_string()) {
      auto p = reg.find_by_name(args["project"]);
      if (!p) {
        throw ProtocolError(status::kNotFound, "unknown_project",
                            "no project named '" + args["project"].get<std::string>() + "' is registered");
      }
      project_id = p->id;
    }
    int64_t limit = std::clamp<int64_t>(get_int(args, "limit", 20), 1, 200);
    return {{"runs", reg.list_runs(project_id, limit)}, {"data_dir", data_dir.string()}};
  }

  if (op == "get_run") {
    Registry& reg = registry(data_dir);
    reg.mark_interrupted_runs();
    int64_t run_id = get_int(args, "run_id", -1);
    if (run_id < 0) throw ProtocolError(status::kInvalidArguments, "missing_argument", "argument 'run_id' is required");
    auto run = reg.get_run(run_id, std::clamp<int64_t>(get_int(args, "diag_limit", 100), 1, 2000),
                           std::max<int64_t>(0, get_int(args, "diag_offset", 0)));
    if (!run) throw ProtocolError(status::kNotFound, "unknown_run", "no analysis run with id " + std::to_string(run_id));
    return *run;
  }

  // ---- index queries --------------------------------------------------------
  auto traverse_options = [&]() {
    TraverseOptions t;
    t.direction = args.value("direction", std::string("forward"));
    if (t.direction != "forward" && t.direction != "backward") {
      throw ProtocolError(status::kInvalidArguments, "invalid_argument", "direction must be 'forward' or 'backward'");
    }
    t.max_depth = static_cast<int>(get_int(args, "max_depth", 4));
    t.max_nodes = get_int(args, "max_nodes", 200);
    t.max_children = get_int(args, "max_children", 40);
    t.include_unresolved = get_bool(args, "include_unresolved", true);
    t.edge_kinds = get_strings(args, "edge_kinds");
    return t;
  };
  auto required_id = [&](const char* key) {
    int64_t v = get_int(args, key, -1);
    if (v < 0) throw ProtocolError(status::kInvalidArguments, "missing_argument", std::string("argument '") + key + "' is required");
    return v;
  };
  auto not_null = [&](json j, const std::string& what) {
    if (j.is_null()) throw ProtocolError(status::kNotFound, "not_found", what + " was not found in the index");
    return j;
  };

  if (op == "summary") return open_index(data_dir, args).summary();
  if (op == "search") {
    auto& idx = open_index(data_dir, args);
    return idx.search(args.value("query", std::string()), get_strings(args, "kinds"), get_bool(args, "exact", false),
                      get_int(args, "limit", 20));
  }
  if (op == "symbol") return not_null(open_index(data_dir, args).symbol(required_id("symbol_id")), "symbol");
  if (op == "edges") {
    std::string dir = args.value("direction", std::string("out"));
    if (dir != "in" && dir != "out") throw ProtocolError(status::kInvalidArguments, "invalid_argument", "direction must be 'in' or 'out'");
    auto& idx = open_index(data_dir, args);
    int64_t sid = required_id("symbol_id");
    return idx.edges(sid, dir, get_strings(args, "kinds"), get_int(args, "limit", 50), get_int(args, "offset", 0));
  }
  if (op == "traverse") {
    auto& idx = open_index(data_dir, args);
    return not_null(idx.traverse(required_id("symbol_id"), traverse_options()), "symbol");
  }
  if (op == "file") {
    auto& idx = open_index(data_dir, args);
    std::optional<int64_t> fid;
    if (args.contains("file_id")) fid = required_id("file_id");
    std::string path = args.value("path", std::string());
    if (!fid && path.empty()) throw ProtocolError(status::kInvalidArguments, "missing_argument", "file_id or path is required");
    return not_null(idx.file(fid, path, get_int(args, "symbol_limit", 200)), "file");
  }
  if (op == "file_deps") {
    std::string dir = args.value("direction", std::string("out"));
    if (dir != "in" && dir != "out") throw ProtocolError(status::kInvalidArguments, "invalid_argument", "direction must be 'in' or 'out'");
    auto& idx = open_index(data_dir, args);
    return idx.file_deps(required_id("file_id"), dir, get_int(args, "limit", 100), get_int(args, "offset", 0));
  }
  if (op == "entry_points") return open_index(data_dir, args).entry_points(get_int(args, "limit", 100));
  if (op == "routes") return open_index(data_dir, args).routes(get_int(args, "limit", 100), get_int(args, "offset", 0));
  if (op == "overview") {
    std::string focus = args.contains("focus") && args["focus"].is_string() ? args["focus"].get<std::string>() : "";
    return open_index(data_dir, args).overview(focus, static_cast<int>(get_int(args, "max_nodes", 14)),
                                               get_bool(args, "direct_files", false));
  }
  if (op == "route_flow") {
    auto& idx = open_index(data_dir, args);
    return not_null(idx.route_flow(required_id("route_id"), traverse_options()), "route");
  }
  if (op == "exceptions") {
    auto& idx = open_index(data_dir, args);
    int64_t sid = required_id("symbol_id");
    return not_null(idx.exceptions(sid, static_cast<int>(get_int(args, "depth", 3)), get_int(args, "max_nodes", 200)),
                    "symbol");
  }
  if (op == "startup_path") {
    auto& idx = open_index(data_dir, args);
    return idx.startup_path(required_id("symbol_id"), static_cast<int>(std::clamp<int64_t>(get_int(args, "max_depth", 10), 1, 25)));
  }
  if (op == "check_index") {
    json r = open_index(data_dir, args).check();
    if (!r["ok"].get<bool>()) st = status::kPartial;
    return r;
  }
  if (op == "snippet") {
    auto& idx = open_index(data_dir, args);
    return not_null(idx.snippet(required_id("file_id"), get_int(args, "line", 1),
                                static_cast<int>(get_int(args, "before", 5)), static_cast<int>(get_int(args, "after", 10))),
                    "file");
  }
  throw ProtocolError(status::kUnsupportedOperation, "unsupported_operation", "unknown operation '" + op + "'");
}

}  // namespace proj::protocol
