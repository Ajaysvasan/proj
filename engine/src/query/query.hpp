#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/json.hpp"
#include "db/sqlite.hpp"

namespace proj {

// Hard upper bounds applied to every query regardless of what the caller asks.
struct QueryLimits {
  static constexpr int64_t kMaxResults = 500;
  static constexpr int kMaxDepth = 25;
  static constexpr int64_t kMaxNodes = 2000;
  static constexpr int64_t kMaxChildrenPerNode = 100;
  static constexpr int kMaxSnippetLines = 200;
};

struct TraverseOptions {
  std::string direction = "forward";  // forward (callees) | backward (callers)
  int max_depth = 4;
  int64_t max_nodes = 200;
  int64_t max_children = 40;
  bool include_unresolved = true;
  std::vector<std::string> edge_kinds;  // empty = default set
};

// Read-only access to one project index. Every query is bounded by explicit
// limits and reads only the rows it needs; nothing loads the whole graph.
class IndexReader {
 public:
  explicit IndexReader(const std::filesystem::path& index_path);

  const std::filesystem::path& path() const { return path_; }
  json summary();
  json search(const std::string& query, const std::vector<std::string>& kinds, bool exact, int64_t limit);
  json symbol(int64_t id);
  json edges(int64_t symbol_id, const std::string& direction, const std::vector<std::string>& kinds, int64_t limit,
             int64_t offset);
  json traverse(int64_t symbol_id, const TraverseOptions& options);
  json file(std::optional<int64_t> id, const std::string& path, int64_t symbol_limit);
  json file_deps(int64_t file_id, const std::string& direction, int64_t limit, int64_t offset);
  json entry_points(int64_t limit);
  json routes(int64_t limit, int64_t offset);
  // Code map: the main parts of the project (directory groups, at most
  // `max_nodes`) under `focus` and the calls, registrations and wiring between them.
  // `direct_files` lists only the files directly in `focus` instead.
  json overview(const std::string& focus, int max_nodes, bool direct_files = false);
  json route_flow(int64_t route_id, const TraverseOptions& options);
  json exceptions(int64_t symbol_id, int depth, int64_t max_nodes);
  json startup_path(int64_t symbol_id, int max_depth);
  json check();
  json snippet(int64_t file_id, int64_t line, int before, int after);

 private:
  std::filesystem::path path_;
  db::Database db_;

  json symbol_brief(int64_t id);
  json symbol_brief_row(db::Statement& st, int first_col);
  std::string meta(const std::string& key);
};

}  // namespace proj
