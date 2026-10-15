// Runtime observations (dynamic tracing) read from a project's runtime store and placed in this
// index: traces, errors and sessions, plus the RuntimeMap used to annotate static traversals.
#include <algorithm>
#include <functional>
#include <memory>

#include "dynamic/runtime_store.hpp"
#include "query/query.hpp"
#include "query/runtime_map.hpp"

namespace proj {
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// RuntimeMap
// ---------------------------------------------------------------------------

namespace {

std::string simple_type(const std::string& internal) {
  std::string s = internal;
  auto slash = s.find_last_of('/');
  if (slash != std::string::npos) s = s.substr(slash + 1);
  auto dollar = s.find_last_of('$');
  if (dollar != std::string::npos) s = s.substr(dollar + 1);
  return s;
}

std::string simple_class(const std::string& binary) {
  auto dot = binary.find_last_of('.');
  return dot == std::string::npos ? binary : binary.substr(dot + 1);
}

std::string package_dir(const std::string& binary) {
  auto dot = binary.find_last_of('.');
  if (dot == std::string::npos) return "";
  std::string p = binary.substr(0, dot);
  std::replace(p.begin(), p.end(), '.', '/');
  return p;
}

bool ends_with(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string squash(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c != ' ') out += c;
  }
  return out;
}

}  // namespace

std::vector<std::string> RuntimeMap::descriptor_params(const std::string& desc) {
  std::vector<std::string> out;
  size_t i = desc.find('(');
  if (i == std::string::npos) return out;
  ++i;
  while (i < desc.size() && desc[i] != ')') {
    int dims = 0;
    while (i < desc.size() && desc[i] == '[') {
      ++dims;
      ++i;
    }
    std::string t;
    switch (desc[i]) {
      case 'Z': t = "boolean"; break;
      case 'B': t = "byte"; break;
      case 'C': t = "char"; break;
      case 'S': t = "short"; break;
      case 'I': t = "int"; break;
      case 'J': t = "long"; break;
      case 'F': t = "float"; break;
      case 'D': t = "double"; break;
      case 'L': {
        size_t end = desc.find(';', i);
        if (end == std::string::npos) return out;
        t = simple_type(desc.substr(i + 1, end - i - 1));
        i = end;
        break;
      }
      default: return out;
    }
    ++i;
    for (int d = 0; d < dims; ++d) t += "[]";
    out.push_back(t);
  }
  return out;
}

std::string RtMethod::display() const {
  std::string params;
  for (const auto& p : RuntimeMap::descriptor_params(desc)) params += (params.empty() ? "" : ", ") + p;
  return simple_class(cls) + "." + name + "(" + params + ")";
}

const RtMethod* RuntimeMap::method(int64_t rt_id) {
  if (auto it = methods_.find(rt_id); it != methods_.end()) return &it->second;
  auto st = store_.prepare(
      "SELECT id, class_name, name, descriptor, IFNULL(source_file, ''), IFNULL(first_line, 0), IFNULL(last_line, 0) "
      "FROM rt_methods WHERE id = ?");
  st.bind(1, rt_id);
  if (!st.step()) return nullptr;
  RtMethod m{st.int64(0), st.text(1), st.text(2), st.text(3), st.text(4), st.int64(5), st.int64(6)};
  return &methods_.emplace(rt_id, std::move(m)).first->second;
}

int64_t RuntimeMap::file_for(const std::string& cls, const std::string& source) {
  std::string dir = package_dir(cls);
  std::string file = source;
  if (file.empty()) {
    std::string outer = simple_class(cls);
    outer = outer.substr(0, outer.find('$'));
    file = outer + ".java";
  }
  std::string key = dir + "|" + file;
  if (auto it = files_.find(key); it != files_.end()) return it->second;
  // Package directories sit under any source root (src/main/java/...).
  std::string want = dir.empty() ? file : dir + "/" + file;
  auto st = index_.prepare("SELECT id, path FROM files WHERE name = ? ORDER BY path");
  st.bind(1, file);
  int64_t found = -1;
  while (st.step()) {
    std::string path = st.text(1);
    if (path == want || ends_with(path, "/" + want)) {
      found = st.int64(0);
      break;
    }
  }
  files_[key] = found;
  return found;
}

const std::vector<RuntimeMap::Sym>& RuntimeMap::file_symbols(int64_t file_id) {
  if (auto it = symbols_.find(file_id); it != symbols_.end()) return it->second;
  auto st = index_.prepare(
      "SELECT id, name, kind, start_line, end_line, IFNULL(param_count, -1), IFNULL(param_types, '') FROM symbols "
      "WHERE file_id = ? AND kind IN ('method', 'constructor', 'function', 'route_handler', 'class', 'interface', 'enum', 'record') "
      "ORDER BY start_line");
  st.bind(1, file_id);
  std::vector<Sym> out;
  while (st.step()) out.push_back({st.int64(0), st.text(1), st.text(2), st.int64(3), st.int64(4), st.int64(5), st.text(6)});
  return symbols_.emplace(file_id, std::move(out)).first->second;
}

RtMapping RuntimeMap::place(int64_t file, const std::string& cls, const std::string& name, const std::string& desc,
                            int64_t line) {
  const auto& syms = file_symbols(file);
  auto is_callable = [](const Sym& s) { return s.kind == "method" || s.kind == "constructor" || s.kind == "function" || s.kind == "route_handler"; };
  auto contains = [&](const Sym& s) { return line > 0 && s.start <= line && line <= s.end; };
  // Innermost callable (or, failing that, class) containing the line.
  auto innermost = [&](bool callable_only) -> int64_t {
    int64_t best = -1;
    int64_t span = INT64_MAX;
    for (const auto& s : syms) {
      if (!contains(s) || (callable_only && !is_callable(s))) continue;
      if (s.end - s.start < span) {
        span = s.end - s.start;
        best = s.id;
      }
    }
    return best;
  };
  std::string simple = simple_class(cls);
  std::string inner = simple.substr(simple.find_last_of('$') == std::string::npos ? 0 : simple.find_last_of('$') + 1);
  bool anonymous = !inner.empty() && std::all_of(inner.begin(), inner.end(), [](char c) { return c >= '0' && c <= '9'; });
  if (name.rfind("lambda$", 0) == 0) {
    int64_t s = innermost(true);
    return s >= 0 ? RtMapping{s, "lambda"} : RtMapping{};
  }
  if (name == "<clinit>") {
    for (const auto& s : syms) {
      if (s.kind != "method" && s.kind != "constructor" && s.name == inner) return {s.id, "implicit"};
    }
    return {};
  }
  std::string want = name == "<init>" ? inner : name;
  std::vector<const Sym*> cands;
  for (const auto& s : syms) {
    if (s.name != want) continue;
    if (name == "<init>" ? s.kind == "constructor" : is_callable(s) && s.kind != "constructor") cands.push_back(&s);
  }
  if (cands.empty()) {
    if (name == "<init>") {
      if (anonymous) {
        int64_t s = innermost(true);
        return s >= 0 ? RtMapping{s, "enclosing"} : RtMapping{};
      }
      for (const auto& s : syms) {
        if (!is_callable(s) && s.name == inner) return {s.id, "implicit"};
      }
      return {};
    }
    int64_t s = innermost(true);
    return s >= 0 ? RtMapping{s, "enclosing"} : RtMapping{};
  }
  if (cands.size() == 1) return {cands[0]->id, "exact"};
  std::vector<const Sym*> narrowed;
  for (const auto* s : cands) {
    if (contains(*s)) narrowed.push_back(s);
  }
  if (narrowed.size() == 1) return {narrowed[0]->id, "exact"};
  if (narrowed.empty()) narrowed = cands;
  if (!desc.empty()) {
    auto params = descriptor_params(desc);
    std::string joined;
    for (const auto& p : params) joined += (joined.empty() ? "" : ",") + p;
    std::vector<const Sym*> by_count;
    for (const auto* s : narrowed) {
      if (s->params == static_cast<int64_t>(params.size())) by_count.push_back(s);
    }
    if (by_count.size() == 1) return {by_count[0]->id, "exact"};
    for (const auto* s : by_count) {
      if (squash(s->param_types) == joined) return {s->id, "exact"};
    }
    if (!by_count.empty()) narrowed = by_count;
  }
  return {narrowed[0]->id, "ambiguous"};
}

RtMapping RuntimeMap::map(int64_t rt_id) {
  if (auto it = mappings_.find(rt_id); it != mappings_.end()) return it->second;
  RtMapping out;
  if (const RtMethod* m = method(rt_id)) {
    int64_t file = file_for(m->cls, m->source);
    if (file >= 0) out = place(file, m->cls, m->name, m->desc, m->first_line);
  }
  mappings_[rt_id] = out;
  return out;
}

RtMapping RuntimeMap::map_frame(const std::string& cls, const std::string& method, const std::string& file, int64_t line) {
  int64_t f = file_for(cls, file);
  return f >= 0 ? place(f, cls, method, "", line) : RtMapping{};
}

std::vector<int64_t> RuntimeMap::methods_of_symbol(int64_t symbol_id) {
  std::vector<int64_t> out;
  auto st = index_.prepare("SELECT f.name FROM symbols s JOIN files f ON f.id = s.file_id WHERE s.id = ?");
  st.bind(1, symbol_id);
  if (!st.step()) return out;
  std::string file = st.text(0);
  auto rt = store_.prepare("SELECT id FROM rt_methods WHERE source_file = ?");
  rt.bind(1, file);
  std::vector<int64_t> ids;
  while (rt.step()) ids.push_back(rt.int64(0));
  for (int64_t id : ids) {
    if (map(id).symbol == symbol_id) out.push_back(id);
  }
  return out;
}

Evidence RuntimeMap::evidence(int64_t caller, int64_t callee) {
  auto direct = index_.prepare(
      "SELECT status FROM edges WHERE src_symbol_id = ? AND dst_symbol_id = ? AND kind IN "
      "('calls', 'instantiates', 'configures', 'registers_route', 'uses_middleware') ORDER BY status = 'resolved' DESC LIMIT 1");
  direct.bind(1, caller).bind(2, callee);
  if (direct.step()) return {direct.text(0) == "resolved" ? "STATIC_CONFIRMED" : "STATIC_INFERRED", -1, -1, ""};
  auto cand = index_.prepare(
      "SELECT e.id FROM edges e JOIN edge_candidates c ON c.edge_id = e.id WHERE e.src_symbol_id = ? AND c.symbol_id = ? LIMIT 1");
  cand.bind(1, caller).bind(2, callee);
  if (cand.step()) return {"STATIC_INFERRED", cand.int64(0), -1, "runtime picked this candidate of an ambiguous call"};
  auto dispatch = index_.prepare(
      "SELECT e.dst_symbol_id FROM edges e JOIN edges o ON o.src_symbol_id = e.dst_symbol_id "
      "WHERE e.src_symbol_id = ? AND e.kind = 'calls' AND o.kind = 'overridden_by' AND o.dst_symbol_id = ? LIMIT 1");
  dispatch.bind(1, caller).bind(2, callee);
  if (dispatch.step()) return {"STATIC_INFERRED", -1, dispatch.int64(0), "dynamic dispatch to this override"};
  // A constructor's implicit super() call: the static model records the inheritance, not the call.
  auto super_ctor = index_.prepare(
      "SELECT 1 FROM symbols c JOIN symbols t ON t.id = ? JOIN edges e ON e.kind = 'extends' AND e.dst_symbol_id = "
      "CASE WHEN t.kind = 'constructor' THEN t.parent_id ELSE t.id END "
      "WHERE c.id = ? AND (c.kind = 'constructor' OR c.kind = 'class') AND (t.kind = 'constructor' OR t.kind = 'class') "
      "AND e.src_symbol_id = CASE WHEN c.kind = 'constructor' THEN c.parent_id ELSE c.id END LIMIT 1");
  super_ctor.bind(1, callee).bind(2, caller);
  if (super_ctor.step()) return {"STATIC_INFERRED", -1, -1, "the superclass constructor (an implicit super() call)"};
  return {"RUNTIME_OBSERVED", -1, -1, "seen at runtime; no static call site links these"};
}

void RuntimeMap::observed(int64_t symbol, bool forward, std::map<int64_t, int64_t>& mapped,
                          std::map<std::string, int64_t>& unmapped) {
  auto q = store_.prepare(forward ? "SELECT callee, calls FROM rt_edges WHERE caller = ?"
                                  : "SELECT caller, calls FROM rt_edges WHERE callee = ?");
  for (int64_t self : methods_of_symbol(symbol)) {
    q.bind(1, self);
    std::vector<std::pair<int64_t, int64_t>> rows;
    while (q.step()) rows.emplace_back(q.int64(0), q.int64(1));
    q.reset();
    for (auto [other, n] : rows) {
      RtMapping m = map(other);
      // Another runtime method of the same symbol is a lambda body inside it (its calls already
      // count as this method's); the same runtime method is recursion.
      if (m.symbol == symbol && other != self) continue;
      if (m.symbol >= 0) {
        mapped[m.symbol] += n;
      } else if (const RtMethod* om = method(other)) {
        unmapped[om->cls + "." + om->name + om->desc] += n;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// IndexReader: runtime queries
// ---------------------------------------------------------------------------

namespace {

struct Store {
  std::unique_ptr<db::Database> db;
  bool open(const fs::path& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return false;
    db = std::make_unique<db::Database>(path, db::OpenMode::ReadOnly);
    db->exec("PRAGMA busy_timeout=5000");
    return dynamic::runtime_schema_current(*db);
  }
};

json method_json(RuntimeMap& map, int64_t rt_id) {
  const RtMethod* m = map.method(rt_id);
  if (!m) return {{"id", rt_id}, {"display", "#" + std::to_string(rt_id)}};
  return {{"id", m->id}, {"class", m->cls}, {"name", m->name}, {"descriptor", m->desc}, {"display", m->display()},
          {"source", m->source.empty() ? json(nullptr) : json(m->source)}, {"line", m->first_line}};
}

json empty_store(const char* list) { return {{list, json::array()}, {"total", 0}, {"store", false}}; }

}  // namespace

json IndexReader::runtime_sessions(const fs::path& path, int64_t limit) {
  Store s;
  if (!s.open(path)) return empty_store("sessions");
  auto st = s.db->prepare(
      "SELECT id, pid, mode, startup_coverage, attach_delay_ms, agent_version, jvm_version, jvm_vendor, include, "
      "source_revision, started_at, ended_at, status, end_reason, stats_json, traces, errors FROM rt_sessions "
      "ORDER BY id DESC LIMIT ?");
  st.bind(1, std::clamp<int64_t>(limit, 1, QueryLimits::kMaxResults));
  json out = json::array();
  while (st.step()) {
    json stats = json::parse(st.text(14), nullptr, false);
    out.push_back({{"id", st.int64(0)},
                   {"pid", st.int64(1)},
                   {"mode", st.text(2)},
                   {"startup_coverage", st.text(3)},
                   {"attach_delay_ms", st.is_null(4) ? json(nullptr) : json(st.int64(4))},
                   {"agent_version", st.text(5)},
                   {"jvm", st.text(6) + (st.text(7).empty() ? "" : " (" + st.text(7) + ")")},
                   {"include", st.text(8)},
                   {"source_revision", st.text(9)},
                   {"started_at", st.text(10)},
                   {"ended_at", st.is_null(11) ? json(nullptr) : json(st.text(11))},
                   {"status", st.text(12)},
                   {"end_reason", st.is_null(13) ? json(nullptr) : json(st.text(13))},
                   {"stats", stats.is_discarded() ? json::object() : stats},
                   {"traces", st.int64(15)},
                   {"errors", st.int64(16)}});
  }
  return {{"sessions", out}, {"total", s.db->query_int("SELECT COUNT(*) FROM rt_sessions").value_or(0)}, {"store", true}};
}

json IndexReader::runtime_traces(const fs::path& path, int64_t limit, int64_t offset) {
  Store s;
  if (!s.open(path)) return empty_store("traces");
  RuntimeMap map(db_, *s.db);
  auto st = s.db->prepare(
      "SELECT t.id, t.session_id, t.thread_name, t.root_method, t.http_method, t.http_path, t.started_ms, t.duration_us, "
      "t.status, t.node_count, t.dropped_nodes, t.error_count, ss.mode, ss.startup_coverage FROM rt_traces t "
      "JOIN rt_sessions ss ON ss.id = t.session_id ORDER BY t.id DESC LIMIT ? OFFSET ?");
  st.bind(1, std::clamp<int64_t>(limit, 1, QueryLimits::kMaxResults)).bind(2, std::max<int64_t>(0, offset));
  json out = json::array();
  while (st.step()) {
    int64_t root = st.int64(3);
    RtMapping mapping = map.map(root);
    json t = {{"id", st.int64(0)},
              {"session", st.int64(1)},
              {"thread", st.text(2)},
              {"root", method_json(map, root)},
              {"root_symbol", mapping.symbol >= 0 ? symbol_brief(mapping.symbol) : json(nullptr)},
              {"started_ms", st.int64(6)},
              {"duration_us", st.int64(7)},
              {"status", st.text(8)},
              {"nodes", st.int64(9)},
              {"dropped_nodes", st.int64(10)},
              {"errors", st.int64(11)}};
    if (!st.is_null(4)) t["http"] = {{"method", st.text(4)}, {"path", st.text(5)}};
    out.push_back(std::move(t));
  }
  return {{"traces", out}, {"total", s.db->query_int("SELECT COUNT(*) FROM rt_traces").value_or(0)}, {"store", true}};
}

json IndexReader::runtime_trace(const fs::path& path, int64_t trace_id) {
  Store s;
  if (!s.open(path)) return nullptr;
  RuntimeMap map(db_, *s.db);
  auto st = s.db->prepare(
      "SELECT t.id, t.session_id, t.thread_name, t.thread_id, t.http_method, t.http_path, t.started_ms, t.duration_us, "
      "t.status, t.dropped_nodes, t.nodes, ss.mode, ss.startup_coverage, ss.pid FROM rt_traces t "
      "JOIN rt_sessions ss ON ss.id = t.session_id WHERE t.id = ?");
  st.bind(1, trace_id);
  if (!st.step()) return nullptr;
  json raw = json::parse(st.text(10), nullptr, false);
  json header = {{"id", st.int64(0)},
                 {"session", st.int64(1)},
                 {"thread", st.text(2)},
                 {"thread_id", st.int64(3)},
                 {"started_ms", st.int64(6)},
                 {"duration_us", st.int64(7)},
                 {"status", st.text(8)},
                 {"dropped_nodes", st.int64(9)},
                 {"session_mode", st.text(11)},
                 {"startup_coverage", st.text(12)},
                 {"pid", st.int64(13)}};
  if (!st.is_null(4)) header["http"] = {{"method", st.text(4)}, {"path", st.text(5)}};
  json nodes = json::array();
  std::vector<int64_t> symbol_of;
  std::vector<int> depth;
  if (raw.is_array()) {
    for (size_t i = 0; i < raw.size(); ++i) {
      const json& n = raw[i];
      int64_t m = n[0].get<int64_t>();
      int64_t parent = n[1].get<int64_t>();
      RtMapping mapping = map.map(m);
      depth.push_back(parent < 0 ? 0 : depth[static_cast<size_t>(parent)] + 1);
      symbol_of.push_back(mapping.symbol);
      json node = {{"index", i},
                   {"parent", parent < 0 ? json(nullptr) : json(parent)},
                   {"depth", depth.back()},
                   {"method", method_json(map, m)},
                   {"mapping", mapping.status},
                   {"symbol", mapping.symbol >= 0 ? symbol_brief(mapping.symbol) : json(nullptr)},
                   {"calls", n[2]},
                   {"first_us", n[3]},
                   {"exits_by_exception", n[4]}};
      // How the static graph accounts for the call from the parent.
      if (parent >= 0) {
        int64_t from = symbol_of[static_cast<size_t>(parent)];
        if (mapping.symbol < 0 || from < 0) {
          node["evidence"] = "RUNTIME_UNRESOLVED";
        } else if (from == mapping.symbol) {
          node["evidence"] = "STATIC_CONFIRMED";  // a lambda body: the static model credits its calls to this method
        } else {
          Evidence ev = map.evidence(from, mapping.symbol);
          node["evidence"] = ev.kind;
          if (ev.kind != "STATIC_CONFIRMED") node["evidence_note"] = ev.note;
        }
      }
      nodes.push_back(std::move(node));
    }
  }
  header["nodes"] = nodes;
  json errors = json::array();
  auto e = s.db->prepare(
      "SELECT e.id, e.exception, e.state, e.message FROM rt_errors e JOIN rt_error_traces l ON l.error_id = e.id "
      "WHERE l.trace_id = ? ORDER BY e.id");
  e.bind(1, trace_id);
  while (e.step()) errors.push_back({{"id", e.int64(0)}, {"exception", e.text(1)}, {"state", e.text(2)}, {"message", e.is_null(3) ? json(nullptr) : json(e.text(3))}});
  header["errors"] = errors;
  return header;
}

namespace {
json site_json(RuntimeMap& map, db::Statement& st, int method_col, int line_col, const std::function<json(int64_t)>& brief) {
  if (st.is_null(method_col)) return nullptr;
  int64_t m = st.int64(method_col);
  RtMapping mapping = map.map(m);
  return {{"method", method_json(map, m)},
          {"line", st.int64(line_col)},
          {"mapping", mapping.status},
          {"symbol", mapping.symbol >= 0 ? brief(mapping.symbol) : json(nullptr)}};
}
}  // namespace

json IndexReader::runtime_errors(const fs::path& path, int64_t limit, int64_t offset) {
  Store s;
  if (!s.open(path)) return empty_store("errors");
  RuntimeMap map(db_, *s.db);
  auto st = s.db->prepare(
      "SELECT id, exception, message, state, rethrown, wrapped_by, occurrences, first_seen, last_seen, frames, thread_name "
      "FROM rt_errors ORDER BY last_seen DESC, id DESC LIMIT ? OFFSET ?");
  st.bind(1, std::clamp<int64_t>(limit, 1, QueryLimits::kMaxResults)).bind(2, std::max<int64_t>(0, offset));
  json out = json::array();
  while (st.step()) {
    json frames = json::parse(st.text(9), nullptr, false);
    json top = nullptr;  // the most recent application frame
    if (frames.is_array()) {
      for (const auto& f : frames) {
        if (f.is_array() && f.size() >= 5 && f[4] == 1) {
          RtMapping m = map.map_frame(f[0].get<std::string>(), f[1].get<std::string>(), f[2].is_string() ? f[2].get<std::string>() : "",
                                      f[3].get<int64_t>());
          top = {{"class", f[0]}, {"method", f[1]}, {"file", f[2]}, {"line", f[3]},
                 {"symbol", m.symbol >= 0 ? symbol_brief(m.symbol) : json(nullptr)}};
          break;
        }
      }
    }
    out.push_back({{"id", st.int64(0)},
                   {"exception", st.text(1)},
                   {"message", st.is_null(2) ? json(nullptr) : json(st.text(2))},
                   {"state", st.text(3)},
                   {"rethrown", st.int64(4) != 0},
                   {"wrapped_by", st.is_null(5) ? json(nullptr) : json(st.text(5))},
                   {"occurrences", st.int64(6)},
                   {"first_seen", st.text(7)},
                   {"last_seen", st.text(8)},
                   {"thread", st.is_null(10) ? json(nullptr) : json(st.text(10))},
                   {"top_app_frame", top}});
  }
  return {{"errors", out}, {"total", s.db->query_int("SELECT COUNT(*) FROM rt_errors").value_or(0)}, {"store", true}};
}

json IndexReader::runtime_error(const fs::path& path, int64_t error_id) {
  Store s;
  if (!s.open(path)) return nullptr;
  RuntimeMap map(db_, *s.db);
  auto st = s.db->prepare(
      "SELECT id, exception, message, state, rethrown, wrapped_by, throw_method, throw_line, caught_method, caught_line, "
      "caught_type, escaped_from, thread_name, frames, frames_omitted, causes, suppressed, events, occurrences, first_seen, "
      "last_seen, last_trace FROM rt_errors WHERE id = ?");
  st.bind(1, error_id);
  if (!st.step()) return nullptr;
  auto brief = [&](int64_t id) { return symbol_brief(id); };
  // Frames as stored: deepest (most recent) first, which is also the display order.
  auto frames_json = [&](const json& raw) {
    json out = json::array();
    if (!raw.is_array()) return out;
    for (const auto& f : raw) {
      if (!f.is_array() || f.size() < 5) continue;
      std::string cls = f[0].get<std::string>();
      std::string method = f[1].get<std::string>();
      std::string file = f[2].is_string() ? f[2].get<std::string>() : "";
      int64_t line = f[3].get<int64_t>();
      bool app = f[4] == 1;
      json frame = {{"class", cls}, {"method", method}, {"file", file.empty() ? json(nullptr) : json(file)}, {"line", line},
                    {"application", app}};
      if (app) {
        RtMapping m = map.map_frame(cls, method, file, line);
        frame["mapping"] = m.status;
        frame["symbol"] = m.symbol >= 0 ? symbol_brief(m.symbol) : json(nullptr);
      } else {
        frame["mapping"] = "external";
      }
      out.push_back(frame);
    }
    return out;
  };
  json frames = frames_json(json::parse(st.text(13), nullptr, false));
  json causes = json::array();
  json raw_causes = json::parse(st.text(15), nullptr, false);
  if (raw_causes.is_array()) {
    for (const auto& c : raw_causes) {
      causes.push_back({{"exception", c.value("exception", std::string())},
                        {"message", c.value("message", json(nullptr))},
                        {"frames", frames_json(c.value("frames", json::array()))},
                        {"frames_omitted", c.value("frames_omitted", 0)}});
    }
  }
  json events = json::array();
  json raw_events = json::parse(st.text(17), nullptr, false);
  if (raw_events.is_array()) {
    for (const auto& ev : raw_events) {
      if (!ev.is_array() || ev.size() < 2) continue;
      json e = {{"kind", ev[0]}, {"method", method_json(map, ev[1].get<int64_t>())}};
      if (ev.size() > 2) e["line"] = ev[2];
      events.push_back(e);
    }
  }
  json out = {{"id", st.int64(0)},
              {"exception", st.text(1)},
              {"message", st.is_null(2) ? json(nullptr) : json(st.text(2))},
              {"state", st.text(3)},
              {"rethrown", st.int64(4) != 0},
              {"wrapped_by", st.is_null(5) ? json(nullptr) : json(st.text(5))},
              {"throw_site", site_json(map, st, 6, 7, brief)},
              {"caught_at", site_json(map, st, 8, 9, brief)},
              {"caught_type", st.is_null(10) ? json(nullptr) : json(st.text(10))},
              {"escaped_from", st.is_null(11) ? json(nullptr) : method_json(map, st.int64(11))},
              {"thread", st.is_null(12) ? json(nullptr) : json(st.text(12))},
              {"frames", frames},
              {"frames_omitted", st.int64(14)},
              {"causes", causes},
              {"suppressed", st.int64(16)},
              {"events", events},
              {"occurrences", st.int64(18)},
              {"first_seen", st.text(19)},
              {"last_seen", st.text(20)}};
  // The most recent application frame: often, but not always, where it was thrown.
  for (const auto& f : frames) {
    if (f["application"] == true) {
      out["top_app_frame"] = f;
      break;
    }
  }
  json traces = json::array();
  auto t = s.db->prepare(
      "SELECT t.id, t.http_method, t.http_path, t.thread_name, t.started_ms FROM rt_error_traces l JOIN rt_traces t "
      "ON t.id = l.trace_id WHERE l.error_id = ? ORDER BY t.id DESC LIMIT 10");
  t.bind(1, error_id);
  while (t.step()) {
    json tr = {{"id", t.int64(0)}, {"thread", t.text(3)}, {"started_ms", t.int64(4)}};
    if (!t.is_null(1)) tr["http"] = {{"method", t.text(1)}, {"path", t.text(2)}};
    traces.push_back(tr);
  }
  out["traces"] = traces;
  return out;
}

}  // namespace proj
