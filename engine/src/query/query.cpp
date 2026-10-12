#include "query/query.hpp"

#include <algorithm>
#include <deque>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "core/hash.hpp"
#include "core/paths.hpp"
#include "db/schema.hpp"

namespace proj {
namespace fs = std::filesystem;
namespace {

constexpr const char* kSymbolBriefCols =
    "s.id, s.name, s.qualified_name, s.kind, s.language, f.path, s.start_line, s.end_line, s.exported";

const std::vector<std::string> kDefaultTraverseKinds = {"calls", "instantiates", "configures", "registers_route", "uses_middleware",
                                                        "mounts", "loads_module", "overridden_by"};

std::string in_list(const std::vector<std::string>& items) {
  std::string out = "(";
  for (size_t i = 0; i < items.size(); ++i) out += (i ? ",?" : "?");
  return out + ")";
}

std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// Canonical route path for matching: parameters (:id, {id}, <int:id>) -> "*"
// (one segment); a literal "*" or "**" (servlet "/api/*", Ant "/**", Express
// "/files/*") -> "**" (any number of segments); "*.do" (servlet extension
// mapping) is kept and matches a last segment ending in ".do".
std::string route_shape(const std::string& path) {
  std::string out;
  std::stringstream ss(path);
  std::string seg;
  bool first = true;
  while (std::getline(ss, seg, '/')) {
    if (first) {
      first = false;
      if (seg.empty()) continue;
    }
    if (seg.empty()) continue;
    bool param = seg[0] == ':' || (seg.front() == '{' && seg.back() == '}') || (seg.front() == '<' && seg.back() == '>');
    if (seg == "*" || seg == "**") out += "/**";
    else out += "/" + (param ? std::string("*") : lower(seg));
  }
  return out.empty() ? "/" : out;
}

bool segments_match(const std::vector<std::string>& a, size_t i, const std::vector<std::string>& b, size_t j, int budget) {
  if (budget <= 0) return false;
  if (i == a.size()) return j == b.size();
  if (a[i] == "**") {
    // zero or more query segments
    for (size_t k = j; k <= b.size(); ++k) {
      if (segments_match(a, i + 1, b, k, budget - 1)) return true;
    }
    return false;
  }
  if (j == b.size()) return false;
  const std::string& r = a[i];
  const std::string& q = b[j];
  bool ok = r == "*" || q == "*" || q == "**" || r == q;
  if (!ok && r.size() > 2 && r.rfind("*.", 0) == 0) {
    // extension mapping: "*.do" matches "login.do"
    ok = q.size() >= r.size() - 1 && q.compare(q.size() - (r.size() - 1), r.size() - 1, r, 1) == 0;
  }
  return ok && segments_match(a, i + 1, b, j + 1, budget - 1);
}

bool shape_matches(const std::string& route, const std::string& query) {
  // A query segment that is a parameter or a concrete value matches a route parameter.
  std::vector<std::string> a, b;
  std::stringstream sa(route), sb(query);
  for (std::string s; std::getline(sa, s, '/');) if (!s.empty()) a.push_back(s);
  for (std::string s; std::getline(sb, s, '/');) if (!s.empty()) b.push_back(s);
  if (a.size() > 64 || b.size() > 64) return false;
  return segments_match(a, 0, b, 0, 4096);
}

int64_t clamp(int64_t v, int64_t lo, int64_t hi) { return std::max(lo, std::min(v, hi)); }

}  // namespace

IndexReader::IndexReader(const fs::path& index_path) : path_(index_path) {
  db_ = db::Database(index_path, db::OpenMode::ReadOnly);
  db::configure_index_read(db_);
  db::check_index_schema(db_);
}

std::string IndexReader::meta(const std::string& key) {
  auto st = db_.prepare("SELECT value FROM index_meta WHERE key = ?");
  st.bind(1, key);
  return st.step() ? st.text(0) : "";
}

json IndexReader::symbol_brief_row(db::Statement& st, int c) {
  return {{"id", st.int64(c)},
          {"name", st.text(c + 1)},
          {"qualified_name", st.text(c + 2)},
          {"kind", st.text(c + 3)},
          {"language", st.text(c + 4)},
          {"path", st.text(c + 5)},
          {"line", st.int64(c + 6)},
          {"end_line", st.int64(c + 7)},
          {"exported", st.int64(c + 8) != 0}};
}

json IndexReader::symbol_brief(int64_t id) {
  auto st = db_.prepare(std::string("SELECT ") + kSymbolBriefCols +
                        " FROM symbols s JOIN files f ON f.id = s.file_id WHERE s.id = ?");
  st.bind(1, id);
  if (!st.step()) return nullptr;
  return symbol_brief_row(st, 0);
}

json IndexReader::summary() {
  json stats = json::parse(meta("stats"), nullptr, false);
  if (stats.is_discarded()) stats = json::object();
  auto count = [&](const char* sql) { return db_.query_int(sql).value_or(0); };
  json languages = json::object();
  auto st = db_.prepare(
      "SELECT language, COUNT(*) FROM files WHERE language IS NOT NULL AND status IN ('parsed','syntax_errors') "
      "GROUP BY language ORDER BY language");
  while (st.step()) languages[st.text(0)] = st.int64(1);
  return {{"project", meta("project_name")},
          {"root_path", meta("root_path")},
          {"analysis_status", meta("analysis_status")},
          {"indexed_at", meta("created_at")},
          {"run_id", meta("run_id")},
          {"tool_version", meta("tool_version")},
          {"schema_version", meta("schema_version")},
          {"counts",
           {{"files", count("SELECT COUNT(*) FROM files")},
            {"source_files", count("SELECT COUNT(*) FROM files WHERE status IN ('parsed','syntax_errors')")},
            {"symbols", count("SELECT COUNT(*) FROM symbols WHERE kind != 'module'")},
            {"relationships", count("SELECT (SELECT COUNT(*) FROM edges) + (SELECT COUNT(*) FROM file_deps)")},
            {"call_edges", count("SELECT COUNT(*) FROM edges WHERE kind IN ('calls','instantiates')")},
            {"dependencies", count("SELECT COUNT(*) FROM file_deps")},
            {"routes", count("SELECT COUNT(*) FROM routes")},
            {"entry_points", count("SELECT COUNT(*) FROM entry_points")},
            {"exception_sites", count("SELECT COUNT(*) FROM exceptions WHERE kind IN ('throw','rethrow')")},
            {"unresolved_edges", count("SELECT COUNT(*) FROM edges WHERE status = 'unresolved'")}}},
          {"languages", languages},
          {"stats", stats}};
}

json IndexReader::search(const std::string& query, const std::vector<std::string>& kinds, bool exact, int64_t limit) {
  limit = clamp(limit, 1, QueryLimits::kMaxResults);
  auto want = [&](const char* k) { return kinds.empty() || std::find(kinds.begin(), kinds.end(), k) != kinds.end(); };
  json result = {{"query", query}, {"exact", exact}};
  std::string q = query;
  while (!q.empty() && std::isspace(static_cast<unsigned char>(q.back()))) q.pop_back();
  while (!q.empty() && std::isspace(static_cast<unsigned char>(q.front()))) q.erase(q.begin());

  // --- routes: "GET /api/users/:id", "/api/users/42", "users" ---
  std::string method, path_query = q;
  {
    auto sp = q.find(' ');
    std::string head = lower(sp == std::string::npos ? q : q.substr(0, sp));
    static const std::set<std::string> kMethods = {"get", "post", "put", "patch", "delete", "options", "head", "all"};
    if (kMethods.count(head)) {
      method = head;
      for (auto& c : method) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      path_query = sp == std::string::npos ? "" : q.substr(sp + 1);
      while (!path_query.empty() && path_query.front() == ' ') path_query.erase(path_query.begin());
    }
  }
  bool route_like = !method.empty() || (!path_query.empty() && path_query[0] == '/');

  if (want("symbol") && !q.empty() && !(route_like && !method.empty())) {
    json symbols = json::array();
    std::string sql;
    db::Statement st;
    if (exact) {
      sql = std::string("SELECT ") + kSymbolBriefCols +
            ", (SELECT p.qualified_name FROM symbols p WHERE p.id = s.parent_id) FROM symbols s JOIN files f ON "
            "f.id = s.file_id WHERE (s.name = ?1 OR s.qualified_name = ?1) AND s.kind != 'module' "
            "ORDER BY f.path, s.start_line LIMIT ?2";
      st = db_.prepare(sql);
      st.bind(1, q).bind(2, limit);
    } else if (q.size() >= 3) {
      std::string phrase = "\"";
      for (char c : q) phrase += c == '"' ? std::string("\"\"") : std::string(1, c);
      phrase += "\"";
      sql = std::string("SELECT ") + kSymbolBriefCols +
            ", (SELECT p.qualified_name FROM symbols p WHERE p.id = s.parent_id) FROM symbols_fts JOIN symbols s ON "
            "s.id = symbols_fts.rowid JOIN files f ON f.id = s.file_id WHERE symbols_fts MATCH ?1 AND s.kind != 'module' "
            "ORDER BY (lower(s.name) = lower(?2)) DESC, (lower(s.name) LIKE lower(?2) || '%') DESC, length(s.name), "
            "f.path, s.start_line LIMIT ?3";
      st = db_.prepare(sql);
      st.bind(1, phrase).bind(2, q).bind(3, limit);
    } else {
      sql = std::string("SELECT ") + kSymbolBriefCols +
            ", (SELECT p.qualified_name FROM symbols p WHERE p.id = s.parent_id) FROM symbols s JOIN files f ON "
            "f.id = s.file_id WHERE s.name LIKE ?1 || '%' AND s.kind != 'module' "
            "ORDER BY (lower(s.name) = lower(?1)) DESC, length(s.name), f.path, s.start_line LIMIT ?2";
      st = db_.prepare(sql);
      st.bind(1, q).bind(2, limit);
    }
    while (st.step()) {
      json s = symbol_brief_row(st, 0);
      s["parent"] = st.is_null(9) ? json(nullptr) : json(st.text(9));
      symbols.push_back(std::move(s));
    }
    // Same name in several places: flag it so the UI can disambiguate.
    std::map<std::string, int> name_counts;
    for (auto& s : symbols) name_counts[s["name"].get<std::string>()]++;
    for (auto& s : symbols) s["duplicate_name"] = name_counts[s["name"].get<std::string>()] > 1;
    result["symbols"] = symbols;
  }

  if (want("file") && !q.empty() && !(route_like && !method.empty())) {
    json files = json::array();
    auto st = db_.prepare(
        exact ? "SELECT id, path, language, status, size_bytes FROM files WHERE path = ?1 OR name = ?1 ORDER BY path LIMIT ?2"
              : "SELECT id, path, language, status, size_bytes FROM files WHERE path LIKE '%' || ?1 || '%' "
                "ORDER BY (lower(name) = lower(?1)) DESC, (lower(name) LIKE lower(?1) || '%') DESC, "
                "(language IS NULL), length(path), path LIMIT ?2");
    st.bind(1, q).bind(2, limit);
    while (st.step()) {
      files.push_back({{"id", st.int64(0)},
                       {"path", st.text(1)},
                       {"language", st.is_null(2) ? json(nullptr) : json(st.text(2))},
                       {"status", st.text(3)},
                       {"size_bytes", st.int64(4)}});
    }
    result["files"] = files;
  }

  if (want("route") && (!q.empty())) {
    json routes = json::array();
    std::string qshape = route_shape(path_query);
    std::string qlower = lower(path_query);
    auto st = db_.prepare(
        "SELECT r.id, r.method, r.path, r.full_path, f.path, r.line, r.handler_status, r.handler_name, "
        "r.handler_symbol_id, r.framework, r.mount_status, r.confidence, IFNULL(h.qualified_name, ''), IFNULL(h.kind, '') "
        "FROM routes r JOIN files f ON f.id = r.file_id LEFT JOIN symbols h ON h.id = r.handler_symbol_id "
        "WHERE (?1 = '' OR r.method = ?1 OR r.method = 'ALL' OR r.method = 'ANY') ORDER BY r.full_path, r.method, r.id "
        "LIMIT 20000");
    st.bind(1, method);
    struct Hit {
      int score;
      json row;
    };
    std::vector<Hit> hits;
    while (st.step()) {
      std::string full = st.text(3);
      std::string shape = route_shape(full);
      int score = -1;
      if (path_query.empty()) score = 1;
      else if (exact) score = full == path_query ? 3 : -1;
      else if (shape == qshape || shape_matches(shape, qshape)) score = 3;
      else if (shape.rfind(qshape, 0) == 0) score = 2;
      else if (lower(full).find(qlower) != std::string::npos) score = 1;
      if (score < 0) continue;
      hits.push_back({score,
                      {{"id", st.int64(0)},
                       {"method", st.text(1)},
                       {"path", st.text(2)},
                       {"full_path", full},
                       {"file", st.text(4)},
                       {"line", st.int64(5)},
                       {"handler_status", st.text(6)},
                       {"handler_name", st.is_null(7) ? json(nullptr) : json(st.text(7))},
                       {"handler_symbol_id", st.is_null(8) ? json(nullptr) : json(st.int64(8))},
                       {"framework", st.text(9)},
                       {"mount_status", st.text(10)},
                       {"confidence", st.text(11)},
                       {"handler_qualified", st.text(12).empty() ? json(nullptr) : json(st.text(12))},
                       {"handler_symbol_kind", st.text(13).empty() ? json(nullptr) : json(st.text(13))}}});
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.score > b.score; });
    for (size_t i = 0; i < hits.size() && static_cast<int64_t>(i) < limit; ++i) routes.push_back(hits[i].row);
    result["routes"] = routes;
  }
  return result;
}

json IndexReader::symbol(int64_t id) {
  auto st = db_.prepare(
      "SELECT s.id, s.name, s.qualified_name, s.kind, s.language, f.path, s.start_line, s.end_line, s.exported, "
      "s.start_col, s.end_col, s.visibility, s.is_static, s.is_definition, s.param_count, s.container, s.parent_id, "
      "f.id FROM symbols s JOIN files f ON f.id = s.file_id WHERE s.id = ?");
  st.bind(1, id);
  if (!st.step()) return nullptr;
  json s = symbol_brief_row(st, 0);
  s["start_col"] = st.int64(9);
  s["end_col"] = st.int64(10);
  s["visibility"] = st.is_null(11) ? json(nullptr) : json(st.text(11));
  s["is_static"] = st.int64(12) != 0;
  s["is_definition"] = st.int64(13) != 0;
  s["param_count"] = st.is_null(14) ? json(nullptr) : json(st.int64(14));
  s["container"] = st.is_null(15) ? json(nullptr) : json(st.text(15));
  s["parent"] = st.is_null(16) ? json(nullptr) : symbol_brief(st.int64(16));
  s["file_id"] = st.int64(17);

  auto counts = [&](const char* sql) {
    json out = json::object();
    auto c = db_.prepare(sql);
    c.bind(1, id);
    while (c.step()) out[c.text(0)] = c.int64(1);
    return out;
  };
  s["outgoing"] = counts("SELECT status, COUNT(*) FROM edges WHERE src_symbol_id = ? GROUP BY status");
  s["incoming"] = counts("SELECT status, COUNT(*) FROM edges WHERE dst_symbol_id = ? GROUP BY status");
  auto amb = db_.prepare("SELECT COUNT(*) FROM edge_candidates WHERE symbol_id = ?");
  amb.bind(1, id);
  if (amb.step() && amb.int64(0) > 0) s["incoming"]["ambiguous_candidate"] = amb.int64(0);
  s["exception_counts"] = counts("SELECT kind, COUNT(*) FROM exceptions WHERE symbol_id = ? GROUP BY kind");

  json routes = json::array();
  auto r = db_.prepare(
      "SELECT id, method, full_path FROM routes WHERE handler_symbol_id = ? ORDER BY full_path, method LIMIT 50");
  r.bind(1, id);
  while (r.step()) routes.push_back({{"id", r.int64(0)}, {"method", r.text(1)}, {"full_path", r.text(2)}});
  s["routes"] = routes;

  json entries = json::array();
  auto e = db_.prepare("SELECT kind, reason, line FROM entry_points WHERE symbol_id = ? ORDER BY id LIMIT 20");
  e.bind(1, id);
  while (e.step()) entries.push_back({{"kind", e.text(0)}, {"reason", e.text(1)}, {"line", e.int64(2)}});
  s["entry_points"] = entries;

  json children = json::array();
  auto c = db_.prepare(std::string("SELECT ") + kSymbolBriefCols +
                       " FROM symbols s JOIN files f ON f.id = s.file_id WHERE s.parent_id = ? ORDER BY s.start_line "
                       "LIMIT 100");
  c.bind(1, id);
  while (c.step()) children.push_back(symbol_brief_row(c, 0));
  s["children"] = children;
  return s;
}

json IndexReader::edges(int64_t symbol_id, const std::string& direction, const std::vector<std::string>& kinds,
                        int64_t limit, int64_t offset) {
  limit = clamp(limit, 1, QueryLimits::kMaxResults);
  offset = std::max<int64_t>(0, offset);
  bool out = direction != "in";
  std::string kind_filter = kinds.empty() ? "" : " AND e.kind IN " + in_list(kinds);
  std::string sql;
  if (out) {
    sql = std::string("SELECT e.id, e.kind, e.status, e.line, e.col, e.target_name, e.target_text, e.receiver, e.note, ") +
          "ef.path, " + kSymbolBriefCols +
          " FROM edges e JOIN files ef ON ef.id = e.file_id LEFT JOIN symbols s ON s.id = e.dst_symbol_id "
          "LEFT JOIN files f ON f.id = s.file_id WHERE e.src_symbol_id = ?" + kind_filter +
          " ORDER BY e.line, e.col, e.id LIMIT ? OFFSET ?";
  } else {
    sql = std::string("SELECT e.id, e.kind, e.status, e.line, e.col, e.target_name, e.target_text, e.receiver, e.note, ") +
          "ef.path, " + kSymbolBriefCols +
          " FROM edges e JOIN files ef ON ef.id = e.file_id JOIN symbols s ON s.id = e.src_symbol_id "
          "JOIN files f ON f.id = s.file_id WHERE (e.dst_symbol_id = ? OR e.id IN "
          "(SELECT edge_id FROM edge_candidates WHERE symbol_id = ?1))" + kind_filter +
          " ORDER BY ef.path, e.line, e.id LIMIT ? OFFSET ?";
  }
  auto st = db_.prepare(sql);
  int i = 1;
  st.bind(i++, symbol_id);
  for (const auto& k : kinds) st.bind(i++, k);
  st.bind(i, limit + 1);
  st.bind(i + 1, offset);
  json rows = json::array();
  auto cands = db_.prepare(std::string("SELECT ") + kSymbolBriefCols +
                           " FROM edge_candidates c JOIN symbols s ON s.id = c.symbol_id JOIN files f ON f.id = s.file_id "
                           "WHERE c.edge_id = ? ORDER BY s.id LIMIT 8");
  bool more = false;
  while (st.step()) {
    if (static_cast<int64_t>(rows.size()) >= limit) {
      more = true;
      break;
    }
    json row = {{"edge_id", st.int64(0)},
                {"kind", st.text(1)},
                {"status", st.text(2)},
                {"line", st.int64(3)},
                {"col", st.int64(4)},
                {"target_name", st.is_null(5) ? json(nullptr) : json(st.text(5))},
                {"target_text", st.is_null(6) ? json(nullptr) : json(st.text(6))},
                {"receiver", st.is_null(7) ? json(nullptr) : json(st.text(7))},
                {"note", st.is_null(8) ? json(nullptr) : json(st.text(8))},
                {"site_path", st.text(9)}};
    row[out ? "target" : "source"] = st.is_null(10) ? json(nullptr) : symbol_brief_row(st, 10);
    if (out && row["status"] == "ambiguous") {
      json cs = json::array();
      cands.bind(1, st.int64(0));
      while (cands.step()) cs.push_back(symbol_brief_row(cands, 0));
      cands.reset();
      row["candidates"] = cs;
    }
    rows.push_back(std::move(row));
  }
  return {{"symbol", symbol_brief(symbol_id)},
          {"direction", out ? "out" : "in"},
          {"edges", rows},
          {"offset", offset},
          {"limit", limit},
          {"has_more", more}};
}

json IndexReader::traverse(int64_t root, const TraverseOptions& opt) {
  bool forward = opt.direction != "backward";
  int max_depth = static_cast<int>(clamp(opt.max_depth, 0, QueryLimits::kMaxDepth));
  int64_t max_nodes = clamp(opt.max_nodes, 1, QueryLimits::kMaxNodes);
  int64_t max_children = clamp(opt.max_children, 1, QueryLimits::kMaxChildrenPerNode);
  const auto& kinds = opt.edge_kinds.empty() ? kDefaultTraverseKinds : opt.edge_kinds;

  json root_sym = symbol_brief(root);
  if (root_sym.is_null()) return nullptr;
  std::string sql;
  if (forward) {
    sql = std::string("SELECT e.kind, e.status, e.line, e.col, ef.path, e.target_name, e.target_text, e.note, e.id, ") +
          kSymbolBriefCols +
          " FROM edges e JOIN files ef ON ef.id = e.file_id LEFT JOIN symbols s ON s.id = e.dst_symbol_id "
          "LEFT JOIN files f ON f.id = s.file_id WHERE e.src_symbol_id = ?1 AND e.kind IN " + in_list(kinds) +
          " ORDER BY e.line, e.col, e.id LIMIT ?";
  } else {
    sql = std::string("SELECT e.kind, e.status, e.line, e.col, ef.path, e.target_name, e.target_text, e.note, e.id, ") +
          kSymbolBriefCols +
          " FROM edges e JOIN files ef ON ef.id = e.file_id JOIN symbols s ON s.id = e.src_symbol_id "
          "JOIN files f ON f.id = s.file_id WHERE (e.dst_symbol_id = ?1 OR e.id IN "
          "(SELECT edge_id FROM edge_candidates WHERE symbol_id = ?1)) AND e.kind IN " + in_list(kinds) +
          " ORDER BY ef.path, e.line, e.id LIMIT ?";
  }
  auto st = db_.prepare(sql);
  auto cands = db_.prepare(std::string("SELECT ") + kSymbolBriefCols +
                           " FROM edge_candidates c JOIN symbols s ON s.id = c.symbol_id JOIN files f ON f.id = s.file_id "
                           "WHERE c.edge_id = ? ORDER BY s.id LIMIT 5");

  json nodes = json::array();
  nodes.push_back({{"index", 0}, {"parent", nullptr}, {"depth", 0}, {"symbol", root_sym}, {"edge", nullptr}});
  std::unordered_set<int64_t> visited{root};
  std::deque<size_t> queue{0};
  bool truncated = false;
  int64_t hidden_unresolved = 0;
  int64_t fetch_limit = max_children * 4 + 1;

  while (!queue.empty()) {
    size_t idx = queue.front();
    queue.pop_front();
    int depth = nodes[idx]["depth"].get<int>();
    if (depth >= max_depth) continue;
    int64_t sym_id = nodes[idx]["symbol"]["id"].get<int64_t>();
    st.bind(1, sym_id);
    int i = 2;
    for (const auto& k : kinds) st.bind(i++, k);
    st.bind(i, fetch_limit);
    // Group parallel edges (same target) so each callee appears once with a site count.
    struct Group {
      json edge;
      json symbol;
      int64_t sites = 0;
      int64_t edge_id = 0;
    };
    std::vector<Group> groups;
    std::map<std::string, size_t> by_key;
    int64_t fetched = 0;
    while (st.step()) {
      ++fetched;
      std::string status = st.text(1);
      if (!opt.include_unresolved && (status == "unresolved" || status == "external")) {
        ++hidden_unresolved;
        continue;
      }
      std::string key = st.is_null(9) ? st.text(0) + "|" + status + "|" + st.text(6) : st.text(0) + "|" + std::to_string(st.int64(9));
      if (auto it = by_key.find(key); it != by_key.end()) {
        groups[it->second].sites++;
        continue;
      }
      Group g;
      g.edge = {{"kind", st.text(0)},
                {"status", status},
                {"line", st.int64(2)},
                {"col", st.int64(3)},
                {"path", st.text(4)},
                {"target_name", st.is_null(5) ? json(nullptr) : json(st.text(5))},
                {"target_text", st.is_null(6) ? json(nullptr) : json(st.text(6))},
                {"note", st.is_null(7) ? json(nullptr) : json(st.text(7))}};
      g.edge_id = st.int64(8);
      g.symbol = st.is_null(9) ? json(nullptr) : symbol_brief_row(st, 9);
      g.sites = 1;
      by_key[key] = groups.size();
      groups.push_back(std::move(g));
    }
    st.reset();
    int64_t children = 0;
    for (auto& g : groups) {
      if (children >= max_children) {
        nodes[idx]["truncated_children"] = static_cast<int64_t>(groups.size()) - children;
        truncated = true;
        break;
      }
      if (static_cast<int64_t>(nodes.size()) >= max_nodes) {
        truncated = true;
        break;
      }
      g.edge["sites"] = g.sites;
      json node = {{"index", nodes.size()}, {"parent", idx}, {"depth", depth + 1}, {"edge", g.edge}, {"symbol", g.symbol}};
      if (forward && g.edge["status"] == "ambiguous") {
        json cs = json::array();
        cands.bind(1, g.edge_id);
        while (cands.step()) cs.push_back(symbol_brief_row(cands, 0));
        cands.reset();
        node["candidates"] = cs;
      }
      bool expand = false;
      if (!g.symbol.is_null()) {
        int64_t target = g.symbol["id"].get<int64_t>();
        if (visited.count(target)) node["seen"] = true;  // cycle or shared callee: shown, not expanded again
        else {
          visited.insert(target);
          expand = true;
        }
      }
      nodes.push_back(std::move(node));
      if (expand) queue.push_back(nodes.size() - 1);
      ++children;
    }
    if (fetched >= fetch_limit) {
      nodes[idx]["truncated_children"] = nodes[idx].value("truncated_children", int64_t{0}) + 1;
      truncated = true;
    }
    if (static_cast<int64_t>(nodes.size()) >= max_nodes) {
      truncated = truncated || !queue.empty();
      break;
    }
  }
  return {{"root", root_sym},
          {"direction", forward ? "forward" : "backward"},
          {"max_depth", max_depth},
          {"max_nodes", max_nodes},
          {"edge_kinds", kinds},
          {"nodes", nodes},
          {"truncated", truncated},
          {"hidden_unresolved", hidden_unresolved}};
}

json IndexReader::file(std::optional<int64_t> id, const std::string& path, int64_t symbol_limit) {
  symbol_limit = clamp(symbol_limit, 1, QueryLimits::kMaxResults);
  auto st = db_.prepare(
      id ? "SELECT id, path, language, status, size_bytes, fingerprint, error_count, first_error_line, line_count, "
           "module_name, note, mtime_s FROM files WHERE id = ?"
         : "SELECT id, path, language, status, size_bytes, fingerprint, error_count, first_error_line, line_count, "
           "module_name, note, mtime_s FROM files WHERE path = ?");
  if (id) st.bind(1, *id); else st.bind(1, path);
  if (!st.step()) return nullptr;
  int64_t fid = st.int64(0);
  json f = {{"id", fid},
            {"path", st.text(1)},
            {"language", st.is_null(2) ? json(nullptr) : json(st.text(2))},
            {"status", st.text(3)},
            {"size_bytes", st.int64(4)},
            {"fingerprint", st.is_null(5) ? json(nullptr) : json(st.text(5))},
            {"error_count", st.int64(6)},
            {"first_error_line", st.is_null(7) ? json(nullptr) : json(st.int64(7))},
            {"line_count", st.is_null(8) ? json(nullptr) : json(st.int64(8))},
            {"module_name", st.is_null(9) ? json(nullptr) : json(st.text(9))},
            {"note", st.is_null(10) ? json(nullptr) : json(st.text(10))},
            {"mtime_s", st.is_null(11) ? json(nullptr) : json(st.int64(11))}};
  json symbols = json::array();
  auto s = db_.prepare(std::string("SELECT ") + kSymbolBriefCols +
                       ", s.parent_id FROM symbols s JOIN files f ON f.id = s.file_id WHERE s.file_id = ? "
                       "AND s.kind != 'module' ORDER BY s.start_line, s.id LIMIT ?");
  s.bind(1, fid).bind(2, symbol_limit + 1);
  bool more = false;
  while (s.step()) {
    if (static_cast<int64_t>(symbols.size()) >= symbol_limit) {
      more = true;
      break;
    }
    json row = symbol_brief_row(s, 0);
    row["parent_id"] = s.is_null(9) ? json(nullptr) : json(s.int64(9));
    symbols.push_back(std::move(row));
  }
  f["symbols"] = symbols;
  f["symbols_truncated"] = more;
  auto mod = db_.prepare("SELECT MIN(id) FROM symbols WHERE file_id = ?");
  mod.bind(1, fid);
  f["module_symbol_id"] = mod.step() && !mod.is_null(0) ? json(mod.int64(0)) : json(nullptr);
  auto c = db_.prepare(
      "SELECT (SELECT COUNT(*) FROM file_deps WHERE src_file_id = ?1), (SELECT COUNT(*) FROM file_deps WHERE dst_file_id = ?1), "
      "(SELECT COUNT(*) FROM routes WHERE file_id = ?1), (SELECT COUNT(*) FROM exceptions WHERE file_id = ?1 AND kind IN ('throw','rethrow'))");
  c.bind(1, fid);
  if (c.step()) {
    f["dependency_count"] = c.int64(0);
    f["dependent_count"] = c.int64(1);
    f["route_count"] = c.int64(2);
    f["throw_count"] = c.int64(3);
  }
  return f;
}

json IndexReader::file_deps(int64_t file_id, const std::string& direction, int64_t limit, int64_t offset) {
  limit = clamp(limit, 1, QueryLimits::kMaxResults);
  offset = std::max<int64_t>(0, offset);
  bool out = direction != "in";
  auto st = db_.prepare(
      out ? "SELECT d.id, d.kind, d.specifier, d.line, d.col, d.status, d.note, f.id, f.path, f.language FROM file_deps d "
            "LEFT JOIN files f ON f.id = d.dst_file_id WHERE d.src_file_id = ? ORDER BY d.line, d.id LIMIT ? OFFSET ?"
          : "SELECT d.id, d.kind, d.specifier, d.line, d.col, d.status, d.note, f.id, f.path, f.language FROM file_deps d "
            "JOIN files f ON f.id = d.src_file_id WHERE d.dst_file_id = ? ORDER BY f.path, d.line LIMIT ? OFFSET ?");
  st.bind(1, file_id).bind(2, limit + 1).bind(3, offset);
  json rows = json::array();
  bool more = false;
  while (st.step()) {
    if (static_cast<int64_t>(rows.size()) >= limit) {
      more = true;
      break;
    }
    json row = {{"dep_id", st.int64(0)},
                {"kind", st.text(1)},
                {"specifier", st.text(2)},
                {"line", st.int64(3)},
                {"col", st.int64(4)},
                {"status", st.text(5)},
                {"note", st.is_null(6) ? json(nullptr) : json(st.text(6))}};
    json other = st.is_null(7) ? json(nullptr)
                               : json{{"id", st.int64(7)},
                                      {"path", st.text(8)},
                                      {"language", st.is_null(9) ? json(nullptr) : json(st.text(9))}};
    row[out ? "target" : "source"] = other;
    rows.push_back(std::move(row));
  }
  auto fp = db_.prepare("SELECT path FROM files WHERE id = ?");
  fp.bind(1, file_id);
  return {{"file", fp.step() ? json({{"id", file_id}, {"path", fp.text(0)}}) : json(nullptr)},
          {"direction", out ? "out" : "in"},
          {"dependencies", rows},
          {"offset", offset},
          {"limit", limit},
          {"has_more", more}};
}

json IndexReader::entry_points(int64_t limit) {
  limit = clamp(limit, 1, QueryLimits::kMaxResults);
  auto st = db_.prepare(
      "SELECT e.id, e.kind, e.reason, e.line, f.path, e.symbol_id FROM entry_points e JOIN files f ON f.id = e.file_id "
      "ORDER BY CASE e.kind WHEN 'main_function' THEN 0 WHEN 'main_guard' THEN 1 WHEN 'package_main' THEN 2 "
      "WHEN 'package_bin' THEN 3 WHEN 'package_script' THEN 4 WHEN 'module_main' THEN 5 WHEN 'framework_app' THEN 6 "
      "WHEN 'server_start' THEN 7 ELSE 8 END, f.path, e.line LIMIT ?");
  st.bind(1, limit);
  json rows = json::array();
  std::vector<std::pair<size_t, int64_t>> syms;
  while (st.step()) {
    rows.push_back({{"id", st.int64(0)}, {"kind", st.text(1)}, {"reason", st.text(2)}, {"line", st.int64(3)}, {"path", st.text(4)}});
    if (!st.is_null(5)) syms.emplace_back(rows.size() - 1, st.int64(5));
  }
  for (auto& [i, sid] : syms) rows[i]["symbol"] = symbol_brief(sid);
  json result = {{"entry_points", rows}};
  if (rows.empty()) {
    result["message"] =
        "no entry points were recognized; supported patterns are listed in docs/LANGUAGE_SUPPORT.md";
  }
  return result;
}

json IndexReader::routes(int64_t limit, int64_t offset) {
  limit = clamp(limit, 1, QueryLimits::kMaxResults);
  auto st = db_.prepare(
      "SELECT r.id, r.method, r.full_path, r.framework, f.path, r.line, r.handler_status, r.handler_name, r.mount_status, "
      "r.confidence, h.id, h.name, h.qualified_name, h.kind FROM routes r JOIN files f ON f.id = r.file_id "
      "LEFT JOIN symbols h ON h.id = r.handler_symbol_id ORDER BY r.full_path, r.method, r.id LIMIT ? OFFSET ?");
  st.bind(1, limit + 1).bind(2, std::max<int64_t>(0, offset));
  json rows = json::array();
  bool more = false;
  while (st.step()) {
    if (static_cast<int64_t>(rows.size()) >= limit) {
      more = true;
      break;
    }
    rows.push_back({{"id", st.int64(0)},
                    {"method", st.text(1)},
                    {"full_path", st.text(2)},
                    {"framework", st.text(3)},
                    {"file", st.text(4)},
                    {"line", st.int64(5)},
                    {"handler_status", st.text(6)},
                    {"handler_name", st.is_null(7) ? json(nullptr) : json(st.text(7))},
                    {"mount_status", st.text(8)},
                    {"confidence", st.text(9)},
                    {"handler", st.is_null(10) ? json(nullptr)
                                               : json{{"id", st.int64(10)}, {"name", st.text(11)},
                                                      {"qualified_name", st.text(12)}, {"kind", st.text(13)}}}});
  }
  return {{"routes", rows}, {"has_more", more}, {"offset", offset}, {"limit", limit}};
}

json IndexReader::route_flow(int64_t route_id, const TraverseOptions& options) {
  auto st = db_.prepare(
      "SELECT r.id, r.framework, r.method, r.path, r.full_path, r.mount_prefix, r.mount_status, r.mount_id, f.path, r.line, "
      "r.col, r.registrar, r.scope_symbol_id, r.handler_kind, r.handler_name, r.handler_text, r.handler_symbol_id, "
      "r.handler_status, r.handler_note, r.confidence FROM routes r JOIN files f ON f.id = r.file_id WHERE r.id = ?");
  st.bind(1, route_id);
  if (!st.step()) return nullptr;
  json route = {{"id", st.int64(0)},
                {"framework", st.text(1)},
                {"method", st.text(2)},
                {"path", st.text(3)},
                {"full_path", st.text(4)},
                {"mount_prefix", st.is_null(5) ? json(nullptr) : json(st.text(5))},
                {"mount_status", st.text(6)},
                {"registration", {{"path", st.text(8)}, {"line", st.int64(9)}, {"col", st.int64(10)}, {"registrar", st.text(11)}}},
                {"handler",
                 {{"kind", st.is_null(13) ? json(nullptr) : json(st.text(13))},
                  {"name", st.is_null(14) ? json(nullptr) : json(st.text(14))},
                  {"text", st.is_null(15) ? json(nullptr) : json(st.text(15))},
                  {"status", st.text(17)},
                  {"note", st.is_null(18) ? json(nullptr) : json(st.text(18))}}},
                {"confidence", st.text(19)}};
  std::optional<int64_t> mount_id = st.opt_int64(7);
  std::optional<int64_t> scope = st.opt_int64(12);
  std::optional<int64_t> handler = st.opt_int64(16);
  route["registration"]["symbol"] = scope ? symbol_brief(*scope) : json(nullptr);
  route["handler"]["symbol"] = handler ? symbol_brief(*handler) : json(nullptr);
  if (mount_id) {
    auto m = db_.prepare(
        "SELECT f.path, m.line, m.registrar, m.prefix, COALESCE(m.target_text, m.target_name) FROM mounts m "
        "JOIN files f ON f.id = m.file_id WHERE m.id = ?");
    m.bind(1, *mount_id);
    if (m.step()) {
      route["mount"] = {{"path", m.text(0)},
                        {"line", m.int64(1)},
                        {"registrar", m.text(2)},
                        {"prefix", m.is_null(3) ? json(nullptr) : json(m.text(3))},
                        {"target", m.text(4)}};
    }
  }
  json middleware = json::array();
  auto mw = db_.prepare(
      "SELECT m.scope, m.position, m.ref_kind, m.name, m.text, m.status, f.path, m.line, m.symbol_id FROM route_middleware m "
      "JOIN files f ON f.id = m.file_id WHERE m.route_id = ? ORDER BY CASE m.scope WHEN 'mount' THEN 0 WHEN 'app' THEN 1 "
      "WHEN 'router' THEN 2 ELSE 3 END, m.position, m.id");
  mw.bind(1, route_id);
  std::vector<std::pair<size_t, int64_t>> mw_syms;
  while (mw.step()) {
    middleware.push_back({{"scope", mw.text(0)},
                          {"kind", mw.text(2)},
                          {"name", mw.is_null(3) ? json(nullptr) : json(mw.text(3))},
                          {"text", mw.text(4)},
                          {"status", mw.text(5)},
                          {"path", mw.text(6)},
                          {"line", mw.int64(7)}});
    if (!mw.is_null(8)) mw_syms.emplace_back(middleware.size() - 1, mw.int64(8));
  }
  for (auto& [i, sid] : mw_syms) middleware[i]["symbol"] = symbol_brief(sid);
  route["middleware"] = middleware;

  json result = {{"route", route}};
  if (handler) {
    result["flow"] = traverse(*handler, options);
    result["exceptions"] = exceptions(*handler, options.max_depth, options.max_nodes);
  } else {
    result["flow"] = nullptr;
    result["exceptions"] = nullptr;
    result["message"] = "the handler could not be resolved statically, so no call flow is available";
  }
  if (scope) result["startup"] = startup_path(*scope, 10);
  return result;
}

json IndexReader::exceptions(int64_t symbol_id, int depth, int64_t max_nodes) {
  auto own = db_.prepare(
      "SELECT x.id, x.kind, x.type_name, x.expression, f.path, x.line, x.col, h.line, h.type_name, x.handler_match "
      "FROM exceptions x JOIN files f ON f.id = x.file_id LEFT JOIN exceptions h ON h.id = x.handler_id "
      "WHERE x.symbol_id = ? ORDER BY x.line, x.col LIMIT 200");
  auto row = [&](db::Statement& s) {
    json r = {{"kind", s.text(1)},
              {"type", s.is_null(2) ? json(nullptr) : json(s.text(2))},
              {"expression", s.is_null(3) ? json(nullptr) : json(s.text(3))},
              {"path", s.text(4)},
              {"line", s.int64(5)},
              {"col", s.int64(6)}};
    if (!s.is_null(7)) {
      r["handled_locally"] = {{"catch_line", s.int64(7)},
                              {"catch_type", s.is_null(8) ? json(nullptr) : json(s.text(8))},
                              {"match", s.is_null(9) ? json(nullptr) : json(s.text(9))}};
    }
    return r;
  };
  json explicit_sites = json::array(), handlers = json::array(), declared = json::array();
  own.bind(1, symbol_id);
  while (own.step()) {
    std::string kind = own.text(1);
    if (kind == "catch") handlers.push_back(row(own));
    else if (kind == "declares") declared.push_back(row(own));
    else explicit_sites.push_back(row(own));
  }
  // Throws in functions reachable through resolved call edges. These are
  // possible exceptions, not proof that the call path raises them.
  json in_callees = json::array();
  if (depth > 0) {
    TraverseOptions t;
    t.max_depth = std::min(depth, 6);
    t.max_nodes = std::min<int64_t>(max_nodes, 300);
    t.edge_kinds = {"calls", "instantiates", "overridden_by"};
    t.include_unresolved = false;
    json tree = traverse(symbol_id, t);
    auto sites = db_.prepare(
        "SELECT x.id, x.kind, x.type_name, x.expression, f.path, x.line, x.col, h.line, h.type_name, x.handler_match "
        "FROM exceptions x JOIN files f ON f.id = x.file_id LEFT JOIN exceptions h ON h.id = x.handler_id "
        "WHERE x.symbol_id = ? AND x.kind IN ('throw','rethrow','declares') ORDER BY x.line LIMIT 50");
    if (tree.is_object()) {
      for (const auto& n : tree["nodes"]) {
        if (n["depth"].get<int>() == 0 || n["symbol"].is_null() || n.value("seen", false)) continue;
        sites.bind(1, n["symbol"]["id"].get<int64_t>());
        while (sites.step()) {
          json r = row(sites);
          r["in_symbol"] = n["symbol"];
          r["call_depth"] = n["depth"];
          r["edge_status"] = n["edge"]["status"];
          r["certainty"] = "possible";
          in_callees.push_back(std::move(r));
        }
        sites.reset();
        if (in_callees.size() >= 200) break;
      }
    }
  }
  return {{"symbol", symbol_brief(symbol_id)},
          {"explicit", explicit_sites},
          {"handlers", handlers},
          {"declared", declared},
          {"in_callees", in_callees},
          {"note",
           "explicit sites are throw/raise statements in this symbol; in_callees lists explicit sites in functions "
           "reachable through statically resolved calls and may not occur at runtime"}};
}

json IndexReader::startup_path(int64_t symbol_id, int max_depth) {
  // Backward BFS from a symbol to the nearest recognized entry point.
  auto is_entry = db_.prepare(
      "SELECT kind, reason FROM entry_points WHERE symbol_id = ? ORDER BY CASE kind WHEN 'main_function' THEN 0 "
      "WHEN 'main_guard' THEN 1 WHEN 'package_main' THEN 2 WHEN 'package_bin' THEN 3 WHEN 'package_script' THEN 4 "
      "ELSE 5 END LIMIT 1");
  auto preds = db_.prepare(
      "SELECT e.src_symbol_id, e.kind, e.line, ef.path FROM edges e JOIN files ef ON ef.id = e.file_id "
      "WHERE e.dst_symbol_id = ? AND e.kind IN ('calls','instantiates','mounts','loads_module','registers_route','configures') "
      "AND e.status IN ('resolved','heuristic') ORDER BY CASE e.kind WHEN 'mounts' THEN 0 WHEN 'registers_route' THEN 1 "
      "WHEN 'configures' THEN 2 WHEN 'calls' THEN 3 WHEN 'instantiates' THEN 4 ELSE 5 END, e.id LIMIT 50");
  struct Node {
    int64_t sym;
    int parent;
    std::string kind, path;
    int64_t line;
    int depth;
  };
  std::vector<Node> nodes{{symbol_id, -1, "", "", 0, 0}};
  std::unordered_set<int64_t> seen{symbol_id};
  for (size_t i = 0; i < nodes.size() && nodes.size() < 2000; ++i) {
    is_entry.bind(1, nodes[i].sym);
    bool entry = is_entry.step();
    std::string ekind = entry ? is_entry.text(0) : "", reason = entry ? is_entry.text(1) : "";
    is_entry.reset();
    if (entry) {
      json path = json::array();
      for (int j = static_cast<int>(i); j >= 0; j = nodes[static_cast<size_t>(j)].parent) {
        const auto& n = nodes[static_cast<size_t>(j)];
        json step = {{"symbol", symbol_brief(n.sym)}};
        if (n.parent >= 0) step["edge"] = {{"kind", n.kind}, {"path", n.path}, {"line", n.line}};
        path.push_back(std::move(step));
      }
      return {{"found", true}, {"entry_kind", ekind}, {"entry_reason", reason}, {"path", path}};
    }
    if (nodes[i].depth >= max_depth) continue;
    preds.bind(1, nodes[i].sym);
    std::vector<Node> next;
    while (preds.step()) {
      int64_t src = preds.int64(0);
      if (seen.insert(src).second) {
        next.push_back({src, static_cast<int>(i), preds.text(1), preds.text(3), preds.int64(2), nodes[i].depth + 1});
      }
    }
    preds.reset();
    for (auto& n : next) nodes.push_back(std::move(n));
  }
  return {{"found", false},
          {"message", "no recognized entry point reaches this code through statically resolved edges"}};
}

json IndexReader::check() {
  json checks = json::array();
  bool ok = true;
  auto add = [&](const std::string& name, int64_t problems, const std::string& detail) {
    if (problems != 0) ok = false;
    checks.push_back({{"check", name}, {"problems", problems}, {"detail", detail}});
  };
  auto qc = db_.prepare("PRAGMA quick_check");
  std::string quick = qc.step() ? qc.text(0) : "no result";
  add("sqlite_quick_check", quick == "ok" ? 0 : 1, quick);
  auto count = [&](const char* sql) { return db_.query_int(sql).value_or(0); };
  add("symbols_without_file", count("SELECT COUNT(*) FROM symbols s LEFT JOIN files f ON f.id = s.file_id WHERE f.id IS NULL"),
      "symbols referencing a missing file");
  add("symbols_with_missing_parent",
      count("SELECT COUNT(*) FROM symbols s LEFT JOIN symbols p ON p.id = s.parent_id WHERE s.parent_id IS NOT NULL AND p.id IS NULL"),
      "symbols whose parent symbol is missing");
  add("edges_with_missing_source",
      count("SELECT COUNT(*) FROM edges e LEFT JOIN symbols s ON s.id = e.src_symbol_id WHERE s.id IS NULL"),
      "edges whose source symbol is missing");
  add("edges_with_missing_target",
      count("SELECT COUNT(*) FROM edges e LEFT JOIN symbols s ON s.id = e.dst_symbol_id WHERE e.dst_symbol_id IS NOT NULL AND s.id IS NULL"),
      "edges whose target symbol is missing");
  add("edge_candidates_dangling",
      count("SELECT COUNT(*) FROM edge_candidates c LEFT JOIN symbols s ON s.id = c.symbol_id LEFT JOIN edges e ON e.id = c.edge_id "
            "WHERE s.id IS NULL OR e.id IS NULL"),
      "ambiguity candidates referencing missing rows");
  add("file_deps_dangling",
      count("SELECT COUNT(*) FROM file_deps d LEFT JOIN files f ON f.id = d.dst_file_id WHERE d.dst_file_id IS NOT NULL AND f.id IS NULL"),
      "dependencies whose target file is missing");
  add("routes_dangling_handler",
      count("SELECT COUNT(*) FROM routes r LEFT JOIN symbols s ON s.id = r.handler_symbol_id WHERE r.handler_symbol_id IS NOT NULL AND s.id IS NULL"),
      "routes whose handler symbol is missing");
  add("exceptions_dangling",
      count("SELECT COUNT(*) FROM exceptions x LEFT JOIN symbols s ON s.id = x.symbol_id WHERE x.symbol_id IS NOT NULL AND s.id IS NULL"),
      "exception sites whose symbol is missing");
  add("entry_points_dangling",
      count("SELECT COUNT(*) FROM entry_points e LEFT JOIN symbols s ON s.id = e.symbol_id WHERE e.symbol_id IS NOT NULL AND s.id IS NULL"),
      "entry points whose symbol is missing");
  add("pending_relationships",
      count("SELECT (SELECT COUNT(*) FROM edges WHERE status='pending') + (SELECT COUNT(*) FROM file_deps WHERE status='pending')"),
      "relationships never processed by the resolver");
  add("fts_row_count",
      count("SELECT (SELECT COUNT(*) FROM symbols) - (SELECT COUNT(*) FROM symbols_fts)"),
      "difference between symbols and the search index");
  return {{"ok", ok}, {"schema_version", meta("schema_version")}, {"checks", checks}};
}

json IndexReader::snippet(int64_t file_id, int64_t line, int before, int after) {
  before = static_cast<int>(clamp(before, 0, QueryLimits::kMaxSnippetLines / 2));
  after = static_cast<int>(clamp(after, 0, QueryLimits::kMaxSnippetLines / 2));
  auto st = db_.prepare("SELECT path, fingerprint, size_bytes FROM files WHERE id = ?");
  st.bind(1, file_id);
  if (!st.step()) return nullptr;
  std::string rel = st.text(0);
  std::string indexed_fp = st.is_null(1) ? "" : st.text(1);
  fs::path root = meta("root_path");
  std::error_code ec;
  fs::path abs = root / rel;
  // Open only regular files that are still inside the project root.
  auto canon_root = fs::canonical(root, ec);
  auto canon = fs::canonical(abs, ec);
  json result = {{"path", rel}, {"line", line}};
  if (ec || !paths::relative_to_root(canon_root, canon) || fs::is_symlink(fs::symlink_status(abs, ec)) ||
      !fs::is_regular_file(canon, ec)) {
    result["error"] = "file is not available inside the project root";
    return result;
  }
  if (fs::file_size(canon, ec) > 8 * 1024 * 1024) {
    result["error"] = "file is too large to display";
    return result;
  }
  std::ifstream in(canon, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  std::string content = ss.str();
  Fnv1a64 h;
  h.update(content);
  result["changed_since_index"] = !indexed_fp.empty() && h.hex() != indexed_fp;
  json lines = json::array();
  int64_t first = std::max<int64_t>(1, line - before), last = line + after, n = 0;
  std::stringstream ls(content);
  for (std::string l; std::getline(ls, l);) {
    ++n;
    if (n < first) continue;
    if (n > last) break;
    if (!l.empty() && l.back() == '\r') l.pop_back();
    if (l.size() > 400) l = l.substr(0, 400);
    lines.push_back({{"number", n}, {"text", l}});
  }
  result["lines"] = lines;
  return result;
}

}  // namespace proj
