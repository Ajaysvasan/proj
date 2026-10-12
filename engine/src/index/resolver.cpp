#include "index/resolver.hpp"

#include <algorithm>
#include <deque>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>

#include "core/log.hpp"
#include "core/paths.hpp"

namespace proj {
namespace {

using Kind = Resolver::Value::Kind;

constexpr int64_t kPage = 512;
constexpr size_t kMaxCandidates = 8;
constexpr size_t kSymbolCacheLimit = 20000;  // entries; caches are cleared when full (bounded memory)

bool starts_with(std::string_view s, std::string_view p) { return s.size() >= p.size() && s.substr(0, p.size()) == p; }
bool ends_with(std::string_view s, std::string_view p) {
  return s.size() >= p.size() && s.substr(s.size() - p.size()) == p;
}

bool is_class_kind(const std::string& k) {
  return k == "class" || k == "interface" || k == "struct" || k == "enum" || k == "union";
}
bool is_callable_kind(const std::string& k) {
  return k == "function" || k == "method" || k == "constructor" || k == "route_handler" || k == "middleware";
}
bool is_value_kind(const std::string& k) {
  return k == "function" || k == "class" || k == "interface" || k == "variable" || k == "object" || k == "enum" ||
         k == "namespace" || k == "struct" || k == "type_alias" || k == "union";
}

// Method names so common across libraries that a name-only match would be
// meaningless; they are never resolved heuristically.
const std::set<std::string, std::less<>> kHeuristicStoplist = {
    "add", "all", "any", "append", "apply", "bind", "call", "catch", "clear", "close", "collect", "concat",
    "contains", "copy", "count", "debug", "delete", "emit", "end", "equals", "error", "execute", "filter", "find",
    "finally", "flush", "forEach", "format", "get", "has", "hashCode", "includes", "indexOf", "info", "init",
    "isEmpty", "items", "join", "json", "keys", "length", "log", "map", "next", "of", "on", "once", "open",
    "orElse", "orElseThrow", "pop", "print", "printf", "println", "push", "put", "read", "reduce", "remove",
    "render", "replace", "reset", "run", "send", "set", "size", "slice", "sort", "splice", "split", "start",
    "status", "stop", "stream", "strip", "then", "toString", "trim", "update", "values", "warn", "write"};

std::string last_segment(const std::string& s) {
  auto pos = s.find_last_of(".:");
  return pos == std::string::npos ? s : s.substr(pos + 1);
}

std::string join_route(const std::string& a, const std::string& b) {
  if (b.rfind("<dynamic", 0) == 0) return a.empty() ? b : a + " " + b;
  std::string left = a;
  while (!left.empty() && left.back() == '/') left.pop_back();
  std::string right = b;
  if (right.empty() || right == "/") return left.empty() ? (right.empty() ? "/" : right) : left;
  if (right[0] != '/' && right[0] != '*') right = "/" + right;
  std::string out = left + right;
  std::string collapsed;
  for (char c : out) {
    if (c == '/' && !collapsed.empty() && collapsed.back() == '/') continue;
    collapsed += c;
  }
  return collapsed.empty() ? "/" : collapsed;
}

bool path_has_prefix(const std::string& path, const std::string& prefix) {
  std::string p = prefix;
  while (!p.empty() && p.back() == '/') p.pop_back();
  if (p.empty()) return true;
  return path == p || starts_with(path, p + "/");
}

// "const std::shared_ptr<Foo>&" -> "Foo"; "List<User>" -> "List"; "User[]" -> "User".
std::string clean_type(std::string t, bool unwrap_smart_pointers) {
  auto strip = [&](std::string_view word) {
    size_t pos;
    while ((pos = t.find(word)) != std::string::npos) t.erase(pos, word.size());
  };
  strip("const ");
  strip("volatile ");
  strip("struct ");
  strip("class ");
  strip("typename ");
  if (unwrap_smart_pointers) {
    for (std::string_view w : {"std::unique_ptr<", "std::shared_ptr<", "std::weak_ptr<", "std::optional<",
                               "unique_ptr<", "shared_ptr<", "weak_ptr<", "optional<"}) {
      if (starts_with(t, w)) {
        auto close = t.rfind('>');
        if (close != std::string::npos && close > w.size()) t = t.substr(w.size(), close - w.size());
        break;
      }
    }
  }
  auto lt = t.find('<');
  if (lt != std::string::npos) t = t.substr(0, lt);
  std::string out;
  for (char c : t) {
    if (c == '*' || c == '&' || c == ' ' || c == '[' || c == ']' || c == '\t' || c == '?') continue;
    out += c;
  }
  return out;
}

}  // namespace

json ResolveStats::to_json() const {
  return {{"dependencies",
           {{"total", deps_total},
            {"resolved", deps_resolved},
            {"external", deps_external},
            {"unresolved", deps_unresolved},
            {"other", deps_other}}},
          {"calls",
           {{"total", calls_total},
            {"resolved", calls_resolved},
            {"ambiguous", calls_ambiguous},
            {"heuristic", calls_heuristic},
            {"unresolved", calls_unresolved},
            {"external", calls_external}}},
          {"inheritance", {{"resolved", inherits_resolved}, {"unresolved", inherits_unresolved}}},
          {"override_edges", override_edges},
          {"module_load_edges", module_load_edges},
          {"routes",
           {{"total", routes},
            {"handlers_resolved", route_handlers_resolved},
            {"handlers_unresolved", route_handlers_unresolved}}},
          {"mounts", {{"router", mounts_router}, {"middleware", mounts_middleware}, {"unresolved", mounts_unresolved}}},
          {"manifest_entry_points", manifest_entry_points}};
}

Resolver::Resolver(db::Database& db, std::filesystem::path root, DiagnosticSink& diagnostics,
                   std::vector<std::string> manifests)
    : db_(db), root_(std::move(root)), diagnostics_(diagnostics), manifests_(std::move(manifests)) {
  q_file_by_path_ = db_.prepare("SELECT id FROM files WHERE path = ?");
  q_files_by_name_ = db_.prepare("SELECT id, path FROM files WHERE name = ? ORDER BY path LIMIT 64");
  q_symbol_ = db_.prepare(
      "SELECT id, file_id, parent_id, name, kind, qualified_name, container, language, param_count, is_definition, "
      "IFNULL(param_types, '') FROM symbols WHERE id = ?");
  q_children_ = db_.prepare("SELECT id FROM symbols WHERE parent_id = ? AND name = ? ORDER BY id LIMIT 64");
  q_module_symbol_ = db_.prepare("SELECT MIN(id) FROM symbols WHERE file_id = ?");
  q_bindings_ = db_.prepare(
      "SELECT b.imported_name, b.kind, d.status, d.dst_file_id, d.specifier FROM bindings b "
      "JOIN file_deps d ON d.id = b.dep_id WHERE b.file_id = ? AND b.local_name = ? ORDER BY b.id LIMIT 8");
  q_exports_ = db_.prepare(
      "SELECT e.local_name, e.symbol_id, e.kind, d.status, d.dst_file_id, d.specifier FROM exports e "
      "LEFT JOIN file_deps d ON d.id = e.dep_id WHERE e.file_id = ? AND e.exported_name = ? ORDER BY e.id LIMIT 16");
  q_hint_ = db_.prepare("SELECT type_name FROM type_hints WHERE scope_symbol_id = ? AND name = ? ORDER BY id LIMIT 1");
  q_bases_ = db_.prepare(
      "SELECT dst_symbol_id FROM edges WHERE src_symbol_id = ? AND kind IN ('extends','implements') "
      "AND dst_symbol_id IS NOT NULL ORDER BY id LIMIT 16");
  // Cross-file candidate lists are ordered by location, not row id, so results
  // do not depend on insertion order (a refreshed index assigns new ids).
  q_by_name_kind_ = db_.prepare(
      "SELECT s.id FROM symbols s JOIN files f ON f.id = s.file_id WHERE s.name = ? AND s.kind = ? "
      "ORDER BY f.path, s.start_line, s.start_col LIMIT ?");
  q_container_ = db_.prepare(
      "SELECT s.id FROM symbols s JOIN files f ON f.id = s.file_id WHERE s.container = ? AND s.name = ? "
      "ORDER BY f.path, s.start_line, s.start_col LIMIT 32");
  q_qualified_ = db_.prepare(
      "SELECT s.id, s.qualified_name FROM symbols s JOIN files f ON f.id = s.file_id WHERE s.name = ? "
      "ORDER BY f.path, s.start_line, s.start_col LIMIT 512");
  q_same_package_type_ = db_.prepare(
      "SELECT s.id FROM symbols s JOIN files f ON f.id = s.file_id WHERE s.name = ? AND f.module_name = ? "
      "AND s.kind IN ('class','interface','enum') ORDER BY f.path, s.start_line LIMIT 4");
  q_file_lang_ = db_.prepare("SELECT language, path, module_name FROM files WHERE id = ?");
  q_dep_by_spec_ = db_.prepare(
      "SELECT status, dst_file_id FROM file_deps WHERE src_file_id = ? AND specifier = ? ORDER BY id LIMIT 1");
  q_router_ = db_.prepare("SELECT id FROM routers WHERE file_id = ? AND name = ? ORDER BY id LIMIT 1");
  q_file_symbols_named_ = db_.prepare("SELECT id FROM symbols WHERE file_id = ? AND name = ? ORDER BY id LIMIT 32");
  q_file_has_module_ = db_.prepare("SELECT 1 FROM files WHERE module_name = ? LIMIT 1");
}

ResolveStats Resolver::run() {
  run_dependencies();
  return run_relationships();
}

namespace {
template <class F>
void phase(db::Database& db, const char* name, F&& fn) {
  db::Transaction tx(db);
  fn();
  tx.commit();
  log::debug("resolver", std::string("finished ") + name);
}
}  // namespace

void Resolver::run_dependencies() {
  phase(db_, "dependencies", [&] { resolve_dependencies(); });
  phase(db_, "python submodules", [&] { resolve_python_submodule_bindings(); });
}

ResolveStats Resolver::run_relationships() {
  phase(db_, "module loads", [&] { add_module_load_edges(); });
  phase(db_, "inheritance", [&] { resolve_inheritance(); });
  resolve_calls();  // commits per batch of files
  phase(db_, "overrides", [&] { add_override_edges(); });
  phase(db_, "mounts", [&] { resolve_mounts(); });
  phase(db_, "route handlers", [&] { resolve_route_handlers(); });
  phase(db_, "route middleware", [&] { resolve_route_middleware(); });
  phase(db_, "route prefixes", [&] { compute_route_prefixes(); });
  phase(db_, "manifests", [&] { add_manifest_entry_points(); });
  return stats_;
}

// ===========================================================================
// Basic lookups
// ===========================================================================

db::Statement& Resolver::cached(const char* sql) {
  auto it = stmt_cache_.find(sql);
  if (it == stmt_cache_.end()) it = stmt_cache_.emplace(sql, db_.prepare(sql)).first;
  return it->second;
}

std::optional<Resolver::SymbolInfo> Resolver::symbol(int64_t id) {
  if (id < 0) return std::nullopt;
  if (auto it = sym_cache_.find(id); it != sym_cache_.end()) return it->second;
  q_symbol_.bind(1, id);
  std::optional<SymbolInfo> out;
  if (q_symbol_.step()) {
    SymbolInfo s;
    s.id = q_symbol_.int64(0);
    s.file_id = q_symbol_.int64(1);
    s.parent_id = q_symbol_.is_null(2) ? -1 : q_symbol_.int64(2);
    s.name = q_symbol_.text(3);
    s.kind = q_symbol_.text(4);
    s.qualified_name = q_symbol_.text(5);
    s.container = q_symbol_.text(6);
    s.language = q_symbol_.text(7);
    s.param_count = q_symbol_.is_null(8) ? -1 : static_cast<int>(q_symbol_.int64(8));
    s.is_definition = q_symbol_.int64(9) != 0;
    s.param_types = q_symbol_.text(10);
    out = s;
  }
  q_symbol_.reset();
  if (out) {
    if (sym_cache_.size() > kSymbolCacheLimit) sym_cache_.clear();
    sym_cache_.emplace(id, *out);
  }
  return out;
}

std::vector<Resolver::SymbolInfo> Resolver::children_named(int64_t parent, const std::string& name) {
  std::vector<int64_t> ids;
  q_children_.bind(1, parent).bind(2, name);
  while (q_children_.step()) ids.push_back(q_children_.int64(0));
  q_children_.reset();
  std::vector<SymbolInfo> out;
  for (auto id : ids) {
    if (auto s = symbol(id)) out.push_back(*s);
  }
  return out;
}

int64_t Resolver::module_symbol(int64_t file_id) {
  if (auto it = module_symbol_cache_.find(file_id); it != module_symbol_cache_.end()) return it->second;
  q_module_symbol_.bind(1, file_id);
  int64_t id = -1;
  if (q_module_symbol_.step() && !q_module_symbol_.is_null(0)) id = q_module_symbol_.int64(0);
  q_module_symbol_.reset();
  if (module_symbol_cache_.size() > kSymbolCacheLimit) module_symbol_cache_.clear();
  module_symbol_cache_[file_id] = id;
  return id;
}

std::string Resolver::language_of_file(int64_t file_id) {
  if (auto it = file_lang_cache_.find(file_id); it != file_lang_cache_.end()) return it->second;
  q_file_lang_.bind(1, file_id);
  std::string lang;
  if (q_file_lang_.step()) lang = q_file_lang_.text(0);
  q_file_lang_.reset();
  if (file_lang_cache_.size() > kSymbolCacheLimit) file_lang_cache_.clear();
  file_lang_cache_[file_id] = lang;
  return lang;
}

std::string Resolver::family(const std::string& lang) const {
  if (lang == "javascript" || lang == "typescript" || lang == "tsx") return "js";
  if (lang == "c" || lang == "cpp") return "c";
  if (lang == "xml" || lang == "yaml") return "config";
  return lang;
}

void Resolver::set_file_context(int64_t file_id) {
  if (file_id == file_id_) return;
  file_id_ = file_id;
  q_file_lang_.bind(1, file_id);
  if (q_file_lang_.step()) {
    file_lang_ = q_file_lang_.text(0);
    file_path_ = q_file_lang_.text(1);
    file_module_ = q_file_lang_.text(2);
  }
  q_file_lang_.reset();
  visible_files_.clear();
  if (family(file_lang_) == "c") compute_visible_files();
}

void Resolver::compute_visible_files() {
  auto& st = cached("SELECT dst_file_id FROM file_deps WHERE src_file_id = ? AND dst_file_id IS NOT NULL");
  std::deque<std::pair<int64_t, int>> queue{{file_id_, 0}};
  visible_files_.insert(file_id_);
  while (!queue.empty() && visible_files_.size() < 1000) {
    auto [f, depth] = queue.front();
    queue.pop_front();
    if (depth >= 8) continue;
    st.bind(1, f);
    std::vector<int64_t> next;
    while (st.step()) next.push_back(st.int64(0));
    st.reset();
    for (auto n : next) {
      if (visible_files_.insert(n).second) queue.push_back({n, depth + 1});
    }
  }
}

std::optional<int64_t> Resolver::file_by_path(const std::string& path) {
  q_file_by_path_.bind(1, path);
  std::optional<int64_t> id;
  if (q_file_by_path_.step()) id = q_file_by_path_.int64(0);
  q_file_by_path_.reset();
  return id;
}

std::optional<int64_t> Resolver::first_existing(const std::vector<std::string>& candidates) {
  for (const auto& c : candidates) {
    if (c.empty()) continue;
    if (auto id = file_by_path(c)) return id;
  }
  return std::nullopt;
}

// ===========================================================================
// File dependencies
// ===========================================================================

void Resolver::resolve_dependencies() {
  auto sel = db_.prepare(
      "SELECT d.id, d.kind, d.specifier, d.raw_note, f.path, f.language FROM file_deps d JOIN files f ON f.id = d.src_file_id "
      "WHERE d.status = 'pending' AND d.id > ? ORDER BY d.id LIMIT ?");
  auto upd = db_.prepare("UPDATE file_deps SET dst_file_id = ?, status = ?, note = ? WHERE id = ?");
  int64_t last = 0;
  while (true) {
    struct Row {
      int64_t id;
      std::string kind, spec, note, path, lang;
    };
    std::vector<Row> rows;
    sel.bind(1, last).bind(2, kPage);
    while (sel.step()) rows.push_back({sel.int64(0), sel.text(1), sel.text(2), sel.text(3), sel.text(4), sel.text(5)});
    sel.reset();
    if (rows.empty()) break;
    for (const auto& r : rows) {
      last = r.id;
      DepResult res = r.kind == "include" ? resolve_c_dep(r.path, r.spec, r.note == "system include")
                                          : resolve_dep(r.path, r.lang, r.kind, r.spec);
      ++stats_.deps_total;
      if (res.status == "resolved" || res.status == "heuristic") ++stats_.deps_resolved;
      else if (res.status == "external") ++stats_.deps_external;
      else if (res.status == "unresolved") ++stats_.deps_unresolved;
      else ++stats_.deps_other;
      upd.bind_id_or_null(1, res.dst).bind(2, res.status);
      std::string note = r.note;
      if (!res.note.empty()) note = note.empty() ? res.note : note + "; " + res.note;
      upd.bind_text_or_null(3, note).bind(4, r.id).run();
    }
  }
}

Resolver::DepResult Resolver::resolve_dep(const std::string& src_path, const std::string& lang,
                                          const std::string& kind, const std::string& spec) {
  auto fam = family(lang);
  if (fam == "js") return resolve_js_dep(src_path, spec);
  if (fam == "python") return resolve_python_dep(src_path, spec);
  if (fam == "java") return resolve_java_dep(kind == "static_import" ? spec + "#static" : spec);
  return {"unresolved", -1, "no resolver for this dependency kind"};
}

Resolver::DepResult Resolver::resolve_js_dep(const std::string& src_path, const std::string& spec) {
  static const std::vector<std::string> kExts = {".ts", ".tsx", ".d.ts", ".js", ".jsx", ".mjs", ".cjs", ".mts", ".cts", ".json"};
  auto candidates = [&](const std::string& base) {
    std::vector<std::string> c;
    for (std::string_view js : {".js", ".jsx", ".mjs", ".cjs"}) {
      if (ends_with(base, js)) {
        // TypeScript ESM convention: import "./x.js" refers to x.ts.
        std::string stem = base.substr(0, base.size() - js.size());
        for (const char* e : {".ts", ".tsx", ".mts", ".cts"}) c.push_back(stem + e);
      }
    }
    c.push_back(base);
    for (const auto& e : kExts) c.push_back(base + e);
    for (const auto& e : kExts) c.push_back(base.empty() ? "index" + e : base + "/index" + e);
    return c;
  };
  if (spec.empty()) return {"unresolved", -1, "empty module specifier"};
  if (starts_with(spec, "node:")) return {"external", -1, "Node.js built-in module"};
  if (spec[0] == '.') {
    auto base = paths::join_relative(paths::parent_dir(src_path), spec);
    if (!base) return {"unresolved", -1, "path leaves the project root"};
    if (auto id = first_existing(candidates(*base))) return {"resolved", *id, ""};
    return {"unresolved", -1, "no matching local file"};
  }
  if (spec[0] == '/') return {"unresolved", -1, "absolute import paths are not resolved"};
  // Common path aliases ("@/x", "~/x") and baseUrl-style imports are matched
  // against the project tree but labelled heuristic: tsconfig is not read.
  std::vector<std::string> alias_bases;
  if (starts_with(spec, "@/") || starts_with(spec, "~/")) {
    alias_bases = {"src/" + spec.substr(2), spec.substr(2)};
  } else if (spec.find('/') != std::string::npos && spec[0] != '@') {
    alias_bases = {spec, "src/" + spec};
  }
  for (const auto& b : alias_bases) {
    auto norm = paths::normalize_relative(b);
    if (!norm) continue;
    if (auto id = first_existing(candidates(*norm))) {
      return {"heuristic", *id, "bare specifier matched a project path (tsconfig paths are not read)"};
    }
  }
  return {"external", -1, "package"};
}

bool Resolver::python_dir_exists(const std::string& dir) {
  if (dir.empty()) return false;
  auto& st = cached("SELECT 1 FROM files WHERE dir = ? AND language = 'python' LIMIT 1");
  st.bind(1, dir);
  bool found = st.step();
  st.reset();
  return found;
}

std::optional<std::string> Resolver::python_package_dir(const std::string& src_path, const std::string& spec) {
  size_t dots = 0;
  while (dots < spec.size() && spec[dots] == '.') ++dots;
  std::string rest = spec.substr(dots);
  std::replace(rest.begin(), rest.end(), '.', '/');
  if (dots > 0) {
    std::string dir = paths::parent_dir(src_path);
    for (size_t i = 1; i < dots; ++i) {
      if (dir.empty()) return std::nullopt;
      dir = paths::parent_dir(dir);
    }
    return rest.empty() ? dir : (dir.empty() ? rest : dir + "/" + rest);
  }
  for (const char* root : {"", "src"}) {
    std::string base = std::string(root).empty() ? rest : std::string(root) + "/" + rest;
    if (python_dir_exists(base)) return base;
  }
  return std::nullopt;
}

Resolver::DepResult Resolver::resolve_python_dep(const std::string& src_path, const std::string& spec) {
  size_t dots = 0;
  while (dots < spec.size() && spec[dots] == '.') ++dots;
  std::string rest = spec.substr(dots);
  std::replace(rest.begin(), rest.end(), '.', '/');
  auto module_candidates = [](const std::string& base) {
    if (base.empty()) return std::vector<std::string>{"__init__.py"};
    return std::vector<std::string>{base + ".py", base + "/__init__.py", base + ".pyi"};
  };
  if (dots > 0) {
    std::string dir = paths::parent_dir(src_path);
    for (size_t i = 1; i < dots; ++i) {
      if (dir.empty()) return {"unresolved", -1, "relative import goes above the project root"};
      dir = paths::parent_dir(dir);
    }
    if (rest.empty()) {
      if (auto id = file_by_path(dir.empty() ? "__init__.py" : dir + "/__init__.py")) return {"resolved", *id, ""};
      return {"package", -1, "namespace package (no __init__.py)"};
    }
    std::string base = dir.empty() ? rest : dir + "/" + rest;
    if (auto id = first_existing(module_candidates(base))) return {"resolved", *id, ""};
    if (python_dir_exists(base)) return {"package", -1, "namespace package (no __init__.py)"};
    return {"unresolved", -1, "no matching local module"};
  }
  // Absolute imports: try the project root, src/, then each ancestor of the
  // importing file from the top down (covers src layouts and monorepos).
  std::vector<std::string> roots = {"", "src"};
  std::vector<std::string> ancestors;
  for (std::string d = paths::parent_dir(src_path); !d.empty(); d = paths::parent_dir(d)) ancestors.push_back(d);
  std::reverse(ancestors.begin(), ancestors.end());
  for (auto& a : ancestors) {
    if (a != "src") roots.push_back(a);
  }
  for (const auto& r : roots) {
    std::string base = r.empty() ? rest : r + "/" + rest;
    if (auto id = first_existing(module_candidates(base))) {
      return {"resolved", *id, r.empty() ? "" : "resolved under source root '" + r + "'"};
    }
  }
  for (const auto& r : roots) {
    std::string base = r.empty() ? rest : r + "/" + rest;
    if (python_dir_exists(base)) return {"package", -1, "namespace package (no __init__.py)"};
  }
  std::string first = rest.substr(0, rest.find('/'));
  auto& dir_exists = cached("SELECT 1 FROM files WHERE dir = ? OR dir LIKE ? LIMIT 1");
  for (const auto& r : {std::string(), std::string("src")}) {
    std::string d = r.empty() ? first : r + "/" + first;
    if (file_by_path(d + ".py")) return {"unresolved", -1, "local module exists but the imported name does not"};
    dir_exists.bind(1, d).bind(2, d + "/%");
    bool exists = dir_exists.step();
    dir_exists.reset();
    if (exists) return {"unresolved", -1, "local package found but the module was not"};
  }
  return {"external", -1, "package or standard library"};
}

Resolver::DepResult Resolver::resolve_java_dep(const std::string& raw) {
  std::string spec = raw;
  bool is_static = ends_with(spec, "#static");
  if (is_static) spec = spec.substr(0, spec.size() - 7);
  auto class_file = [&](const std::string& qname) -> std::optional<int64_t> {
    std::string simple = last_segment(qname);
    std::string suffix = qname;
    std::replace(suffix.begin(), suffix.end(), '.', '/');
    suffix += ".java";
    q_files_by_name_.bind(1, simple + ".java");
    std::optional<int64_t> found;
    while (q_files_by_name_.step()) {
      std::string p = q_files_by_name_.text(1);
      if (p == suffix || ends_with(p, "/" + suffix)) {
        found = q_files_by_name_.int64(0);
        break;
      }
    }
    q_files_by_name_.reset();
    if (found) return found;
    // Non-standard directory layouts: match the declared qualified name.
    q_qualified_.bind(1, simple);
    while (q_qualified_.step()) {
      if (q_qualified_.text(1) == qname) {
        auto s = q_qualified_.int64(0);
        q_qualified_.reset();
        if (auto info = symbol(s)) return info->file_id;
        return std::nullopt;
      }
    }
    q_qualified_.reset();
    return std::nullopt;
  };
  if (ends_with(spec, ".*")) {
    std::string pkg = spec.substr(0, spec.size() - 2);
    q_file_has_module_.bind(1, pkg);
    bool exists = q_file_has_module_.step();
    q_file_has_module_.reset();
    if (exists) return {"package", -1, "wildcard import of project package " + pkg};
    if (auto id = class_file(pkg)) return {"resolved", *id, "static members of a project class"};
    return {"external", -1, "package"};
  }
  if (auto id = class_file(spec)) return {"resolved", *id, ""};
  if (is_static) {
    auto dot = spec.rfind('.');
    if (dot != std::string::npos) {
      if (auto id = class_file(spec.substr(0, dot))) return {"resolved", *id, "static import"};
    }
  }
  return {"external", -1, "class not found in project"};
}

Resolver::DepResult Resolver::resolve_c_dep(const std::string& src_path, const std::string& spec, bool system) {
  if (!system) {
    if (auto rel = paths::join_relative(paths::parent_dir(src_path), spec)) {
      if (auto id = file_by_path(*rel)) return {"resolved", *id, ""};
    }
    if (auto norm = paths::normalize_relative(spec)) {
      if (auto id = file_by_path(*norm)) return {"resolved", *id, "relative to project root"};
    }
  }
  auto norm = paths::normalize_relative(spec);
  if (norm) {
    std::vector<std::pair<int64_t, std::string>> matches;
    q_files_by_name_.bind(1, paths::base_name(*norm));
    while (q_files_by_name_.step()) {
      std::string p = q_files_by_name_.text(1);
      if (p == *norm || ends_with(p, "/" + *norm)) matches.emplace_back(q_files_by_name_.int64(0), p);
    }
    q_files_by_name_.reset();
    if (matches.size() == 1) return {"resolved", matches[0].first, "matched by path suffix"};
    if (matches.size() > 1) {
      // Prefer the candidate sharing the longest directory prefix with the includer.
      std::string src_dir = paths::parent_dir(src_path);
      auto common = [&](const std::string& p) {
        size_t n = 0;
        while (n < p.size() && n < src_dir.size() && p[n] == src_dir[n]) ++n;
        return n;
      };
      std::sort(matches.begin(), matches.end(), [&](auto& a, auto& b) { return common(a.second) > common(b.second); });
      if (common(matches[0].second) > common(matches[1].second)) {
        return {"resolved", matches[0].first,
                "nearest of " + std::to_string(matches.size()) + " files with this path suffix"};
      }
      return {"ambiguous", -1, std::to_string(matches.size()) + " project files match this include"};
    }
  }
  return {system ? "external" : "unresolved", -1, system ? "system or third-party header" : "header not found in project"};
}

void Resolver::resolve_python_submodule_bindings() {
  // `from pkg import mod` imports a submodule when pkg/mod.py exists.
  auto sel = db_.prepare(
      "SELECT b.id, b.file_id, b.imported_name, d.specifier, d.line, d.col, d.scope_symbol_id, f.path FROM bindings b "
      "JOIN file_deps d ON d.id = b.dep_id JOIN files f ON f.id = b.file_id "
      "WHERE d.kind = 'from_import' AND b.kind = 'named' AND b.id > ? ORDER BY b.id LIMIT ?");
  auto ins = db_.prepare(
      "INSERT INTO file_deps(src_file_id, dst_file_id, scope_symbol_id, kind, specifier, line, col, status, note, generated) "
      "VALUES (?,?,?,'from_import',?,?,?,'resolved','submodule import',1)");
  auto upd = db_.prepare("UPDATE bindings SET dep_id = ?, kind = 'module', imported_name = '*' WHERE id = ?");
  int64_t last = 0;
  while (true) {
    struct Row {
      int64_t id, file;
      std::string name, spec;
      int64_t line, col, scope;
      std::string src;
    };
    std::vector<Row> rows;
    sel.bind(1, last).bind(2, kPage);
    while (sel.step()) {
      rows.push_back({sel.int64(0), sel.int64(1), sel.text(2), sel.text(3), sel.int64(4), sel.int64(5),
                      sel.is_null(6) ? -1 : sel.int64(6), sel.text(7)});
    }
    sel.reset();
    if (rows.empty()) break;
    for (const auto& r : rows) {
      last = r.id;
      auto pkg = python_package_dir(r.src, r.spec);
      if (!pkg) continue;
      std::string base = pkg->empty() ? r.name : *pkg + "/" + r.name;
      auto sub = first_existing({base + ".py", base + "/__init__.py"});
      if (!sub) continue;
      std::string spec = r.spec + (r.spec.back() == '.' ? "" : ".") + r.name;
      ins.bind(1, r.file).bind(2, *sub).bind_id_or_null(3, r.scope).bind(4, spec).bind(5, r.line).bind(6, r.col).run();
      upd.bind(1, db_.last_insert_rowid()).bind(2, r.id).run();
    }
  }
}

void Resolver::add_module_load_edges() {
  // Importing a JS/TS or Python module at top level executes its top-level
  // code, which is how startup files reach route registration.
  db_.exec(
      "INSERT INTO edges(kind, raw_kind, generated, src_symbol_id, dst_symbol_id, file_id, line, col, status, target_name, "
      "target_text, note) SELECT 'loads_module', 'loads_module', 1, d.scope_symbol_id, (SELECT MIN(id) FROM symbols WHERE file_id = d.dst_file_id), "
      "d.src_file_id, d.line, d.col, 'resolved', d.specifier, d.specifier, 'module-level import executes the module' "
      "FROM file_deps d JOIN files f ON f.id = d.src_file_id JOIN symbols s ON s.id = d.scope_symbol_id "
      "WHERE d.dst_file_id IS NOT NULL AND d.status IN ('resolved','heuristic') AND s.kind = 'module' "
      "AND d.kind IN ('import','require','from_import','reexport') "
      "AND f.language IN ('javascript','typescript','tsx','python')");
  stats_.module_load_edges = db_.changes();
}

// ===========================================================================
// Names, exports, members
// ===========================================================================

Resolver::Value Resolver::resolve_name(int64_t file, int64_t scope, const std::string& name, int depth) {
  Value v;
  if (depth > 8 || name.empty()) return v;
  std::string fam = family(language_of_file(file));
  // Lexical scopes, innermost first. Class bodies are not lexical scopes for
  // code inside methods in JS or Python.
  int hops = 0;
  for (auto s = symbol(scope); s && hops < 64; s = symbol(s->parent_id), ++hops) {
    if (is_class_kind(s->kind) || s->kind == "object") continue;
    auto cands = children_named(s->id, name);
    const SymbolInfo* best = nullptr;
    for (const auto& c : cands) {
      if (!is_value_kind(c.kind)) continue;
      if (!best || (c.is_definition && !best->is_definition)) best = &c;
    }
    if (best) {
      v.kind = Kind::Symbol;
      v.id = best->id;
      return v;
    }
    if (fam == "js" || fam == "python") {
      // A parameter or local variable at this level shadows outer names.
      q_hint_.bind(1, s->id).bind(2, name);
      std::optional<std::string> t;
      if (q_hint_.step()) t = q_hint_.text(0);
      q_hint_.reset();
      if (t) {
        if (*t != "?") {
          v.kind = Kind::Instance;
          v.type = *t;
          v.context_file = file;
          v.context_scope = s->id;
          return v;
        }
        v.kind = Kind::Unresolved;
        v.note = "'" + name + "' has no statically known type (" +
                 std::string(s->kind == "module" ? "module-level variable" : "parameter or local variable") + ")";
        return v;
      }
    }
  }
  // Import bindings.
  struct B {
    std::string imported, kind, status, spec;
    int64_t dst;
  };
  std::vector<B> binds;
  q_bindings_.bind(1, file).bind(2, name);
  while (q_bindings_.step()) {
    binds.push_back({q_bindings_.text(0), q_bindings_.text(1), q_bindings_.text(2), q_bindings_.text(4),
                     q_bindings_.is_null(3) ? -1 : q_bindings_.int64(3)});
  }
  q_bindings_.reset();
  for (const auto& b : binds) {
    if (b.dst >= 0) {
      if (b.kind == "namespace" || b.kind == "module") {
        v.kind = Kind::Module;
        v.id = b.dst;
        v.commonjs = b.kind == "module";
        return v;
      }
      Value e = export_value(b.dst, b.kind == "default" ? "default" : b.imported, depth + 1);
      if (e.kind != Kind::None) return e;
      v.kind = Kind::Unresolved;
      v.note = "'" + b.imported + "' is not exported by " + b.spec;
      return v;
    }
    if (b.status == "external" || b.status == "package") {
      v.kind = Kind::External;
      v.note = "imported from " + b.spec;
      return v;
    }
    v.kind = Kind::Unresolved;
    v.note = "import of '" + b.spec + "' could not be resolved";
    return v;
  }
  if (fam == "python") {
    // from x import *: every public module-level name of x becomes visible.
    std::vector<int64_t> stars;
    q_bindings_.bind(1, file).bind(2, "*");
    while (q_bindings_.step()) {
      if (!q_bindings_.is_null(3)) stars.push_back(q_bindings_.int64(3));
    }
    q_bindings_.reset();
    for (auto dst : stars) {
      Value e = export_value(dst, name, depth + 1);
      if (e.kind != Kind::None) return e;
    }
  }
  return v;
}

Resolver::Value Resolver::export_value(int64_t file, const std::string& name, int depth) {
  Value v;
  if (depth > 8) return v;
  std::string fam = family(language_of_file(file));
  int64_t mod = module_symbol(file);
  if (fam == "python") {
    for (const auto& c : children_named(mod, name)) {
      if (is_value_kind(c.kind)) {
        v.kind = Kind::Symbol;
        v.id = c.id;
        return v;
      }
    }
    Value through = resolve_name(file, mod, name, depth + 1);  // re-exported via import
    if (through.kind != Kind::None) return through;
    return v;
  }
  struct E {
    std::string local, kind, status, spec;
    int64_t symbol, dst;
  };
  auto load = [&](const std::string& exported) {
    std::vector<E> rows;
    q_exports_.bind(1, file).bind(2, exported);
    while (q_exports_.step()) {
      rows.push_back({q_exports_.text(0), q_exports_.text(2), q_exports_.text(3), q_exports_.text(5),
                      q_exports_.is_null(1) ? -1 : q_exports_.int64(1), q_exports_.is_null(4) ? -1 : q_exports_.int64(4)});
    }
    q_exports_.reset();
    return rows;
  };
  for (const auto& e : load(name)) {
    if (e.kind == "symbol") {
      if (e.symbol >= 0) {
        v.kind = Kind::Symbol;
        v.id = e.symbol;
        return v;
      }
      Value r = resolve_name(file, mod, e.local, depth + 1);
      if (r.kind != Kind::None) return r;
    } else if (e.kind == "instance") {
      v.kind = Kind::Instance;
      v.type = e.local;
      v.context_file = file;
      return v;
    } else if (e.kind == "reexport" || e.kind == "reexport_all") {
      if (e.dst >= 0) {
        if (e.local == "*" || e.kind == "reexport_all") {
          v.kind = Kind::Module;
          v.id = e.dst;
          v.commonjs = true;
          return v;
        }
        return export_value(e.dst, e.local, depth + 1);
      }
      v.kind = e.status == "external" ? Kind::External : Kind::Unresolved;
      v.note = "re-exported from " + e.spec;
      return v;
    }
  }
  if (name != "default") {
    for (const auto& e : load("*")) {
      if (e.dst < 0) continue;
      Value r = export_value(e.dst, name, depth + 1);
      if (r.kind != Kind::None) return r;
    }
  }
  return v;
}

Resolver::Outcome Resolver::pick(std::vector<SymbolInfo> cands, int arg_count, const std::string& note) {
  Outcome o;
  // Collapse declaration/definition pairs (C/C++ headers): keep the definition.
  // Same-arity overloads differ in their parameter types and stay separate.
  auto same_params = [](const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return true;
    auto squash = [](const std::string& s) {
      std::string out;
      for (char ch : s) {
        if (!std::isspace(static_cast<unsigned char>(ch))) out += ch;
      }
      return out;
    };
    return squash(a) == squash(b);
  };
  std::vector<SymbolInfo> unique;
  for (const auto& c : cands) {
    auto it = std::find_if(unique.begin(), unique.end(), [&](const SymbolInfo& u) {
      return u.qualified_name == c.qualified_name && u.param_count == c.param_count && u.id != c.id &&
             family(u.language) == "c" && family(c.language) == "c" && same_params(u.param_types, c.param_types);
    });
    if (it == unique.end()) {
      if (std::none_of(unique.begin(), unique.end(), [&](const SymbolInfo& u) { return u.id == c.id; })) unique.push_back(c);
    } else if (c.is_definition && !it->is_definition) {
      *it = c;
    }
  }
  if (unique.empty()) {
    o.status = "unresolved";
    return o;
  }
  if (unique.size() == 1) {
    o.status = "resolved";
    o.targets = {unique[0].id};
    o.note = note;
    return o;
  }
  if (arg_count >= 0) {
    std::vector<SymbolInfo> by_args;
    for (const auto& c : unique) {
      if (c.param_count == arg_count || c.param_count < 0) by_args.push_back(c);
    }
    if (by_args.size() == 1) {
      o.status = "resolved";
      o.targets = {by_args[0].id};
      o.note = note.empty() ? "overload selected by argument count" : note + "; overload selected by argument count";
      return o;
    }
    if (!by_args.empty()) unique = by_args;
    // Same arity: compare the argument types visible at the call site.
    if (unique.size() > 1 && arg_count == call_ctx_.arg_count && !call_ctx_.arg_types.empty()) {
      std::string type_note;
      auto narrowed = narrow_by_types(unique, &type_note);
      if (narrowed.size() == 1) {
        o.status = "resolved";
        o.targets = {narrowed[0].id};
        o.note = note.empty() ? type_note : note + "; " + type_note;
        return o;
      }
      if (!narrowed.empty()) unique = narrowed;
    }
  }
  o.status = "ambiguous";
  for (size_t i = 0; i < unique.size() && i < kMaxCandidates; ++i) o.targets.push_back(unique[i].id);
  o.note = std::to_string(unique.size()) + " candidates";
  return o;
}

Resolver::Outcome Resolver::method_lookup(int64_t cls, const std::string& name, int arg_count, int depth) {
  Outcome none;
  none.status = "unresolved";
  if (cls < 0 || depth > 6) return none;
  auto info = symbol(cls);
  if (!info) return none;
  std::vector<SymbolInfo> cands;
  for (const auto& c : children_named(cls, name)) {
    if (is_callable_kind(c.kind)) cands.push_back(c);
  }
  if (family(info->language) == "c") {
    // Out-of-line member definitions (void A::f() {...}) carry the class as container.
    std::vector<int64_t> ids;
    q_container_.bind(1, info->qualified_name).bind(2, name);
    while (q_container_.step()) ids.push_back(q_container_.int64(0));
    q_container_.reset();
    for (auto id : ids) {
      if (auto s = symbol(id); s && is_callable_kind(s->kind)) cands.push_back(*s);
    }
  }
  if (!cands.empty()) return pick(cands, arg_count, depth > 0 ? "inherited from a base class" : "");
  std::vector<int64_t> bases;
  q_bases_.bind(1, cls);
  while (q_bases_.step()) bases.push_back(q_bases_.int64(0));
  q_bases_.reset();
  for (auto b : bases) {
    Outcome o = method_lookup(b, name, arg_count, depth + 1);
    if (o.status != "unresolved") return o;
  }
  if (depth == 0) {
    // Not declared in project code. If the hierarchy reaches a base class
    // outside the project, the method is inherited from it.
    std::string base = external_base(cls, 0);
    if (!base.empty()) {
      Outcome ext;
      ext.status = "external";
      ext.note = "not declared in " + info->name + "; inherited from external base " + base;
      return ext;
    }
    none.note = "method not declared in " + info->name + " or its project base classes";
  }
  return none;
}

std::string Resolver::external_base(int64_t cls, int depth) {
  if (cls < 0 || depth > 6) return "";
  std::vector<std::pair<std::string, int64_t>> rows;  // (status or "", target) per base edge
  auto& st = cached(
      "SELECT status, IFNULL(dst_symbol_id, -1), IFNULL(target_text, '') FROM edges WHERE src_symbol_id = ? "
      "AND kind IN ('extends','implements') ORDER BY id");
  st.bind(1, cls);
  std::vector<std::tuple<std::string, int64_t, std::string>> bases;
  while (st.step()) bases.emplace_back(st.text(0), st.int64(1), st.text(2));
  st.reset();  // before recursing: the statement is shared
  for (const auto& [status, dst, text] : bases) {
    if (status == "external") return text;
  }
  for (const auto& [status, dst, text] : bases) {
    if (dst >= 0) {
      std::string b = external_base(dst, depth + 1);
      if (!b.empty()) return b;
    }
  }
  return "";
}

int64_t Resolver::enclosing_class(int64_t scope) {
  int hops = 0;
  for (auto s = symbol(scope); s && hops < 64; s = symbol(s->parent_id), ++hops) {
    if (is_class_kind(s->kind)) return s->id;
    if (!s->container.empty()) {
      // C++ out-of-line member: find the class by qualified name.
      std::string simple = last_segment(s->container);
      std::vector<std::pair<int64_t, std::string>> rows;
      q_qualified_.bind(1, simple);
      while (q_qualified_.step()) rows.emplace_back(q_qualified_.int64(0), q_qualified_.text(1));
      q_qualified_.reset();
      for (auto& [id, qn] : rows) {
        if (qn != s->container) continue;
        if (auto c = symbol(id); c && is_class_kind(c->kind)) return id;
      }
      return -1;
    }
  }
  return -1;
}

std::optional<std::string> Resolver::hint_in_scope_chain(int64_t scope, const std::string& name) {
  bool dynamic = family(file_lang_) == "js" || family(file_lang_) == "python";
  int hops = 0;
  for (auto s = symbol(scope); s && hops < 64; s = symbol(s->parent_id), ++hops) {
    if (dynamic && is_class_kind(s->kind)) continue;
    q_hint_.bind(1, s->id).bind(2, name);
    std::optional<std::string> t;
    if (q_hint_.step()) t = q_hint_.text(0);
    q_hint_.reset();
    if (t) return t;
    if (!dynamic && !s->container.empty()) {
      int64_t cls = enclosing_class(s->id);
      if (cls >= 0) {
        q_hint_.bind(1, cls).bind(2, name);
        if (q_hint_.step()) t = q_hint_.text(0);
        q_hint_.reset();
        if (t) return t;
      }
    }
  }
  return std::nullopt;
}

// Type of a value produced by a call ("@call:<callee>" hint): the class the
// callee instantiates or declares as its return type. An external callee makes
// the value external (reported through `note`).
int64_t Resolver::call_result_type(int64_t file, int64_t scope, const std::string& spec, std::string* note) {
  std::string key = std::to_string(file) + "|" + std::to_string(scope) + "|" + spec;
  if (auto it = call_type_cache_.find(key); it != call_type_cache_.end()) {
    if (note && !it->second.second.empty()) *note = it->second.second;
    return it->second.first;
  }
  std::string computed_note;
  int64_t cls = evaluate_call_type(file, scope, spec, &computed_note);
  if (call_type_cache_.size() > kSymbolCacheLimit) call_type_cache_.clear();
  call_type_cache_.emplace(key, std::make_pair(cls, computed_note));
  if (note && !computed_note.empty()) *note = computed_note;
  return cls;
}

int64_t Resolver::evaluate_call_type(int64_t file, int64_t scope, const std::string& spec, std::string* note) {
  if (type_depth_ >= 8) return -1;
  struct Guard {
    int& d;
    explicit Guard(int& depth) : d(depth) { ++d; }
    ~Guard() { --d; }
  } guard(type_depth_);
  // The producing call has its own (unknown) arguments.
  struct CtxGuard {
    CallContext& ctx;
    CallContext saved;
    explicit CtxGuard(CallContext& c) : ctx(c), saved(std::move(c)) { ctx = CallContext{}; }
    ~CtxGuard() { ctx = std::move(saved); }
  } ctx_guard(call_ctx_);
  std::string callee = spec;
  bool awaited = starts_with(callee, "await ");
  if (awaited) callee = callee.substr(6);
  // Split "a.b.c" / "ns::f" / "p->f" into receiver and name.
  size_t cut = std::string::npos, sep_len = 0;
  for (std::string_view sep : {".", "::", "->"}) {
    size_t p = callee.rfind(sep);
    if (p != std::string::npos && (cut == std::string::npos || p > cut)) {
      cut = p;
      sep_len = sep.size();
    }
  }
  std::string receiver = cut == std::string::npos ? "" : callee.substr(0, cut);
  std::string name = cut == std::string::npos ? callee : callee.substr(cut + sep_len);
  auto identifier = [](const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$'; });
  };
  std::string rk;
  std::string recv = receiver;
  if (receiver.empty()) rk = "none";
  else if (receiver == "this" || receiver == "self" || receiver == "cls") rk = "this";
  else if ((starts_with(receiver, "this.") || starts_with(receiver, "self.")) && identifier(receiver.substr(5))) {
    rk = "this_member";
    recv = receiver.substr(5);
  } else if (identifier(receiver)) rk = "identifier";
  else if (receiver.find('(') == std::string::npos) rk = "qualified";
  else return -1;  // the callee is itself a computed expression
  int64_t saved = file_id_;
  set_file_context(file);
  Outcome o = resolve_call(scope >= 0 ? scope : module_symbol(file), "calls", name, recv, rk, -1);
  set_file_context(saved);
  if (o.status == "external") {
    if (note) *note = "(value returned by external " + callee + ")";
    return -1;
  }
  if (!(o.status == "resolved" || o.status == "heuristic") || o.targets.size() != 1) return -1;
  auto target = symbol(o.targets[0]);
  if (!target) return -1;
  if (is_class_kind(target->kind)) return target->id;
  if (target->kind == "constructor") return enclosing_class(target->id);
  q_hint_.bind(1, target->id).bind(2, "#return");
  std::string ret;
  if (q_hint_.step()) ret = q_hint_.text(0);
  q_hint_.reset();
  if (ret.empty()) return -1;
  if (awaited && starts_with(ret, "Promise<") && ret.back() == '>') ret = ret.substr(8, ret.size() - 9);
  return resolve_type(target->file_id, target->parent_id >= 0 ? target->parent_id : target->id, ret, note);
}

int64_t Resolver::resolve_type(int64_t file, int64_t scope, const std::string& type, std::string* note) {
  if (starts_with(type, "@call:")) return call_result_type(file, scope, type.substr(6), note);
  std::string fam = family(language_of_file(file));
  if (fam == "java") return java_type(file, type, note);
  if (fam == "c") {
    int64_t cls = cpp_type(scope, type);
    if (cls < 0 && note && starts_with(clean_type(type, false), "std::")) *note = "(C++ standard library)";
    return cls;
  }
  std::string t = clean_type(type, false);
  if (t.empty() || t == "?") return -1;
  static const std::set<std::string, std::less<>> kPythonBuiltins = {
      "str", "int", "float", "bool", "bytes", "bytearray", "list", "dict", "set", "frozenset", "tuple", "object",
      "complex", "List", "Dict", "Set", "Tuple", "Any", "Callable", "Iterable", "Iterator", "Sequence", "Mapping",
      "Generator", "AsyncIterator", "AsyncGenerator", "Union", "Type", "deque", "defaultdict", "OrderedDict", "Counter"};
  static const std::set<std::string, std::less<>> kJsBuiltins = {
      "string", "number", "boolean", "any", "unknown", "object", "bigint", "symbol", "Array", "ReadonlyArray", "Map",
      "Set", "WeakMap", "WeakSet", "Promise", "Record", "Partial", "Pick", "Omit", "Readonly", "Required", "Date",
      "RegExp", "Error", "String", "Number", "Boolean", "Object", "Function", "Buffer", "URL", "Uint8Array",
      "ArrayBuffer", "Iterable", "AsyncIterable", "JSON", "Math"};
  if ((fam == "python" && kPythonBuiltins.count(t)) || (fam == "js" && kJsBuiltins.count(t))) {
    if (note) *note = fam == "python" ? "(Python built-in type)" : "(JavaScript built-in type)";
    return -1;
  }
  Value v;
  auto dot = t.find('.');
  if (dot == std::string::npos) {
    v = resolve_name(file, scope, t);
  } else {
    Value base = resolve_name(file, scope, t.substr(0, dot));
    if (base.kind == Kind::Module) v = export_value(base.id, last_segment(t));
    else if (base.kind == Kind::External) v = base;
  }
  if (v.kind == Kind::Symbol) {
    if (auto s = symbol(v.id); s && is_class_kind(s->kind)) return s->id;
  }
  if (note && v.kind == Kind::External) *note = v.note;
  return -1;
}

Resolver::Outcome Resolver::member_of(const Value& v, const std::string& name, int arg_count, int depth) {
  Outcome o;
  o.status = "unresolved";
  if (depth > 6) return o;
  switch (v.kind) {
    case Kind::Symbol: {
      auto s = symbol(v.id);
      if (!s) return o;
      if (is_class_kind(s->kind)) return method_lookup(s->id, name, arg_count);
      if (s->kind == "object" || s->kind == "namespace") {
        auto kids = children_named(s->id, name);
        std::vector<SymbolInfo> callables;
        for (auto& k : kids) {
          if (is_callable_kind(k.kind) || is_value_kind(k.kind)) callables.push_back(k);
        }
        return pick(callables, arg_count, "");
      }
      if (s->kind == "module") return member_of(Value{Kind::Module, s->file_id}, name, arg_count, depth + 1);
      if (s->kind == "variable") {
        q_hint_.bind(1, s->parent_id).bind(2, s->name);
        std::optional<std::string> t;
        if (q_hint_.step()) t = q_hint_.text(0);
        q_hint_.reset();
        if (t && *t != "?") {
          std::string ext;
          int64_t cls = resolve_type(s->file_id, s->parent_id, *t, &ext);
          if (cls >= 0) return method_lookup(cls, name, arg_count);
          if (!ext.empty()) {
            o.status = "external";
            o.note = "type " + *t + " " + ext;
            return o;
          }
          o.note = "type " + *t + " of '" + s->name + "' is not a project class";
          return o;
        }
        o.note = "type of '" + s->name + "' is not known";
      }
      return o;
    }
    case Kind::Module: {
      Value e = export_value(v.id, name);
      if (e.kind == Kind::Symbol) {
        o.status = "resolved";
        o.targets = {e.id};
        return o;
      }
      if (e.kind == Kind::External) {
        o.status = "external";
        o.note = e.note;
        return o;
      }
      if (v.commonjs) {
        // require(x).m reads module.exports.m: look through the default export.
        Value d = export_value(v.id, "default");
        if (d.kind != Kind::None && !(d.kind == Kind::Module && d.id == v.id)) {
          return member_of(d, name, arg_count, depth + 1);
        }
      }
      o.note = "'" + name + "' is not exported by the imported module";
      return o;
    }
    case Kind::Instance: {
      std::string ext;
      int64_t cls = resolve_type(v.context_file, v.context_scope >= 0 ? v.context_scope : module_symbol(v.context_file),
                                 v.type, &ext);
      if (cls >= 0) return method_lookup(cls, name, arg_count);
      if (!ext.empty()) {
        o.status = "external";
        o.note = "receiver type " + v.type + " " + ext;
        return o;
      }
      o.note = starts_with(v.type, "@call:") ? "type of the value returned by " + v.type.substr(6) + " is not known"
                                             : "class '" + v.type + "' not found";
      return o;
    }
    case Kind::External:
      o.status = "external";
      o.note = v.note;
      return o;
    case Kind::Unresolved:
      o.note = v.note;
      return o;
    case Kind::None:
      return o;
  }
  return o;
}

Resolver::Outcome Resolver::instantiate(int64_t cls, int arg_count) {
  auto info = symbol(cls);
  Outcome o;
  o.kind = "instantiates";
  if (!info) {
    o.status = "unresolved";
    return o;
  }
  std::vector<SymbolInfo> ctors;
  for (const std::string& n : {std::string("constructor"), std::string("__init__"), info->name}) {
    for (const auto& c : children_named(cls, n)) {
      if (c.kind == "constructor") ctors.push_back(c);
    }
  }
  if (family(info->language) == "c") {
    std::vector<int64_t> ids;
    q_container_.bind(1, info->qualified_name).bind(2, info->name);
    while (q_container_.step()) ids.push_back(q_container_.int64(0));
    q_container_.reset();
    for (auto id : ids) {
      if (auto s = symbol(id); s && s->kind == "constructor") ctors.push_back(*s);
    }
  }
  if (!ctors.empty()) {
    Outcome p = pick(ctors, arg_count, "constructor");
    p.kind = "instantiates";
    return p;
  }
  o.status = "resolved";
  o.targets = {cls};
  o.note = "class (no explicit constructor)";
  return o;
}

Resolver::Outcome Resolver::value_call(const Value& v, int arg_count, bool instantiate_call) {
  Outcome o;
  o.status = "unresolved";
  switch (v.kind) {
    case Kind::Symbol: {
      auto s = symbol(v.id);
      if (!s) return o;
      if (is_class_kind(s->kind)) return instantiate(s->id, arg_count);
      if (is_callable_kind(s->kind)) {
        o.status = "resolved";
        o.targets = {s->id};
        if (instantiate_call) o.kind = "instantiates";
        return o;
      }
      o.note = "'" + s->name + "' is a " + s->kind + ", not a known function";
      return o;
    }
    case Kind::Module: {
      Value d = export_value(v.id, "default");
      if (d.kind == Kind::Symbol) return value_call(d, arg_count, instantiate_call);
      o.note = "module has no statically known default export";
      return o;
    }
    case Kind::External:
      o.status = "external";
      o.note = v.note;
      return o;
    case Kind::Unresolved:
      o.note = v.note;
      return o;
    case Kind::Instance:
      o.note = "calling an object instance";
      return o;
    case Kind::None:
      o.note = "name is not defined in the project";
      return o;
  }
  return o;
}

Resolver::Outcome Resolver::heuristic(const std::string& name, const std::string& receiver_kind) {
  Outcome o;
  o.status = "unresolved";
  if (receiver_kind != "identifier" && receiver_kind != "this_member" && receiver_kind != "qualified") return o;
  if (name.size() < 3 || kHeuristicStoplist.count(name)) return o;
  std::vector<int64_t> ids;
  q_by_name_kind_.bind(1, name).bind(2, "method").bind(3, static_cast<int64_t>(64));
  while (q_by_name_kind_.step()) ids.push_back(q_by_name_kind_.int64(0));
  q_by_name_kind_.reset();
  std::vector<int64_t> same_family;
  std::string fam = family(file_lang_);
  for (auto id : ids) {
    if (auto s = symbol(id); s && family(s->language) == fam && s->is_definition) same_family.push_back(id);
  }
  if (same_family.size() == 1) {
    o.status = "heuristic";
    o.targets = same_family;
    o.note = "receiver type unknown; the only project method with this name";
  } else if (same_family.size() > 1 && same_family.size() <= 5) {
    o.status = "ambiguous";
    o.targets = same_family;
    o.note = "receiver type unknown; " + std::to_string(same_family.size()) + " project methods share this name";
  }
  return o;
}

// ===========================================================================
// Call resolution
// ===========================================================================

Resolver::Outcome Resolver::resolve_typed_receiver(int64_t scope, const std::string& type, const std::string& name,
                                                   int arg_count) {
  Outcome o;
  o.status = "unresolved";
  std::string ext;
  int64_t cls = resolve_type(file_id_, scope, type, &ext);
  if (cls >= 0) return method_lookup(cls, name, arg_count);
  if (!ext.empty()) {
    o.status = "external";
    o.note = "receiver type " + type + " " + ext;
  } else {
    o.note = "receiver type " + type + " is not a project class";
  }
  return o;
}

Resolver::Outcome Resolver::resolve_chained(const std::string& receiver_text, const Outcome& receiver, const std::string& name,
                                            int arg_count) {
  Outcome o;
  o.status = "unresolved";
  if (receiver.status == "external") {
    o.status = "external";
    o.note = "receiver is returned by an external call";
    return o;
  }
  bool single = (receiver.status == "resolved" || receiver.status == "heuristic") && receiver.targets.size() == 1;
  if (!single) {
    o.note = "receiver is the result of a call that is " + receiver.status;
    return o;
  }
  auto target = symbol(receiver.targets[0]);
  if (!target) return o;
  int64_t cls = -1;
  std::string type;
  if (is_class_kind(target->kind)) {
    cls = target->id;  // instantiation without explicit constructor
  } else if (target->kind == "constructor") {
    cls = enclosing_class(target->id);
  } else {
    q_hint_.bind(1, target->id).bind(2, "#return");
    if (q_hint_.step()) type = q_hint_.text(0);
    q_hint_.reset();
    if (type.empty()) {
      o.note = "return type of " + target->qualified_name + " is not declared";
      return o;
    }
    std::string_view rt = receiver_text;
    while (!rt.empty() && (rt.front() == '(' || rt.front() == ' ')) rt.remove_prefix(1);
    if (starts_with(rt, "await") && starts_with(type, "Promise<") && type.back() == '>') {
      type = type.substr(8, type.size() - 9);  // await unwraps Promise<T>
    }
    std::string ext;
    int64_t scope = target->parent_id >= 0 ? target->parent_id : target->id;
    cls = resolve_type(target->file_id, scope, type, &ext);
    if (cls < 0) {
      if (!ext.empty()) {
        o.status = "external";
        o.note = target->qualified_name + " returns " + type + " " + ext;
      } else {
        o.note = target->qualified_name + " returns " + type + ", which is not a project class";
      }
      return o;
    }
  }
  Outcome r = method_lookup(cls, name, arg_count);
  if (receiver.status == "heuristic" && r.status == "resolved") {
    r.status = "heuristic";
    r.note = "receiver call was matched heuristically";
  }
  if (!type.empty() && r.status == "resolved" && r.note.empty()) r.note = "via return type " + type;
  return r;
}

Resolver::Outcome Resolver::resolve_call(int64_t scope, const std::string& kind, const std::string& name,
                                         const std::string& receiver, const std::string& receiver_kind, int arg_count) {
  if (receiver_kind == "config_class" || receiver_kind == "config_bean") {
    return resolve_call_config(name, receiver, receiver_kind);
  }
  if (receiver_kind == "typed") return resolve_typed_receiver(scope, receiver, name, arg_count);
  std::string fam = family(file_lang_);
  if (fam == "java") return resolve_call_java(scope, kind, name, receiver, receiver_kind, arg_count);
  if (fam == "c") return resolve_call_cpp(scope, kind, name, receiver, receiver_kind, arg_count);
  return resolve_call_dynamic(scope, kind, name, receiver, receiver_kind, arg_count);
}

Resolver::Outcome Resolver::resolve_call_dynamic(int64_t scope, const std::string& kind, const std::string& name,
                                                 const std::string& receiver, const std::string& rk, int args) {
  bool inst = kind == "instantiates";
  Outcome o;
  o.status = "unresolved";
  if (rk == "none") return value_call(resolve_name(file_id_, scope, name), args, inst);
  if (rk == "this" || rk == "super") {
    int64_t cls = enclosing_class(scope);
    if (cls < 0) {
      o.note = "no enclosing class";
      return o;
    }
    if (rk == "this") {
      o = method_lookup(cls, name, args);
      if (o.status == "unresolved") o.note = "method not found in the class or its resolved base classes";
      return o;
    }
    std::vector<int64_t> bases;
    q_bases_.bind(1, cls);
    while (q_bases_.step()) bases.push_back(q_bases_.int64(0));
    q_bases_.reset();
    for (auto b : bases) {
      Outcome r = name == "constructor" || name == "__init__" ? instantiate(b, args) : method_lookup(b, name, args);
      if (r.status != "unresolved") {
        r.kind = "calls";
        return r;
      }
    }
    if (bases.empty()) {
      auto& ext = cached(
          "SELECT note FROM edges WHERE src_symbol_id = ? AND kind IN ('extends','implements') AND status = 'external' LIMIT 1");
      ext.bind(1, cls);
      std::optional<std::string> base_note;
      if (ext.step()) base_note = ext.text(0);
      ext.reset();
      if (base_note) {
        o.status = "external";
        o.note = "base class: " + *base_note;
        return o;
      }
    }
    o.note = bases.empty() ? "base class is not part of the project" : "method not found in base classes";
    return o;
  }
  if (rk == "this_member") {
    int64_t cls = enclosing_class(scope);
    if (cls >= 0) {
      q_hint_.bind(1, cls).bind(2, receiver);
      std::optional<std::string> t;
      if (q_hint_.step()) t = q_hint_.text(0);
      q_hint_.reset();
      if (t && *t != "?") {
        std::string ext;
        int64_t c = resolve_type(file_id_, scope, *t, &ext);
        if (c >= 0) return method_lookup(c, name, args);
        if (!ext.empty()) {
          o.status = "external";
          o.note = "field type " + *t + " " + ext;
          return o;
        }
        o.note = starts_with(*t, "@call:") ? "field '" + receiver + "' holds the value returned by " + t->substr(6) +
                                                 ", whose type is not known"
                                           : "field type " + *t + " is not a project class";
        return o;
      }
    }
    Outcome h = heuristic(name, rk);
    if (h.status != "unresolved") return h;
    o.note = "field type is not known";
    return o;
  }
  if (rk == "identifier" || rk == "qualified") {
    if (rk == "identifier") {
      if (auto t = hint_in_scope_chain(scope, receiver)) {
        if (*t == "?") {
          o.note = "receiver '" + receiver + "' has no statically known type";
          return o;
        }
        std::string ext;
        int64_t c = resolve_type(file_id_, scope, *t, &ext);
        if (c >= 0) {
          Outcome r = method_lookup(c, name, args);
          if (r.status == "unresolved") r.note = "method not found on " + *t;
          return r;
        }
        if (!ext.empty()) {
          o.status = "external";
          o.note = "receiver type " + *t + " " + ext;
          return o;
        }
        o.note = starts_with(*t, "@call:")
                     ? "receiver '" + receiver + "' holds the value returned by " + t->substr(6) + ", whose type is not known"
                     : "receiver type " + *t + " is not a project class";
        return o;
      }
    }
    Value v = resolve_name(file_id_, scope, receiver);
    if (v.kind == Kind::None && rk == "qualified") {
      // a.b.c(): resolve the head, then walk object/namespace members.
      std::vector<std::string> parts;
      std::stringstream ss(receiver);
      for (std::string p; std::getline(ss, p, '.');) parts.push_back(p);
      if (!parts.empty()) {
        v = resolve_name(file_id_, scope, parts[0]);
        for (size_t i = 1; i < parts.size() && v.kind != Kind::None; ++i) {
          if (v.kind == Kind::Module) {
            v = export_value(v.id, parts[i]);
          } else if (v.kind == Kind::Symbol) {
            auto kids = children_named(v.id, parts[i]);
            if (kids.empty()) v = Value{};
            else v = Value{Kind::Symbol, kids[0].id};
          } else {
            break;
          }
        }
      }
    }
    if (v.kind == Kind::External) {
      o.status = "external";
      o.note = v.note;
      return o;
    }
    if (v.kind != Kind::None) {
      Outcome r = member_of(v, name, args);
      if (inst && r.status == "resolved" && !r.targets.empty()) {
        if (auto s = symbol(r.targets[0]); s && is_class_kind(s->kind)) return instantiate(s->id, args);
      }
      if (r.status != "unresolved") return r;
      if (v.kind == Kind::Unresolved) return r;
      o.note = r.note;
    }
    Outcome h = heuristic(name, rk);
    if (h.status != "unresolved") return h;
    if (o.note.empty()) o.note = v.kind == Kind::None ? "receiver is not defined in the project" : "member not found";
    return o;
  }
  if (rk == "module_require") {
    q_dep_by_spec_.bind(1, file_id_).bind(2, receiver);
    int64_t dst = -1;
    std::string status;
    if (q_dep_by_spec_.step()) {
      status = q_dep_by_spec_.text(0);
      dst = q_dep_by_spec_.is_null(1) ? -1 : q_dep_by_spec_.int64(1);
    }
    q_dep_by_spec_.reset();
    if (dst >= 0) return value_call(Value{Kind::Module, dst}, args, false);
    o.status = status == "external" ? "external" : "unresolved";
    o.note = "module " + receiver;
    return o;
  }
  o.note = "receiver is a dynamic expression";
  return o;
}

// Unqualified call that may come from `import static a.b.C.m;` or `import static a.b.C.*;`.
Resolver::Outcome Resolver::java_static_import(const std::string& name, int args, bool inherited_external) {
  Outcome o;
  o.status = "unresolved";
  std::vector<SymbolInfo> cands;
  std::string external_source;
  auto methods_in = [&](int64_t file) {
    std::vector<int64_t> ids;
    q_file_symbols_named_.bind(1, file).bind(2, name);
    while (q_file_symbols_named_.step()) ids.push_back(q_file_symbols_named_.int64(0));
    q_file_symbols_named_.reset();
    for (auto id : ids) {
      if (auto s = symbol(id); s && is_callable_kind(s->kind)) cands.push_back(*s);
    }
  };
  struct B {
    std::string status, spec;
    int64_t dst;
  };
  std::vector<B> binds;
  q_bindings_.bind(1, file_id_).bind(2, name);
  while (q_bindings_.step()) {
    binds.push_back({q_bindings_.text(2), q_bindings_.text(4), q_bindings_.is_null(3) ? -1 : q_bindings_.int64(3)});
  }
  q_bindings_.reset();
  for (const auto& b : binds) {
    if (b.dst >= 0) methods_in(b.dst);
    else if (b.status == "external") external_source = b.spec;
  }
  if (cands.empty() && external_source.empty()) {
    auto& st = cached(
        "SELECT status, IFNULL(dst_file_id, -1), specifier FROM file_deps WHERE src_file_id = ? AND kind = 'static_import' "
        "AND specifier LIKE '%.*' ORDER BY line");
    st.bind(1, file_id_);
    std::vector<B> wild;
    while (st.step()) wild.push_back({st.text(0), st.text(2), st.int64(1)});
    st.reset();
    for (const auto& w : wild) {
      if (w.dst >= 0) methods_in(w.dst);
      else if (w.status == "external" && external_source.empty()) external_source = w.spec;
    }
    if (cands.empty() && !external_source.empty()) {
      o.status = "external";
      o.note = inherited_external
                   ? "not declared in project code; inherited from an external base class or from static import " + external_source
                   : "not declared in the project class hierarchy; may come from static import " + external_source;
      return o;
    }
  }
  if (!cands.empty()) {
    Outcome p = pick(cands, args, "static import");
    return p;
  }
  if (!external_source.empty()) {
    o.status = "external";
    o.note = "static import " + external_source;
  }
  return o;
}

int64_t Resolver::java_type(int64_t file, const std::string& raw, std::string* note) {
  std::string t = clean_type(raw, false);
  if (t.empty()) return -1;
  auto class_like = [&](int64_t id) {
    auto s = symbol(id);
    return s && is_class_kind(s->kind);
  };
  if (t.find('.') != std::string::npos) {
    std::string simple = last_segment(t);
    std::vector<std::pair<int64_t, std::string>> rows;
    q_qualified_.bind(1, simple);
    while (q_qualified_.step()) rows.emplace_back(q_qualified_.int64(0), q_qualified_.text(1));
    q_qualified_.reset();
    for (auto& [id, qn] : rows) {
      if ((qn == t || ends_with(qn, "." + t)) && class_like(id)) return id;
    }
    // Outer.Inner where Outer is imported from outside the project (Map.Entry).
    std::string head = t.substr(0, t.find('.'));
    if (!head.empty() && std::isupper(static_cast<unsigned char>(head[0]))) {
      std::string head_note;
      if (java_type(file, head, &head_note) < 0 && !head_note.empty()) {
        if (note) *note = head_note;
        return -1;
      }
    }
    t = simple;
  }
  std::vector<int64_t> ids;
  q_file_symbols_named_.bind(1, file).bind(2, t);
  while (q_file_symbols_named_.step()) ids.push_back(q_file_symbols_named_.int64(0));
  q_file_symbols_named_.reset();
  for (auto id : ids) {
    if (class_like(id)) return id;
  }
  struct B {
    std::string status, spec;
    int64_t dst;
  };
  std::vector<B> binds;
  q_bindings_.bind(1, file).bind(2, t);
  while (q_bindings_.step()) {
    binds.push_back({q_bindings_.text(2), q_bindings_.text(4), q_bindings_.is_null(3) ? -1 : q_bindings_.int64(3)});
  }
  q_bindings_.reset();
  for (const auto& b : binds) {
    if (b.dst >= 0) {
      ids.clear();
      q_file_symbols_named_.bind(1, b.dst).bind(2, t);
      while (q_file_symbols_named_.step()) ids.push_back(q_file_symbols_named_.int64(0));
      q_file_symbols_named_.reset();
      for (auto id : ids) {
        if (class_like(id)) return id;
      }
    } else if (note) {
      *note = "imported from " + b.spec;
      return -1;
    }
  }
  std::string pkg;
  q_file_lang_.bind(1, file);
  if (q_file_lang_.step()) pkg = q_file_lang_.text(2);
  q_file_lang_.reset();
  std::vector<std::string> packages = {pkg};
  auto& wild = cached("SELECT specifier FROM file_deps WHERE src_file_id = ? AND status = 'package'");
  wild.bind(1, file);
  while (wild.step()) {
    std::string s = wild.text(0);
    packages.push_back(s.substr(0, s.size() - 2));
  }
  wild.reset();
  for (const auto& p : packages) {
    q_same_package_type_.bind(1, t).bind(2, p);
    int64_t found = -1;
    if (q_same_package_type_.step()) found = q_same_package_type_.int64(0);
    q_same_package_type_.reset();
    if (found >= 0) return found;
  }
  static const std::set<std::string, std::less<>> kJavaLang = {
      "String", "Object", "Integer", "Long", "Double", "Float", "Boolean", "Character", "Byte", "Short", "Math",
      "System", "Thread", "StringBuilder", "StringBuffer", "CharSequence", "Number", "Void", "Class", "ClassLoader",
      "Enum", "Record", "Iterable", "Comparable", "Runnable", "AutoCloseable", "Cloneable", "Process",
      "ProcessBuilder", "Runtime", "ThreadLocal", "Throwable", "Exception", "RuntimeException", "Error",
      "AssertionError", "IllegalArgumentException", "IllegalStateException", "UnsupportedOperationException",
      "NullPointerException", "IndexOutOfBoundsException", "ArrayIndexOutOfBoundsException", "ArithmeticException",
      "ClassCastException", "NumberFormatException", "InterruptedException", "CloneNotSupportedException",
      "SecurityException", "StackOverflowError", "OutOfMemoryError", "ReflectiveOperationException"};
  if (note && kJavaLang.count(t)) *note = "java.lang";
  return -1;
}

Resolver::Outcome Resolver::resolve_call_java(int64_t scope, const std::string& kind, const std::string& name,
                                              const std::string& receiver, const std::string& rk, int args) {
  Outcome o;
  o.status = "unresolved";
  if (kind == "instantiates") {
    std::string ext;
    int64_t cls = java_type(file_id_, name, &ext);
    if (cls >= 0) return instantiate(cls, args);
    if (!ext.empty()) {
      o.status = "external";
      o.note = ext;
    } else {
      o.note = "class not found in project";
    }
    return o;
  }
  if (rk == "none" || rk == "this") {
    Outcome inherited;
    for (int64_t cls = enclosing_class(scope); cls >= 0;) {
      Outcome r = method_lookup(cls, name, args);
      if (r.status == "external" && inherited.status.empty()) inherited = r;
      else if (r.status != "unresolved") return r;
      auto info = symbol(cls);
      cls = info ? enclosing_class(info->parent_id) : -1;  // outer class of a nested class
    }
    // Evidence order: project class hierarchy, an exact single static import,
    // then the remaining possibilities (external base class, static wildcard).
    if (rk == "none") {
      Outcome s = java_static_import(name, args, !inherited.status.empty());
      if (s.status != "unresolved" || !s.note.empty()) return s;
    }
    if (!inherited.status.empty()) return inherited;
    o.note = "method not found in the class hierarchy known to the index";
    return o;
  }
  if (rk == "super") {
    int64_t cls = enclosing_class(scope);
    std::vector<int64_t> bases;
    q_bases_.bind(1, cls);
    while (q_bases_.step()) bases.push_back(q_bases_.int64(0));
    q_bases_.reset();
    for (auto b : bases) {
      Outcome r = name == "constructor" ? instantiate(b, args) : method_lookup(b, name, args);
      if (r.status != "unresolved") {
        r.kind = "calls";
        return r;
      }
    }
    if (bases.empty()) {
      auto& ext = cached(
          "SELECT note FROM edges WHERE src_symbol_id = ? AND kind = 'extends' AND status = 'external' LIMIT 1");
      ext.bind(1, cls);
      std::optional<std::string> base_note;
      if (ext.step()) base_note = ext.text(0);
      ext.reset();
      if (base_note) {
        o.status = "external";
        o.note = "superclass: " + *base_note;
        return o;
      }
    }
    o.note = "superclass is not part of the project";
    return o;
  }
  std::optional<std::string> type;
  if (rk == "this_member") {
    int64_t cls = enclosing_class(scope);
    q_hint_.bind(1, cls).bind(2, receiver);
    if (q_hint_.step()) type = q_hint_.text(0);
    q_hint_.reset();
  } else if (rk == "identifier") {
    type = hint_in_scope_chain(scope, receiver);
  }
  std::string ext;
  if (type && *type == "?") {
    o.note = "receiver '" + receiver + "' has no statically known type";
    return o;
  }
  if (type) {
    int64_t cls = starts_with(*type, "@call:") ? resolve_type(file_id_, scope, *type, &ext) : java_type(file_id_, *type, &ext);
    if (cls >= 0) return method_lookup(cls, name, args);
    if (!ext.empty()) {
      o.status = "external";
      o.note = "receiver type " + *type + " (" + ext + ")";
      return o;
    }
    o.note = starts_with(*type, "@call:")
                 ? "receiver '" + receiver + "' holds the value returned by " + type->substr(6) + ", whose type is not known"
                 : "receiver type " + *type + " is not in the project";
    return o;
  }
  if (rk == "identifier" || rk == "qualified") {
    std::string head = receiver.substr(0, receiver.find('.'));
    int64_t cls = java_type(file_id_, rk == "identifier" ? receiver : head, &ext);
    if (cls >= 0 && rk == "identifier") return method_lookup(cls, name, args);  // static call
    if (!ext.empty()) {
      o.status = "external";
      o.note = ext;
      return o;
    }
  }
  Outcome h = heuristic(name, rk);
  if (h.status != "unresolved") return h;
  o.note = "receiver type is not known";
  return o;
}

int64_t Resolver::cpp_type(int64_t scope, const std::string& raw) {
  std::string t = clean_type(raw, true);
  if (t.empty()) return -1;
  std::string simple = last_segment(t);
  std::vector<std::pair<int64_t, std::string>> rows;
  q_qualified_.bind(1, simple);
  while (q_qualified_.step()) rows.emplace_back(q_qualified_.int64(0), q_qualified_.text(1));
  q_qualified_.reset();
  std::vector<SymbolInfo> cands;
  for (auto& [id, qn] : rows) {
    auto s = symbol(id);
    if (!s || !is_class_kind(s->kind)) continue;
    if (t.find("::") != std::string::npos && qn != t && !ends_with(qn, "::" + t)) continue;
    cands.push_back(*s);
  }
  if (cands.empty()) return -1;
  if (cands.size() == 1) return cands[0].id;
  for (const auto& c : cands) {
    if (visible_files_.count(c.file_id)) return c.id;
  }
  (void)scope;
  return -1;
}

Resolver::Outcome Resolver::resolve_call_cpp(int64_t scope, const std::string& kind, const std::string& name,
                                             const std::string& receiver, const std::string& rk, int args) {
  Outcome o;
  o.status = "unresolved";
  if (kind == "instantiates") {
    int64_t cls = cpp_type(scope, rk == "qualified" ? receiver + "::" + name : name);
    if (cls >= 0) return instantiate(cls, args);
    o.note = "type not found in project";
    return o;
  }
  if (rk == "qualified" && (receiver == "std" || starts_with(receiver, "std::"))) {
    o.status = "external";
    o.note = "C++ standard library";
    return o;
  }
  auto functions = [&](const std::string& qualified_suffix) {
    std::vector<std::pair<int64_t, std::string>> rows;
    q_qualified_.bind(1, name);
    while (q_qualified_.step()) rows.emplace_back(q_qualified_.int64(0), q_qualified_.text(1));
    q_qualified_.reset();
    std::vector<SymbolInfo> cands;
    for (auto& [id, qn] : rows) {
      auto s = symbol(id);
      if (!s || !is_callable_kind(s->kind) || family(s->language) != "c") continue;
      if (!qualified_suffix.empty() && qn != qualified_suffix && !ends_with(qn, "::" + qualified_suffix)) continue;
      if (qualified_suffix.empty() && s->kind != "function") continue;
      cands.push_back(*s);
    }
    return cands;
  };
  auto finish = [&](std::vector<SymbolInfo> cands) {
    Outcome r = pick(cands, args, "");
    if (r.status == "resolved") {
      auto s = symbol(r.targets[0]);
      bool visible = s && visible_files_.count(s->file_id);
      if (!visible && s) {
        // A declaration in a visible header makes the definition visible.
        for (const auto& c : cands) {
          if (c.qualified_name == s->qualified_name && visible_files_.count(c.file_id)) visible = true;
        }
      }
      if (!visible) {
        r.status = "heuristic";
        r.note = "matched by name; no declaration found in the include closure";
      }
    }
    return r;
  };
  if (rk == "none" || rk == "this") {
    int64_t cls = enclosing_class(scope);
    if (cls >= 0) {
      Outcome r = method_lookup(cls, name, args);
      if (r.status != "unresolved" || rk == "this") return r;
    }
    auto cands = functions("");
    if (cands.empty()) {
      o.note = "function not defined in project";
      return o;
    }
    return finish(cands);
  }
  if (rk == "qualified") {
    int64_t cls = cpp_type(scope, receiver);
    if (cls >= 0) {
      Outcome r = method_lookup(cls, name, args);
      if (r.status != "unresolved") return r;
    }
    auto cands = functions(receiver + "::" + name);
    if (!cands.empty()) return finish(cands);
    o.note = "qualified name not found in project";
    return o;
  }
  std::optional<std::string> type;
  if (rk == "this_member") {
    int64_t cls = enclosing_class(scope);
    q_hint_.bind(1, cls).bind(2, receiver);
    if (q_hint_.step()) type = q_hint_.text(0);
    q_hint_.reset();
  } else if (rk == "identifier") {
    type = hint_in_scope_chain(scope, receiver);
  }
  if (type) {
    std::string ext;
    int64_t cls = starts_with(*type, "@call:") ? resolve_type(file_id_, scope, *type, &ext) : cpp_type(scope, *type);
    if (cls < 0 && !ext.empty()) {
      o.status = "external";
      o.note = "receiver " + receiver + " holds a value returned by an external call";
      return o;
    }
    if (cls >= 0) return method_lookup(cls, name, args);
    o.note = "receiver type " + *type + " is not in the project";
    if (starts_with(clean_type(*type, false), "std::")) {
      o.status = "external";
      o.note = "C++ standard library type " + *type;
    }
    return o;
  }
  Outcome h = heuristic(name, rk);
  if (h.status != "unresolved") return h;
  o.note = "receiver type is not known";
  return o;
}

void Resolver::resolve_inheritance() {
  auto sel = db_.prepare(
      "SELECT e.id, e.src_symbol_id, e.file_id, e.target_name, e.receiver FROM edges e "
      "WHERE e.kind IN ('extends','implements') AND e.status = 'pending' AND e.id > ? ORDER BY e.id LIMIT ?");
  auto upd = db_.prepare("UPDATE edges SET dst_symbol_id = ?, status = ?, note = ? WHERE id = ?");
  int64_t last = 0;
  while (true) {
    struct Row {
      int64_t id, src, file;
      std::string name, qualifier;
    };
    std::vector<Row> rows;
    sel.bind(1, last).bind(2, kPage);
    while (sel.step()) rows.push_back({sel.int64(0), sel.int64(1), sel.int64(2), sel.text(3), sel.text(4)});
    sel.reset();
    if (rows.empty()) break;
    for (const auto& r : rows) {
      last = r.id;
      set_file_context(r.file);
      auto src = symbol(r.src);
      int64_t scope = src ? src->parent_id : module_symbol(r.file);
      std::string type = r.qualifier.empty() ? r.name : r.qualifier + (family(file_lang_) == "c" ? "::" : ".") + r.name;
      std::string note;
      int64_t cls = resolve_type(r.file, scope < 0 ? module_symbol(r.file) : scope, type, &note);
      if (cls == r.src) cls = -1;
      static const std::set<std::string, std::less<>> kBuiltinBases = {
          // JavaScript / TypeScript
          "Error", "TypeError", "RangeError", "SyntaxError", "Array", "Map", "Set", "Promise", "Object", "EventTarget",
          // Python
          "Exception", "BaseException", "ValueError", "KeyError", "TypeError", "RuntimeError", "LookupError",
          "IndexError", "AttributeError", "PermissionError", "object", "dict", "list", "str", "int", "Enum"};
      if (cls < 0 && note.empty() && (family(file_lang_) == "js" || family(file_lang_) == "python") &&
          kBuiltinBases.count(r.name)) {
        note = "language built-in";
      }
      if (cls >= 0) {
        ++stats_.inherits_resolved;
        upd.bind(1, cls).bind(2, "resolved").bind(3, nullptr).bind(4, r.id).run();
      } else {
        ++stats_.inherits_unresolved;
        upd.bind(1, nullptr).bind(2, note.empty() ? "unresolved" : "external");
        upd.bind(3, note.empty() ? "base type not found in project" : note).bind(4, r.id).run();
      }
    }
  }
}

void Resolver::resolve_calls() {
  auto files = db_.prepare(
      "SELECT DISTINCT file_id FROM edges WHERE status = 'pending' AND kind IN ('calls','instantiates','configures') "
      "AND file_id > ? ORDER BY file_id LIMIT 64");
  auto sel = db_.prepare(
      "SELECT id, kind, src_symbol_id, target_name, receiver, receiver_kind, arg_count, IFNULL(receiver_edge_id, -1), "
      "IFNULL(arg_types, '') FROM edges WHERE file_id = ? AND status = 'pending' AND kind IN ('calls','instantiates','configures') ORDER BY id");
  auto existing = db_.prepare("SELECT status, IFNULL(dst_symbol_id, -1) FROM edges WHERE id = ?");
  auto upd = db_.prepare("UPDATE edges SET dst_symbol_id = ?, status = ?, note = ?, kind = ? WHERE id = ?");
  auto cand = db_.prepare("INSERT OR IGNORE INTO edge_candidates(edge_id, symbol_id) VALUES (?, ?)");
  int64_t last_file = 0;
  while (true) {
    std::vector<int64_t> batch;
    files.bind(1, last_file);
    while (files.step()) batch.push_back(files.int64(0));
    files.reset();
    if (batch.empty()) break;
    db::Transaction tx(db_);
    for (int64_t file : batch) {
      last_file = file;
      set_file_context(file);
      struct Row {
        int64_t id;
        std::string kind;
        int64_t src;
        std::string name, receiver, rk;
        int args;
        int64_t receiver_edge;
        std::string arg_types;
      };
      std::vector<Row> rows;
      sel.bind(1, file);
      while (sel.step()) {
        rows.push_back({sel.int64(0), sel.text(1), sel.int64(2), sel.text(3), sel.text(4), sel.text(5),
                        sel.is_null(6) ? -1 : static_cast<int>(sel.int64(6)), sel.int64(7), sel.text(8)});
      }
      sel.reset();
      std::unordered_map<int64_t, size_t> by_id;
      for (size_t i = 0; i < rows.size(); ++i) by_id[rows[i].id] = i;
      std::vector<std::optional<Outcome>> done(rows.size());
      std::vector<char> active(rows.size(), 0);
      // Chained calls need their receiver call resolved first (same file).
      std::function<Outcome(size_t, int)> resolve_row = [&](size_t i, int depth) -> Outcome {
        if (done[i]) return *done[i];
        const Row& r = rows[i];
        Outcome o;
        if (active[i] || depth > 32) {
          o.status = "unresolved";
          o.note = "receiver chain is too deep";
          return o;
        }
        active[i] = 1;
        // Argument types for overload selection, restored after nested receiver resolution.
        auto with_args = [&](auto&& fn) {
          CallContext saved = std::move(call_ctx_);
          call_ctx_ = CallContext{r.src, r.args, {}};
          if (!r.arg_types.empty()) {
            std::string cur;
            for (char ch : r.arg_types) {
              if (ch == ',') {
                call_ctx_.arg_types.push_back(cur);
                cur.clear();
              } else {
                cur += ch;
              }
            }
            call_ctx_.arg_types.push_back(cur);
          }
          Outcome out = fn();
          call_ctx_ = std::move(saved);
          return out;
        };
        if (r.rk == "call") {
          Outcome recv;
          recv.status = "unresolved";
          if (auto it = by_id.find(r.receiver_edge); it != by_id.end()) {
            recv = resolve_row(it->second, depth + 1);
          } else if (r.receiver_edge >= 0) {
            existing.bind(1, r.receiver_edge);
            if (existing.step()) {
              recv.status = existing.text(0);
              if (existing.int64(1) >= 0) recv.targets = {existing.int64(1)};
            }
            existing.reset();
          }
          o = with_args([&] { return resolve_chained(r.receiver, recv, r.name, r.args); });
          if (r.kind == "instantiates") o.kind = "instantiates";
        } else {
          o = with_args([&] { return resolve_call(r.src, r.kind, r.name, r.receiver, r.rk, r.args); });
        }
        active[i] = 0;
        done[i] = o;
        return o;
      };
      for (size_t i = 0; i < rows.size(); ++i) {
        const Row& r = rows[i];
        Outcome o = resolve_row(i, 0);
        if (r.kind != "configures") {
          ++stats_.calls_total;
          if (o.status == "resolved") ++stats_.calls_resolved;
          else if (o.status == "ambiguous") ++stats_.calls_ambiguous;
          else if (o.status == "heuristic") ++stats_.calls_heuristic;
          else if (o.status == "external") ++stats_.calls_external;
          else ++stats_.calls_unresolved;
        }
        bool single = (o.status == "resolved" || o.status == "heuristic") && o.targets.size() == 1;
        if (single) upd.bind(1, o.targets[0]); else upd.bind(1, nullptr);
        upd.bind(2, o.status).bind_text_or_null(3, o.note).bind(4, o.kind.empty() ? r.kind : o.kind).bind(5, r.id).run();
        if (o.status == "ambiguous") {
          for (auto t : o.targets) cand.bind(1, r.id).bind(2, t).run();
        }
      }
    }
    tx.commit();
  }
}

void Resolver::add_override_edges() {
  auto sel = db_.prepare(
      "SELECT id, src_symbol_id, dst_symbol_id FROM edges WHERE kind IN ('extends','implements') "
      "AND status = 'resolved' AND id > ? ORDER BY id LIMIT ?");
  auto methods = db_.prepare("SELECT id, name, param_count FROM symbols WHERE parent_id = ? AND kind = 'method'");
  auto ins = db_.prepare(
      "INSERT INTO edges(kind, raw_kind, generated, src_symbol_id, dst_symbol_id, file_id, line, col, status, target_name, "
      "note) SELECT 'overridden_by', 'overridden_by', 1, ?, s.id, s.file_id, s.start_line, s.start_col, 'resolved', s.name, "
      "'possible dynamic dispatch to an override' FROM symbols s WHERE s.id = ?");
  int64_t last = 0;
  while (true) {
    std::vector<std::tuple<int64_t, int64_t, int64_t>> rows;
    sel.bind(1, last).bind(2, kPage);
    while (sel.step()) rows.emplace_back(sel.int64(0), sel.int64(1), sel.int64(2));
    sel.reset();
    if (rows.empty()) break;
    for (auto [id, sub, base] : rows) {
      last = id;
      std::vector<std::tuple<int64_t, std::string, int>> base_methods;
      methods.bind(1, base);
      while (methods.step()) {
        base_methods.emplace_back(methods.int64(0), methods.text(1), methods.is_null(2) ? -1 : static_cast<int>(methods.int64(2)));
      }
      methods.reset();
      for (auto& [bm, name, params] : base_methods) {
        Outcome o = method_lookup(sub, name, params);
        if (o.status != "resolved" || o.targets.empty() || o.targets[0] == bm) continue;
        auto target = symbol(o.targets[0]);
        if (!target || target->parent_id == base) continue;
        // Source the edge at the base method's definition when one exists out of line.
        int64_t from = bm;
        Outcome def = method_lookup(base, name, params);
        if (def.status == "resolved" && !def.targets.empty()) from = def.targets[0];
        ins.bind(1, from).bind(2, o.targets[0]).run();
        ++stats_.override_edges;
      }
    }
  }
}

// ===========================================================================
// Routes, mounts, middleware
// ===========================================================================

namespace {
struct RefResult {
  std::string status;  // resolved|ambiguous|heuristic|unresolved|external
  int64_t symbol = -1;
  std::vector<int64_t> candidates;
  std::string note;
};
}  // namespace

void Resolver::resolve_mounts() {
  auto sel = db_.prepare(
      "SELECT id, file_id, scope_symbol_id, target_kind, target_name, target_receiver, target_text, target_symbol_id, "
      "line, col, raw_note FROM mounts WHERE status = 'pending' AND id > ? ORDER BY id LIMIT ?");
  auto upd = db_.prepare(
      "UPDATE mounts SET status = ?, target_router_id = ?, target_symbol_id = ?, note = ? WHERE id = ?");
  auto edge = db_.prepare(
      "INSERT INTO edges(kind, raw_kind, generated, src_symbol_id, dst_symbol_id, file_id, line, col, status, target_name, "
      "target_text, note) VALUES (?1,?1,1,?2,?3,?4,?5,?6,?7,?8,?9,?10)");
  auto router_in = [&](int64_t file, const std::string& name) -> int64_t {
    q_router_.bind(1, file).bind(2, name);
    int64_t id = -1;
    if (q_router_.step()) id = q_router_.int64(0);
    q_router_.reset();
    return id;
  };
  // A value that refers to a router variable -> router id.
  auto router_of_value = [&](const Value& v) -> int64_t {
    if (v.kind == Kind::Symbol) {
      auto s = symbol(v.id);
      if (s && s->kind == "variable") return router_in(s->file_id, s->name);
    }
    if (v.kind == Kind::Module) {
      Value d = export_value(v.id, "default");
      if (d.kind == Kind::Symbol) {
        auto s = symbol(d.id);
        if (s && s->kind == "variable") return router_in(s->file_id, s->name);
      }
    }
    return -1;
  };
  int64_t last = 0;
  while (true) {
    struct Row {
      int64_t id, file, scope;
      std::string kind, name, receiver, text;
      int64_t target_symbol, line, col;
      std::string note;
    };
    std::vector<Row> rows;
    sel.bind(1, last).bind(2, kPage);
    while (sel.step()) {
      rows.push_back({sel.int64(0), sel.int64(1), sel.is_null(2) ? -1 : sel.int64(2), sel.text(3), sel.text(4),
                      sel.text(5), sel.text(6), sel.is_null(7) ? -1 : sel.int64(7), sel.int64(8), sel.int64(9), sel.text(10)});
    }
    sel.reset();
    if (rows.empty()) break;
    for (const auto& r : rows) {
      last = r.id;
      set_file_context(r.file);
      int64_t scope = r.scope >= 0 ? r.scope : module_symbol(r.file);
      int64_t router = -1;
      int64_t sym = r.target_symbol;
      std::string status = "unresolved";
      std::string note = r.note;
      if (r.kind == "module") {
        // Django include(): the routes of the included URLconf module get this prefix.
        int64_t target_file = -1;
        auto& q = cached(
            "SELECT id FROM files WHERE module_name = ?1 OR module_name LIKE '%.' || ?1 ORDER BY length(module_name), "
            "path LIMIT 1");
        q.bind(1, r.name);
        if (q.step()) target_file = q.int64(0);
        q.reset();
        if (target_file < 0 && r.name.find('.') == std::string::npos) {
          Value v = resolve_name(r.file, scope, r.name);
          if (v.kind == Kind::Module) target_file = v.id;
        }
        int64_t dst = target_file >= 0 ? module_symbol(target_file) : -1;
        note = dst >= 0 ? "Django URLconf included here" : "URLconf module " + r.name + " not found in the project";
        upd.bind(1, dst >= 0 ? "urlconf" : "unresolved").bind(2, nullptr).bind_id_or_null(3, dst);
        upd.bind_text_or_null(4, note).bind(5, r.id).run();
        edge.bind(1, "mounts").bind(2, scope).bind_id_or_null(3, dst).bind(4, r.file).bind(5, r.line).bind(6, r.col);
        edge.bind(7, dst >= 0 ? "resolved" : "unresolved").bind(8, r.name).bind(9, r.text).bind(10, note).run();
        dst >= 0 ? ++stats_.mounts_router : ++stats_.mounts_unresolved;
        continue;
      }
      if (r.kind == "inline") {
        status = "middleware";
      } else if (r.kind == "identifier") {
        router = router_in(r.file, r.name);
        if (router < 0) {
          Value v = resolve_name(r.file, scope, r.name);
          router = router_of_value(v);
          if (router < 0) {
            Outcome o = value_call(v, -1, false);
            if (o.status == "resolved" && !o.targets.empty()) {
              sym = o.targets[0];
              status = "middleware";
            } else if (o.status == "external") {
              status = "middleware";
              note = o.note;
            } else {
              note = o.note.empty() ? "target is not a recognized router or function" : o.note;
            }
          }
        }
      } else if (r.kind == "member") {
        Value v = resolve_name(r.file, scope, r.receiver);
        Outcome o = member_of(v, r.name, -1);
        if (o.status == "resolved" && !o.targets.empty()) {
          router = router_of_value(Value{Kind::Symbol, o.targets[0]});
          if (router < 0) {
            sym = o.targets[0];
            status = "middleware";
          }
        } else if (o.status == "external" || v.kind == Kind::External) {
          status = "middleware";
          note = o.note.empty() ? v.note : o.note;
        } else {
          note = o.note;
        }
      } else if (r.kind == "require") {
        q_dep_by_spec_.bind(1, r.file).bind(2, r.name);
        int64_t dst = -1;
        if (q_dep_by_spec_.step() && !q_dep_by_spec_.is_null(1)) dst = q_dep_by_spec_.int64(1);
        q_dep_by_spec_.reset();
        if (dst >= 0) {
          router = router_of_value(Value{Kind::Module, dst});
          if (router < 0) {
            Outcome o = value_call(Value{Kind::Module, dst}, -1, false);
            if (o.status == "resolved" && !o.targets.empty()) {
              sym = o.targets[0];
              status = "middleware";
            }
          }
        } else {
          note = "module " + r.name + " is not a project file";
          status = "middleware";
        }
      } else {
        status = "middleware";
        note = "middleware expression (not statically resolved)";
      }
      if (router >= 0) status = "router";
      if (status == "router") ++stats_.mounts_router;
      else if (status == "middleware") ++stats_.mounts_middleware;
      else ++stats_.mounts_unresolved;
      upd.bind(1, status).bind_id_or_null(2, router).bind_id_or_null(3, sym).bind_text_or_null(4, note).bind(5, r.id).run();
      if (status == "router") {
        auto& rs = cached("SELECT file_id FROM routers WHERE id = ?");
        rs.bind(1, router);
        int64_t router_file = rs.step() ? rs.int64(0) : -1;
        rs.reset();
        int64_t dst = router_file >= 0 ? module_symbol(router_file) : -1;
        edge.bind(1, "mounts").bind(2, scope).bind_id_or_null(3, dst).bind(4, r.file).bind(5, r.line).bind(6, r.col);
        edge.bind(7, dst >= 0 ? "resolved" : "unresolved").bind(8, r.name).bind(9, r.text);
        edge.bind(10, "router mounted here; its routes are registered in the target module").run();
      } else if (status == "middleware" && (sym >= 0 || r.kind == "identifier" || r.kind == "member")) {
        edge.bind(1, "uses_middleware").bind(2, scope).bind_id_or_null(3, sym).bind(4, r.file).bind(5, r.line).bind(6, r.col);
        edge.bind(7, sym >= 0 ? "resolved" : (note.find("import") != std::string::npos || note.find("external") != std::string::npos ? "external" : "unresolved"));
        edge.bind(8, r.name).bind(9, r.text).bind_text_or_null(10, note).run();
      }
    }
  }
}

Resolver::Outcome Resolver::resolve_handler_ref(int64_t file, int64_t scope, const std::string& kind,
                                                const std::string& name, const std::string& receiver, int64_t* cls) {
  *cls = -1;
  Outcome o;
  o.status = "unresolved";
  auto class_target = [&](int64_t id) {
    auto s = symbol(id);
    if (!s) return -1L;
    if (is_class_kind(s->kind)) return static_cast<long>(id);
    if (s->kind == "constructor") {
      auto parent = symbol(s->parent_id);
      if (parent && is_class_kind(parent->kind)) return static_cast<long>(parent->id);
    }
    return -1L;
  };
  std::string fam = family(language_of_file(file));
  if (fam == "c" && (kind == "identifier" || kind == "member")) {
    // C/C++: a function, Class::method, ns::function, or obj.method on a typed variable.
    if (kind == "identifier") {
      o = resolve_call(scope, "calls", name, "", "none", -1);
    } else {
      bool qualified = receiver.find("::") != std::string::npos || cpp_type(scope, receiver) >= 0;
      o = resolve_call(scope, "calls", name, receiver, qualified ? "qualified" : "identifier", -1);
    }
    if ((o.status == "resolved" || o.status == "heuristic") && o.targets.size() == 1) {
      auto s = symbol(o.targets[0]);
      if (s && is_class_kind(s->kind)) *cls = s->id;
    }
    return o;
  }
  bool java = fam == "java";
  if (java && (kind == "identifier" || kind == "member")) {
    // Java: a handler variable (or obj::method) is typed by its declaration.
    std::string var = kind == "identifier" ? name : receiver;
    auto type = hint_in_scope_chain(scope, var);
    if (!type || *type == "?") {
      o.note = "the type of '" + var + "' is not known";
      return o;
    }
    Outcome c = resolve_class_ref(file, scope, *type);
    if (c.status != "resolved") return c;
    if (kind == "member") return method_lookup(c.targets[0], name, -1);
    *cls = c.targets[0];
    return c;
  }
  if (kind == "identifier") {
    Value v = resolve_name(file, scope, name);
    if (v.kind == Kind::Instance) {
      // A handler object: route requests to its class.
      int64_t c = resolve_type(v.context_file >= 0 ? v.context_file : file, v.context_scope >= 0 ? v.context_scope : scope,
                               v.type);
      if (c >= 0) {
        o.status = "resolved";
        o.targets = {c};
      } else {
        o.note = "handler object of type " + v.type + ", which is not a project class";
      }
    } else if (v.kind == Kind::Symbol && symbol(v.id) && is_class_kind(symbol(v.id)->kind)) {
      o.status = "resolved";
      o.targets = {v.id};
    } else {
      o = value_call(v, -1, false);
    }
  } else if (kind == "member") {
    if (starts_with(receiver, "this.")) {
      o = resolve_call(scope, "calls", name, receiver.substr(5), "this_member", -1);
    } else {
      o = member_of(resolve_name(file, scope, receiver), name, -1);
    }
  } else if (kind == "require") {
    q_dep_by_spec_.bind(1, file).bind(2, name);
    int64_t dst = -1;
    if (q_dep_by_spec_.step() && !q_dep_by_spec_.is_null(1)) dst = q_dep_by_spec_.int64(1);
    q_dep_by_spec_.reset();
    if (dst >= 0) o = value_call(Value{Kind::Module, dst}, -1, false);
  } else if (kind == "class") {
    o = resolve_class_ref(file, scope, name);
  } else if (kind == "qualified") {
    o = config_member(name, receiver);
  } else if (kind == "name") {
    o = config_function_name(name);
  } else if (kind == "file") {
    o = config_file(name, receiver);
  } else if (kind == "bean") {
    Outcome b = config_bean(name);
    if (b.status != "resolved") return b;
    auto bean = symbol(b.targets[0]);
    if (!bean || bean->container.empty()) {
      o.note = "bean '" + name + "' declares no class";
      return o;
    }
    o = config_class(bean->container);
  } else if (kind == "method_ref") {
    // Java method references: Type::method, this::method
    int64_t owner = -1;
    if (receiver == "this" || receiver == "super") {
      owner = enclosing_class(scope);
    } else {
      Outcome c = resolve_class_ref(file, scope, receiver);
      if (c.status == "external") return c;
      if (c.status == "resolved") owner = c.targets[0];
    }
    if (owner < 0) {
      o.note = "type " + receiver + " is not a project class";
      return o;
    }
    o = method_lookup(owner, name, -1);
    return o;
  } else {
    o.note = "handler expression is not statically resolvable";
    return o;
  }
  if ((o.status == "resolved" || o.status == "heuristic") && o.targets.size() == 1) {
    int64_t c = class_target(o.targets[0]);
    if (c >= 0) *cls = c;
  }
  return o;
}

void Resolver::resolve_route_handlers() {
  auto sel = db_.prepare(
      "SELECT id, file_id, scope_symbol_id, handler_kind, handler_name, handler_receiver, raw_handler_note, framework, "
      "method FROM routes WHERE handler_status = 'pending' AND id > ? ORDER BY id LIMIT ?");
  auto upd = db_.prepare(
      "UPDATE routes SET handler_symbol_id = ?, handler_status = ?, handler_note = ?, method = ? WHERE id = ?");
  // A handler class serving several HTTP methods becomes one route per method.
  auto copy = db_.prepare(
      "INSERT INTO routes(framework, method, path, full_path, mount_prefix, mount_status, mount_id, file_id, line, col, "
      "registrar, router_id, scope_symbol_id, handler_kind, handler_name, handler_receiver, handler_text, "
      "handler_symbol_id, handler_status, handler_note, confidence, declared_prefix, raw_handler_note, origin_route_id, "
      "raw_method) SELECT framework, ?1, path, full_path, mount_prefix, mount_status, mount_id, file_id, line, col, "
      "registrar, router_id, scope_symbol_id, handler_kind, handler_name, handler_receiver, handler_text, ?2, ?3, ?4, "
      "confidence, declared_prefix, raw_handler_note, ?5, raw_method FROM routes WHERE id = ?5");
  auto copy_mw = db_.prepare(
      "INSERT INTO route_middleware(route_id, position, scope, ref_kind, name, receiver, text, symbol_id, status, "
      "file_id, line) SELECT ?, position, scope, ref_kind, name, receiver, text, symbol_id, status, file_id, line "
      "FROM route_middleware WHERE route_id = ? ORDER BY position, id");
  int64_t last = 0;
  while (true) {
    struct Row {
      int64_t id, file, scope;
      std::string kind, name, receiver, note, framework, method;
    };
    std::vector<Row> rows;
    sel.bind(1, last).bind(2, kPage);
    while (sel.step()) {
      rows.push_back({sel.int64(0), sel.int64(1), sel.is_null(2) ? -1 : sel.int64(2), sel.text(3), sel.text(4),
                      sel.text(5), sel.text(6), sel.text(7), sel.text(8)});
    }
    sel.reset();
    if (rows.empty()) break;
    for (const auto& r : rows) {
      last = r.id;
      set_file_context(r.file);
      int64_t scope = r.scope >= 0 ? r.scope : module_symbol(r.file);
      int64_t cls = -1;
      Outcome o = resolve_handler_ref(r.file, scope, r.kind, r.name, r.receiver, &cls);
      auto with = [](const std::string& a, const std::string& b) { return a.empty() ? b : (b.empty() ? a : a + "; " + b); };
      std::string note = with(r.note, o.note);
      std::vector<DispatchTarget> targets;
      if (cls >= 0) {
        std::string dnote;
        targets = dispatch_targets(cls, r.framework, r.method, r.kind == "class" ? r.receiver : "", &dnote);
        if (targets.empty()) {
          o.status = "unresolved";
          o.targets.clear();
          note = with(note, dnote);
        } else {
          o.targets = {targets[0].symbol};
        }
      }
      bool single = (o.status == "resolved" || o.status == "heuristic") && o.targets.size() == 1;
      if (single) {
        ++stats_.route_handlers_resolved;
        upd.bind(1, o.targets[0]);
      } else {
        ++stats_.route_handlers_unresolved;
        upd.bind(1, nullptr);
      }
      std::string method = targets.empty() ? r.method : targets[0].method;
      upd.bind(2, o.status).bind_text_or_null(3, targets.empty() ? note : with(note, targets[0].note));
      upd.bind(4, method).bind(5, r.id).run();
      for (size_t i = 1; i < targets.size(); ++i) {
        copy.bind(1, targets[i].method).bind(2, targets[i].symbol).bind(3, o.status);
        copy.bind_text_or_null(4, with(note, targets[i].note)).bind(5, r.id).run();
        copy_mw.bind(1, db_.last_insert_rowid()).bind(2, r.id).run();
        ++stats_.route_handlers_resolved;
      }
    }
  }
  // Registration edges: the code that registers a route -> its handler. Copies
  // made for other mount prefixes share the handler; copies for other HTTP
  // methods of a handler class do not.
  db_.exec(
      "INSERT INTO edges(kind, raw_kind, generated, src_symbol_id, dst_symbol_id, file_id, line, col, status, target_name, "
      "target_text, note) SELECT 'registers_route', 'registers_route', 1, COALESCE(scope_symbol_id, (SELECT MIN(id) FROM symbols WHERE file_id = routes.file_id)), "
      "handler_symbol_id, file_id, line, col, handler_status, handler_name, COALESCE(handler_text, handler_name), "
      "method || ' ' || path FROM routes WHERE origin_route_id IS NULL OR method != "
      "(SELECT o.method FROM routes o WHERE o.id = routes.origin_route_id)");
  stats_.routes = db_.query_int("SELECT COUNT(*) FROM routes").value_or(0);
}

void Resolver::resolve_route_middleware() {
  auto sel = db_.prepare(
      "SELECT m.id, m.file_id, r.scope_symbol_id, m.ref_kind, m.name, m.receiver FROM route_middleware m "
      "JOIN routes r ON r.id = m.route_id WHERE m.status = 'pending' AND m.id > ? ORDER BY m.id LIMIT ?");
  auto upd = db_.prepare("UPDATE route_middleware SET symbol_id = ?, status = ? WHERE id = ?");
  int64_t last = 0;
  while (true) {
    struct Row {
      int64_t id, file, scope;
      std::string kind, name, receiver;
    };
    std::vector<Row> rows;
    sel.bind(1, last).bind(2, kPage);
    while (sel.step()) {
      rows.push_back({sel.int64(0), sel.int64(1), sel.is_null(2) ? -1 : sel.int64(2), sel.text(3), sel.text(4), sel.text(5)});
    }
    sel.reset();
    if (rows.empty()) break;
    for (const auto& r : rows) {
      last = r.id;
      set_file_context(r.file);
      int64_t scope = r.scope >= 0 ? r.scope : module_symbol(r.file);
      Outcome o;
      o.status = "unresolved";
      if (r.kind == "identifier") {
        o = value_call(resolve_name(r.file, scope, r.name), -1, false);
      } else if (r.kind == "member") {
        o = member_of(resolve_name(r.file, scope, r.receiver), r.name, -1);
      } else if (r.kind == "class") {
        // A filter class (web.xml): its request method (doFilter), else the class.
        o = resolve_class_ref(r.file, scope, r.name);
        if (o.status == "resolved" && o.targets.size() == 1 && !r.receiver.empty()) {
          Outcome m = method_lookup(o.targets[0], r.receiver, -1);
          if ((m.status == "resolved" || m.status == "ambiguous") && !m.targets.empty()) o.targets = {m.targets[0]};
        }
      }
      bool single = (o.status == "resolved" || o.status == "heuristic") && o.targets.size() == 1;
      upd.bind_id_or_null(1, single ? o.targets[0] : -1).bind(2, o.status).bind(3, r.id).run();
    }
  }
  db_.exec(
      "INSERT INTO edges(kind, raw_kind, generated, src_symbol_id, dst_symbol_id, file_id, line, col, status, target_name, "
      "target_text, note) SELECT 'uses_middleware', 'uses_middleware', 1, COALESCE(r.scope_symbol_id, (SELECT MIN(id) FROM symbols WHERE file_id = r.file_id)), "
      "m.symbol_id, m.file_id, m.line, r.col, m.status, m.name, m.text, 'route middleware for ' || r.method || ' ' || r.path "
      "FROM route_middleware m JOIN routes r ON r.id = m.route_id WHERE m.ref_kind IN ('identifier','member','inline','class') "
      "AND m.scope = 'route' AND r.origin_route_id IS NULL");
}

// Prefixes under which the routes of a Django URLconf file are served: the
// include() chains from the root URLconf. {"", -1} when nothing includes it.
std::vector<std::pair<std::string, int64_t>> Resolver::django_prefixes(int64_t file, int depth, std::set<int64_t>& seen) {
  std::vector<std::pair<std::string, int64_t>> out;
  if (depth > 8 || !seen.insert(file).second) return {{"", -1}};
  struct M {
    int64_t id, file;
    std::string prefix;
  };
  std::vector<M> ms;
  auto& q = cached(
      "SELECT m.id, m.file_id, IFNULL(m.prefix, '') FROM mounts m JOIN symbols s ON s.id = m.target_symbol_id "
      "JOIN files f ON f.id = m.file_id WHERE m.status = 'urlconf' AND s.file_id = ? ORDER BY f.path, m.line, m.col");
  q.bind(1, file);
  while (q.step()) ms.push_back({q.int64(0), q.int64(1), q.text(2)});
  q.reset();
  if (ms.empty()) {
    seen.erase(file);
    return {{"", -1}};
  }
  for (const auto& m : ms) {
    for (const auto& parent : django_prefixes(m.file, depth + 1, seen)) {
      out.emplace_back(join_route(parent.first, m.prefix), m.id);
      if (out.size() >= 8) break;
    }
  }
  seen.erase(file);
  return out;
}

void Resolver::compute_route_prefixes() {
  struct RouterRow {
    int64_t id = -1, file = -1;
    std::string name, kind, framework, prefix;
  };
  auto load_router = db_.prepare("SELECT id, file_id, name, kind, framework, COALESCE(prefix,'') FROM routers WHERE id = ?");
  auto get_router = [&](int64_t id) {
    RouterRow r;
    load_router.bind(1, id);
    if (load_router.step()) {
      r = {load_router.int64(0), load_router.int64(1), load_router.text(2), load_router.text(3), load_router.text(4),
           load_router.text(5)};
    }
    load_router.reset();
    return r;
  };
  auto router_by_name = [&](int64_t file, const std::string& name) {
    q_router_.bind(1, file).bind(2, name);
    int64_t id = -1;
    if (q_router_.step()) id = q_router_.int64(0);
    q_router_.reset();
    return id >= 0 ? get_router(id) : RouterRow{};
  };
  auto mounts_of = db_.prepare(
      "SELECT m.id, m.file_id, m.registrar, m.prefix FROM mounts m JOIN files f ON f.id = m.file_id "
      "WHERE m.target_router_id = ? AND m.status = 'router' ORDER BY f.path, m.line, m.col");

  struct Prefix {
    std::string path;
    int64_t mount_id;
  };
  std::function<std::vector<Prefix>(const RouterRow&, int, std::set<int64_t>&)> prefixes_of;
  prefixes_of = [&](const RouterRow& router, int depth, std::set<int64_t>& seen) {
    std::vector<Prefix> out;
    if (depth > 8 || !seen.insert(router.id).second) return out;
    struct M {
      int64_t id, file;
      std::string registrar;
      std::optional<std::string> prefix;
    };
    std::vector<M> ms;
    mounts_of.bind(1, router.id);
    while (mounts_of.step()) ms.push_back({mounts_of.int64(0), mounts_of.int64(1), mounts_of.text(2), mounts_of.opt_text(3)});
    mounts_of.reset();
    for (const auto& m : ms) {
      std::string part;
      if (router.framework == "flask") part = m.prefix ? *m.prefix : router.prefix;
      else if (router.framework == "fastapi") part = join_route(m.prefix.value_or(""), router.prefix);
      else part = m.prefix.value_or("");
      RouterRow parent = router_by_name(m.file, m.registrar);
      if (parent.id < 0 || parent.kind == "app") {
        out.push_back({part, m.id});
      } else {
        for (const auto& pp : prefixes_of(parent, depth + 1, seen)) out.push_back({join_route(pp.path, part), m.id});
      }
      if (out.size() >= 8) break;
    }
    seen.erase(router.id);
    return out;
  };

  auto sel = db_.prepare(
      "SELECT id, framework, path, COALESCE(declared_prefix,''), file_id, registrar, line, col FROM routes "
      "WHERE mount_status = 'pending' AND id > ? ORDER BY id LIMIT ?");
  auto upd = db_.prepare(
      "UPDATE routes SET full_path = ?, mount_prefix = ?, mount_status = ?, mount_id = ?, router_id = ? WHERE id = ?");
  auto copy = db_.prepare(
      "INSERT INTO routes(framework, method, path, full_path, mount_prefix, mount_status, mount_id, file_id, line, col, "
      "registrar, router_id, scope_symbol_id, handler_kind, handler_name, handler_receiver, handler_text, "
      "handler_symbol_id, handler_status, handler_note, confidence, declared_prefix, origin_route_id, raw_method, "
      "raw_handler_note) SELECT framework, method, path, ?1, ?2, 'resolved', ?3, file_id, line, col, registrar, "
      "router_id, scope_symbol_id, handler_kind, handler_name, handler_receiver, handler_text, handler_symbol_id, "
      "handler_status, handler_note, confidence, declared_prefix, ?4, raw_method, raw_handler_note FROM routes WHERE id = ?4");
  auto copy_mw = db_.prepare(
      "INSERT INTO route_middleware(route_id, position, scope, ref_kind, name, receiver, text, symbol_id, status, "
      "file_id, line) SELECT ?, position, scope, ref_kind, name, receiver, text, symbol_id, status, file_id, line "
      "FROM route_middleware WHERE route_id = ? AND scope = 'route'");
  auto chain = db_.prepare(
      "SELECT target_kind, target_name, target_receiver, COALESCE(target_text, target_name), target_symbol_id, line, "
      "prefix FROM mounts WHERE file_id = ? AND registrar = ? AND status = 'middleware' AND (line < ? OR (line = ? AND col < ?)) "
      "ORDER BY line, col");
  auto add_mw = db_.prepare(
      "INSERT INTO route_middleware(route_id, position, scope, ref_kind, name, receiver, text, symbol_id, status, "
      "file_id, line) VALUES (?,?,?,?,?,?,?,?,?,?,?)");
  auto mount_info = db_.prepare("SELECT file_id, registrar, line, col FROM mounts WHERE id = ?");

  // Router-/app-level middleware registered before the route (or mount) on the same registrar.
  auto add_chain = [&](int64_t route_id, int64_t file, const std::string& registrar, int64_t line, int64_t col,
                       const std::string& path, const char* scope_label) {
    struct C {
      std::string kind, name, receiver, text;
      int64_t sym, line;
      std::optional<std::string> prefix;
    };
    std::vector<C> cs;
    chain.bind(1, file).bind(2, registrar).bind(3, line).bind(4, line).bind(5, col);
    while (chain.step()) {
      cs.push_back({chain.text(0), chain.text(1), chain.text(2), chain.text(3), chain.is_null(4) ? -1 : chain.int64(4),
                    chain.int64(5), chain.opt_text(6)});
    }
    chain.reset();
    int pos = 0;
    for (const auto& c : cs) {
      if (c.prefix && !path_has_prefix(path, *c.prefix)) continue;
      add_mw.bind(1, route_id).bind(2, pos++).bind(3, scope_label).bind(4, c.kind).bind_text_or_null(5, c.name);
      add_mw.bind_text_or_null(6, c.receiver).bind(7, c.text.empty() ? c.name : c.text).bind_id_or_null(8, c.sym);
      add_mw.bind(9, c.sym >= 0 ? "resolved" : "unresolved").bind(10, file).bind(11, c.line).run();
    }
  };

  int64_t last = 0;
  while (true) {
    struct Row {
      int64_t id;
      std::string framework, path, prefix;
      int64_t file;
      std::string registrar;
      int64_t line, col;
    };
    std::vector<Row> rows;
    sel.bind(1, last).bind(2, kPage);
    while (sel.step()) {
      rows.push_back({sel.int64(0), sel.text(1), sel.text(2), sel.text(3), sel.int64(4), sel.text(5), sel.int64(6), sel.int64(7)});
    }
    sel.reset();
    if (rows.empty()) break;
    for (const auto& r : rows) {
      last = r.id;
      if (r.framework == "django") {
        // Prefixes from include() chains; one route per distinct prefix.
        std::set<int64_t> seen;
        auto prefixes = django_prefixes(r.file, 0, seen);
        for (size_t i = 0; i < prefixes.size(); ++i) {
          std::string full = join_route(prefixes[i].first, r.path);
          bool mounted = prefixes[i].second >= 0;
          if (i == 0) {
            upd.bind(1, full).bind_text_or_null(2, mounted ? prefixes[i].first : "").bind(3, mounted ? "resolved" : "none");
            upd.bind_id_or_null(4, prefixes[i].second).bind(5, nullptr).bind(6, r.id).run();
          } else {
            copy.bind(1, full).bind(2, prefixes[i].first).bind(3, prefixes[i].second).bind(4, r.id).run();
            copy_mw.bind(1, db_.last_insert_rowid()).bind(2, r.id).run();
          }
        }
        continue;
      }
      if (r.framework == "pyramid") {
        // config.add_route('name', '/pattern') declares the path of a named route.
        auto& q = cached(
            "SELECT r.prefix FROM routers r JOIN files f ON f.id = r.file_id WHERE r.kind = 'pyramid_route' "
            "AND r.name = ? ORDER BY f.path, r.line LIMIT 1");
        q.bind(1, r.registrar);
        std::optional<std::string> pattern;
        if (q.step()) pattern = q.text(0);
        q.reset();
        upd.bind(1, join_route(pattern.value_or(""), r.path)).bind_text_or_null(2, pattern.value_or(""));
        upd.bind(3, pattern ? "resolved" : "unmounted").bind(4, nullptr).bind(5, nullptr).bind(6, r.id).run();
        continue;
      }
      // Frameworks whose prefixes are declared next to the route (class-level
      // mappings, Struts namespaces, OpenAPI base paths) rather than by mounting
      // router objects.
      if (r.framework != "express" && r.framework != "flask" && r.framework != "fastapi") {
        std::string full = join_route(r.prefix, r.path);
        upd.bind(1, full).bind_text_or_null(2, r.prefix == "/" ? "" : r.prefix);
        upd.bind(3, r.prefix.empty() || r.prefix == "/" ? "none" : "class_mapping").bind(4, nullptr).bind(5, nullptr);
        upd.bind(6, r.id).run();
        continue;
      }
      RouterRow router = router_by_name(r.file, r.registrar);
      if (router.id < 0) {
        upd.bind(1, join_route("", r.path)).bind(2, nullptr).bind(3, "unknown_registrar").bind(4, nullptr);
        upd.bind(5, nullptr).bind(6, r.id).run();
        add_chain(r.id, r.file, r.registrar, r.line, r.col, r.path, "router");
        continue;
      }
      if (router.kind == "app") {
        upd.bind(1, join_route("", r.path)).bind(2, nullptr).bind(3, "none").bind(4, nullptr).bind(5, router.id);
        upd.bind(6, r.id).run();
        add_chain(r.id, r.file, r.registrar, r.line, r.col, r.path, "app");
        continue;
      }
      std::set<int64_t> seen;
      auto prefixes = prefixes_of(router, 0, seen);
      if (prefixes.empty()) {
        std::string own = router.framework == "express" ? "" : router.prefix;
        upd.bind(1, join_route(own, r.path)).bind_text_or_null(2, own).bind(3, "unmounted").bind(4, nullptr);
        upd.bind(5, router.id).bind(6, r.id).run();
        add_chain(r.id, r.file, r.registrar, r.line, r.col, r.path, "router");
        continue;
      }
      for (size_t i = 0; i < prefixes.size(); ++i) {
        int64_t route_id = r.id;
        std::string full = join_route(prefixes[i].path, r.path);
        if (i == 0) {
          upd.bind(1, full).bind(2, prefixes[i].path).bind(3, "resolved").bind(4, prefixes[i].mount_id);
          upd.bind(5, router.id).bind(6, r.id).run();
        } else {
          copy.bind(1, full).bind(2, prefixes[i].path).bind(3, prefixes[i].mount_id).bind(4, r.id).run();
          route_id = db_.last_insert_rowid();
          copy_mw.bind(1, route_id).bind(2, r.id).run();
        }
        add_chain(route_id, r.file, r.registrar, r.line, r.col, r.path, "router");
        mount_info.bind(1, prefixes[i].mount_id);
        if (mount_info.step()) {
          int64_t mf = mount_info.int64(0);
          std::string mreg = mount_info.text(1);
          int64_t ml = mount_info.int64(2), mc = mount_info.int64(3);
          mount_info.reset();
          add_chain(route_id, mf, mreg, ml, mc + 1, full, "mount");
        } else {
          mount_info.reset();
        }
      }
    }
  }
}

// ===========================================================================
// Manifest (package.json) entry points
// ===========================================================================

void Resolver::add_manifest_entry_points() {
  auto ins = db_.prepare("INSERT INTO entry_points(kind, symbol_id, file_id, line, reason) VALUES (?,?,?,1,?)");
  std::set<std::pair<int64_t, std::string>> seen;
  auto add = [&](const std::string& dir, const std::string& target, const std::string& kind, const std::string& reason) {
    auto base = paths::join_relative(dir, target);
    if (!base) return;
    std::vector<std::string> cands = {*base};
    for (const char* e : {".js", ".ts", ".mjs", ".cjs", ".jsx", ".tsx"}) cands.push_back(*base + e);
    for (const char* e : {"/index.js", "/index.ts"}) cands.push_back(*base + e);
    if (ends_with(*base, ".js")) {
      std::string stem = base->substr(0, base->size() - 3);
      cands.insert(cands.begin(), stem + ".ts");
    }
    auto id = first_existing(cands);
    if (!id) {
      diagnostics_.add({"info", "source", "manifest_entry_missing", dir.empty() ? "package.json" : dir + "/package.json",
                        0, kind + " '" + target + "' does not match an indexed file"});
      return;
    }
    if (!seen.insert({*id, kind}).second) return;
    int64_t sym = module_symbol(*id);
    ins.bind(1, kind).bind_id_or_null(2, sym).bind(3, *id).bind(4, reason).run();
    ++stats_.manifest_entry_points;
  };
  for (const auto& m : manifests_) {
    std::ifstream in(root_ / m, std::ios::binary);
    if (!in) continue;
    std::stringstream ss;
    ss << in.rdbuf();
    json pkg = json::parse(ss.str(), nullptr, false);
    if (pkg.is_discarded() || !pkg.is_object()) {
      diagnostics_.add({"warn", "source", "manifest_invalid", m, 0, "package.json is not valid JSON"});
      continue;
    }
    std::string dir = paths::parent_dir(m);
    if (pkg.contains("main") && pkg["main"].is_string()) add(dir, pkg["main"], "package_main", "package.json \"main\"");
    if (pkg.contains("bin")) {
      if (pkg["bin"].is_string()) add(dir, pkg["bin"], "package_bin", "package.json \"bin\"");
      else if (pkg["bin"].is_object()) {
        for (auto& [k, v] : pkg["bin"].items()) {
          if (v.is_string()) add(dir, v, "package_bin", "package.json \"bin\" (" + k + ")");
        }
      }
    }
    if (pkg.contains("scripts") && pkg["scripts"].is_object()) {
      for (const char* key : {"start", "dev", "serve", "server"}) {
        if (!pkg["scripts"].contains(key) || !pkg["scripts"][key].is_string()) continue;
        std::string cmd = pkg["scripts"][key];
        std::stringstream ts(cmd);
        std::string tok, prev;
        while (ts >> tok) {
          bool runner = prev == "node" || prev == "nodemon" || prev == "ts-node" || prev == "tsx" || prev == "bun" ||
                        prev == "babel-node" || prev == "ts-node-dev";
          bool looks_js = ends_with(tok, ".js") || ends_with(tok, ".ts") || ends_with(tok, ".mjs") || ends_with(tok, ".cjs");
          if ((runner && tok[0] != '-') || looks_js) {
            add(dir, tok, "package_script", std::string("npm script \"") + key + "\": " + (cmd.size() > 80 ? cmd.substr(0, 80) + "..." : cmd));
            break;
          }
          prev = tok;
        }
      }
    }
  }
}

}  // namespace proj
