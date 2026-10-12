#pragma once

#include <cstdint>
#include <string>

#include "core/json.hpp"
#include "index/registry.hpp"
#include "scan/scanner.hpp"

namespace proj {

struct AnalysisOptions {
  ScanOptions scan;
  size_t diagnostics_capacity = 2000;
  double parse_timeout_ms = 10000.0;

  // Validates and reads options from a request; throws std::invalid_argument.
  static AnalysisOptions from_json(const json& j);
  json to_json() const;
};

struct AnalysisOutcome {
  std::string status;  // success|partial|no_supported_files|failed
  int64_t run_id = 0;
  json summary;
  std::string error;
};

// Runs one full analysis of a registered project. The index is built in a
// private file next to the live index and atomically renamed over it only
// when the build completes, so a failed run never replaces a valid index.
class Indexer {
 public:
  Indexer(Registry& registry, ProjectRecord project, AnalysisOptions options);
  // Full analysis: builds a new index from scratch.
  AnalysisOutcome run();
  // Incremental refresh of the existing index (see index/refresh.hpp). The
  // existing index must have the current schema version.
  AnalysisOutcome refresh(double max_affected_fraction = 0.4);

 private:
  AnalysisOutcome execute(bool incremental);
  double max_affected_fraction_ = 0.4;

  Registry& registry_;
  ProjectRecord project_;
  AnalysisOptions options_;
};

// Removes leftover build files from analyses that crashed (only files named
// <index>.building-<pid> whose process is gone).
void remove_stale_build_files(const std::filesystem::path& index_path);

}  // namespace proj
