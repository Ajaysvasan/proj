// Code map: the project's main parts and how they call each other.
//
// Files are grouped by directory. Starting from the focus directory, the
// largest groups are split into their subdirectories as long as the number of
// groups stays within `max_nodes`; chains of single subdirectories
// (src/main/java/com/acme) collapse into one group. Calls, route
// registrations, configuration wiring, mounts and module loads between files
// are summed per pair of groups. Everything is streamed from the index with
// memory proportional to the number of files, not edges.
#include <algorithm>
#include <cstdint>
#include <map>
#include <unordered_map>

#include "query/query.hpp"

namespace proj {
namespace {

struct Dir {
  std::string name;
  std::string path;  // project-relative
  int parent = -1;
  std::map<std::string, int> children;
  int64_t files_total = 0;
  int64_t files_direct = 0;
};

struct Group {
  int dir = -1;
  enum class Kind { Subtree, Files, Other, Outside } kind = Kind::Subtree;
  std::vector<int> other_dirs;  // Other: the directories grouped together
};

std::string relative_to(const std::string& path, const std::string& base) {
  if (base.empty()) return path;
  if (path == base) return "";
  return path.substr(base.size() + 1);
}

// The last two path segments: src/main/java/com/acme/legacy/web -> legacy/web
std::string short_label(const std::string& rel) {
  if (rel.empty()) return "(root)";
  auto last = rel.rfind('/');
  if (last == std::string::npos) return rel.size() <= 24 ? rel : rel.substr(0, 21) + "...";
  auto prev = rel.rfind('/', last - 1);
  std::string tail = prev == std::string::npos ? rel : rel.substr(prev + 1);
  if (tail.size() <= 24) return tail;
  std::string leaf = rel.substr(last + 1);
  return leaf.size() <= 24 ? leaf : leaf.substr(0, 21) + "...";
}

}  // namespace

json IndexReader::overview(const std::string& raw_focus, int max_nodes, bool direct_files) {
  max_nodes = std::max(2, std::min(max_nodes, 40));
  std::string focus = raw_focus;
  while (!focus.empty() && focus.back() == '/') focus.pop_back();
  while (!focus.empty() && focus.front() == '/') focus.erase(focus.begin());

  // Directory tree of the analyzed files under the focus.
  std::vector<Dir> dirs;
  dirs.push_back({"", focus, -1, {}, 0, 0});
  std::unordered_map<int64_t, int> file_dir;  // file id -> directory index (files under the focus)
  std::unordered_map<int64_t, std::string> file_lang;
  int64_t max_file_id = 0;
  {
    auto st = db_.prepare(
        "SELECT id, path, IFNULL(language, '') FROM files WHERE status IN ('parsed','syntax_errors') ORDER BY path");
    while (st.step()) {
      int64_t id = st.int64(0);
      std::string path = st.text(1);
      max_file_id = std::max(max_file_id, id);
      if (!focus.empty() && path.rfind(focus + "/", 0) != 0) continue;
      std::string rel = relative_to(path, focus);
      int d = 0;
      size_t start = 0;
      while (true) {
        size_t slash = rel.find('/', start);
        if (slash == std::string::npos) break;
        std::string part = rel.substr(start, slash - start);
        auto it = dirs[static_cast<size_t>(d)].children.find(part);
        if (it == dirs[static_cast<size_t>(d)].children.end()) {
          Dir child;
          child.name = part;
          child.path = (dirs[static_cast<size_t>(d)].path.empty() ? "" : dirs[static_cast<size_t>(d)].path + "/") + part;
          child.parent = d;
          dirs.push_back(child);
          int idx = static_cast<int>(dirs.size()) - 1;
          dirs[static_cast<size_t>(d)].children[part] = idx;
          d = idx;
        } else {
          d = it->second;
        }
        start = slash + 1;
      }
      file_dir[id] = d;
      file_lang[id] = st.text(2);
      ++dirs[static_cast<size_t>(d)].files_direct;
      for (int p = d; p >= 0; p = dirs[static_cast<size_t>(p)].parent) ++dirs[static_cast<size_t>(p)].files_total;
    }
  }

  if (direct_files) {
    // Only the files directly in the focus directory (a "files" group of a code map).
    json files = json::array();
    auto st = db_.prepare(
        "SELECT f.id, f.path, IFNULL(f.language, ''), (SELECT COUNT(*) FROM symbols s WHERE s.file_id = f.id AND "
        "s.kind != 'module') FROM files f WHERE f.status IN ('parsed','syntax_errors') AND f.dir = ? ORDER BY f.path LIMIT 500");
    st.bind(1, focus);
    while (st.step()) {
      files.push_back({{"id", st.int64(0)}, {"path", st.text(1)}, {"language", st.text(2)}, {"symbols", st.int64(3)}});
    }
    return {{"focus", focus}, {"direct_files", true}, {"nodes", json::array()}, {"edges", json::array()}, {"files", files},
            {"total_files", static_cast<int64_t>(files.size())}};
  }
  json result = {{"focus", focus},
                 {"parent", focus.empty() ? json(nullptr) : json(focus.find('/') == std::string::npos ? "" : focus.substr(0, focus.rfind('/')))},
                 {"total_files", dirs[0].files_total}};
  if (dirs[0].files_total == 0) {
    result["nodes"] = json::array();
    result["edges"] = json::array();
    result["message"] = focus.empty() ? "no analyzed files" : "no analyzed files under " + focus;
    return result;
  }

  auto collapse = [&](int d) {
    while (dirs[static_cast<size_t>(d)].files_direct == 0 && dirs[static_cast<size_t>(d)].children.size() == 1) {
      d = dirs[static_cast<size_t>(d)].children.begin()->second;
    }
    return d;
  };
  auto parts_of = [&](int d) {
    return static_cast<int>(dirs[static_cast<size_t>(d)].children.size()) + (dirs[static_cast<size_t>(d)].files_direct > 0 ? 1 : 0);
  };
  auto expand = [&](int d, std::vector<Group>& out, size_t keep) {
    std::vector<int> kids;
    for (const auto& [name, idx] : dirs[static_cast<size_t>(d)].children) kids.push_back(idx);
    std::stable_sort(kids.begin(), kids.end(), [&](int a, int b) {
      return dirs[static_cast<size_t>(a)].files_total > dirs[static_cast<size_t>(b)].files_total;
    });
    Group other;
    other.kind = Group::Kind::Other;
    for (size_t i = 0; i < kids.size(); ++i) {
      if (i < keep) out.push_back({collapse(kids[i]), Group::Kind::Subtree, {}});
      else other.other_dirs.push_back(kids[i]);
    }
    if (dirs[static_cast<size_t>(d)].files_direct > 0) out.push_back({d, Group::Kind::Files, {}});
    if (!other.other_dirs.empty()) {
      other.dir = d;
      out.push_back(other);
    }
  };

  // Choose the groups.
  std::vector<Group> groups;
  int root = collapse(0);
  if (parts_of(root) <= 1) {
    groups.push_back({root, dirs[static_cast<size_t>(root)].children.empty() ? Group::Kind::Files : Group::Kind::Subtree, {}});
  } else if (parts_of(root) > max_nodes) {
    size_t keep = static_cast<size_t>(max_nodes - 1 - (dirs[static_cast<size_t>(root)].files_direct > 0 ? 1 : 0));
    expand(root, groups, keep);
  } else {
    expand(root, groups, SIZE_MAX);
    while (true) {
      int best = -1;
      for (size_t i = 0; i < groups.size(); ++i) {
        const auto& g = groups[i];
        if (g.kind != Group::Kind::Subtree) continue;
        int k = parts_of(g.dir);
        if (k < 2 || static_cast<int>(groups.size()) - 1 + k > max_nodes) continue;
        if (best < 0 || dirs[static_cast<size_t>(g.dir)].files_total > dirs[static_cast<size_t>(groups[static_cast<size_t>(best)].dir)].files_total) {
          best = static_cast<int>(i);
        }
      }
      if (best < 0) break;
      int d = groups[static_cast<size_t>(best)].dir;
      groups.erase(groups.begin() + best);
      std::vector<Group> parts;
      expand(d, parts, SIZE_MAX);
      groups.insert(groups.end(), parts.begin(), parts.end());
    }
  }
  std::stable_sort(groups.begin(), groups.end(), [&](const Group& a, const Group& b) {
    if ((a.kind == Group::Kind::Other) != (b.kind == Group::Kind::Other)) return b.kind == Group::Kind::Other;
    return dirs[static_cast<size_t>(a.dir)].path < dirs[static_cast<size_t>(b.dir)].path;
  });

  // Directory -> group (subtrees and "other" own their descendants; a Files
  // group owns only the files directly in its directory).
  std::unordered_map<int, int> subtree_group, files_group;
  for (size_t i = 0; i < groups.size(); ++i) {
    const auto& g = groups[i];
    if (g.kind == Group::Kind::Subtree) subtree_group[g.dir] = static_cast<int>(i);
    else if (g.kind == Group::Kind::Files) files_group[g.dir] = static_cast<int>(i);
    else for (int d : g.other_dirs) subtree_group[d] = static_cast<int>(i);
  }
  std::vector<int> group_of_file(static_cast<size_t>(max_file_id + 1), -1);
  for (const auto& [file, d] : file_dir) {
    int g = -1;
    if (auto it = files_group.find(d); it != files_group.end()) g = it->second;
    for (int p = d; g < 0 && p >= 0; p = dirs[static_cast<size_t>(p)].parent) {
      if (auto it = subtree_group.find(p); it != subtree_group.end()) g = it->second;
    }
    group_of_file[static_cast<size_t>(file)] = g;
  }

  // Per-group statistics.
  struct Stats {
    int64_t files = 0, symbols = 0, entries = 0, routes = 0, handlers = 0, internal = 0;
    std::map<std::string, int64_t> languages;
  };
  std::vector<Stats> stats(groups.size());
  for (const auto& [file, d] : file_dir) {
    int g = group_of_file[static_cast<size_t>(file)];
    if (g < 0) continue;
    ++stats[static_cast<size_t>(g)].files;
    const auto& lang = file_lang[file];
    if (!lang.empty()) ++stats[static_cast<size_t>(g)].languages[lang];
  }
  auto per_file = [&](const char* sql, int64_t Stats::*field) {
    auto st = db_.prepare(sql);
    while (st.step()) {
      int64_t file = st.int64(0);
      if (file < 0 || file > max_file_id) continue;
      int g = group_of_file[static_cast<size_t>(file)];
      if (g >= 0) stats[static_cast<size_t>(g)].*field += st.int64(1);
    }
  };
  per_file("SELECT file_id, COUNT(*) FROM symbols WHERE kind != 'module' GROUP BY file_id", &Stats::symbols);
  per_file("SELECT file_id, COUNT(*) FROM entry_points GROUP BY file_id", &Stats::entries);
  per_file("SELECT file_id, COUNT(*) FROM routes WHERE origin_route_id IS NULL GROUP BY file_id", &Stats::routes);
  per_file("SELECT s.file_id, 1 FROM routes r JOIN symbols s ON s.id = r.handler_symbol_id", &Stats::handlers);

  // Edges between groups; code outside the focus becomes one "outside" node.
  const int outside = static_cast<int>(groups.size());
  std::map<std::pair<int, int>, std::map<std::string, int64_t>> edges;
  {
    auto st = db_.prepare(
        "SELECT e.file_id, d.file_id, e.kind FROM edges e JOIN symbols d ON d.id = e.dst_symbol_id "
        "WHERE e.dst_symbol_id IS NOT NULL AND e.status IN ('resolved','heuristic') AND e.kind IN "
        "('calls','instantiates','registers_route','configures','mounts','uses_middleware','loads_module')");
    while (st.step()) {
      int64_t a_file = st.int64(0), b_file = st.int64(1);
      int a = a_file <= max_file_id ? group_of_file[static_cast<size_t>(a_file)] : -1;
      int b = b_file <= max_file_id ? group_of_file[static_cast<size_t>(b_file)] : -1;
      if (a < 0 && b < 0) continue;
      if (a < 0) a = outside;
      if (b < 0) b = outside;
      if (a == b) {
        ++stats[static_cast<size_t>(a)].internal;
        continue;
      }
      ++edges[{a, b}][st.text(2)];
    }
  }
  bool has_outside = std::any_of(edges.begin(), edges.end(), [&](const auto& e) {
    return e.first.first == outside || e.first.second == outside;
  });

  json nodes = json::array();
  for (size_t i = 0; i < groups.size(); ++i) {
    const auto& g = groups[i];
    const auto& d = dirs[static_cast<size_t>(g.dir)];
    std::string rel = relative_to(d.path, focus);
    std::string label, kind, path = d.path;
    bool drill = false;
    if (g.kind == Group::Kind::Subtree) {
      label = short_label(rel);
      kind = "dir";
      drill = parts_of(g.dir) >= 2 || !d.children.empty();
    } else if (g.kind == Group::Kind::Files) {
      label = rel.empty() ? (d.children.empty() && !focus.empty() ? short_label(focus) : "(top-level files)")
                          : short_label(rel) + "/*";
      kind = "files";
    } else {
      label = "other (" + std::to_string(g.other_dirs.size()) + " dirs)";
      kind = "other";
    }
    const auto& s = stats[i];
    json langs = json::object();
    for (const auto& [l, n] : s.languages) langs[l] = n;
    nodes.push_back({{"id", static_cast<int64_t>(i)},
                     {"label", label},
                     {"path", path},
                     {"kind", kind},
                     {"drill", drill},
                     {"files", s.files},
                     {"symbols", s.symbols},
                     {"languages", langs},
                     {"entry_points", s.entries},
                     {"routes_declared", s.routes},
                     {"route_handlers", s.handlers},
                     {"internal_calls", s.internal}});
  }
  if (has_outside) {
    nodes.push_back({{"id", outside}, {"label", "(rest of the project)"}, {"path", ""}, {"kind", "outside"},
                     {"drill", false}, {"files", 0}, {"symbols", 0}, {"languages", json::object()}, {"entry_points", 0},
                     {"routes_declared", 0}, {"route_handlers", 0}, {"internal_calls", 0}});
  }
  json out_edges = json::array();
  for (const auto& [pair, kinds] : edges) {
    int64_t total = 0;
    json k = json::object();
    for (const auto& [kind, n] : kinds) {
      k[kind] = n;
      total += n;
    }
    out_edges.push_back({{"from", pair.first}, {"to", pair.second}, {"count", total}, {"kinds", k}});
  }
  result["nodes"] = nodes;
  result["edges"] = out_edges;
  // A leaf directory: list its files so they can be opened directly.
  if (groups.size() == 1 && dirs[static_cast<size_t>(groups[0].dir)].children.empty()) {
    json files = json::array();
    auto st = db_.prepare(
        "SELECT f.id, f.path, IFNULL(f.language, ''), (SELECT COUNT(*) FROM symbols s WHERE s.file_id = f.id AND "
        "s.kind != 'module') FROM files f WHERE f.status IN ('parsed','syntax_errors') AND (?1 = '' OR f.path LIKE ?1 || '/%') "
        "ORDER BY f.path LIMIT 500");
    st.bind(1, focus);
    while (st.step()) {
      files.push_back({{"id", st.int64(0)}, {"path", st.text(1)}, {"language", st.text(2)}, {"symbols", st.int64(3)}});
    }
    result["files"] = files;
  }
  return result;
}

}  // namespace proj
