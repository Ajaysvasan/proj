#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/json.hpp"
#include "db/sqlite.hpp"

namespace proj {

class Resolver;

// Incremental refresh of an existing index (applied to a private copy).
//
// Change detection uses the content fingerprint (FNV-1a 64 + size) together
// with the language and extractor version; modification times alone are never
// trusted. Only changed and added files are re-parsed. Symbols of a changed
// file whose signature is unchanged keep their ids, so references to them from
// other files stay valid. Facts of changed and removed files are deleted, and
// every relationship that could resolve differently is reset to "pending":
//   - all relationships of files that referenced a symbol that no longer exists
//   - relationships whose callee or receiver names a definition that was
//     added, removed or changed signature (including subclasses of a changed
//     class and functions declared to return a changed type), and calls
//     chained on them
//   - all relationships of files whose declared or inferred types name such a
//     definition
//   - all relationships of files that import (directly or transitively) a file
//     whose exported interface changed, or that was added or removed
//   - files whose import resolution changed
// Derived rows (module loads, overrides, route registration, mounts, route
// prefixes and middleware chains, manifest entry points) are regenerated.
// When most files are affected, everything is re-resolved instead.
class IncrementalUpdate {
 public:
  explicit IncrementalUpdate(db::Database& db);

  struct Existing {
    int64_t id = 0;
    std::string status;
    std::string language;
    std::string fingerprint;
    int64_t size = 0;
    int64_t mtime_s = 0;
    int extractor_version = 0;
  };
  std::optional<Existing> lookup(const std::string& path);

  void mark_seen(int64_t file_id);
  // Records the file's current interface and symbols, then deletes all its facts.
  void forget_facts(int64_t file_id, bool removed);
  // Previous symbol ids of the file passed to forget_facts(), by identity key.
  const std::unordered_map<std::string, int64_t>& reuse_ids() const { return reuse_ids_; }
  // Records the file's new interface after its facts were written again;
  // `reused_ids` are previous symbol ids that were kept.
  void record_new_facts(int64_t file_id, const std::vector<int64_t>& reused_ids);
  // Deletes files that were not seen by the scan; returns how many.
  int64_t remove_unseen_files();

  // Resets derived and affected state and resolves file dependencies. After
  // this, Resolver::run_relationships() completes the update.
  // When more than `max_affected_fraction` of the source files are affected,
  // all relationships are re-resolved instead (simpler and as fast).
  void prepare(Resolver& resolver, bool files_added_or_removed, int64_t source_files, double max_affected_fraction);

  json stats() const;
  int64_t changed() const { return changed_; }
  int64_t added() const { return added_; }
  int64_t removed() const { return removed_; }
  int64_t unchanged() const { return unchanged_; }
  void count_unchanged() { ++unchanged_; }
  void count_changed(bool is_new) { is_new ? ++added_ : ++changed_; }

 private:
  db::Database& db_;
  db::Statement lookup_, seen_;
  int64_t changed_ = 0, added_ = 0, removed_ = 0, unchanged_ = 0;
  int64_t affected_files_ = 0, reset_edges_ = 0, changed_names_ = 0, reused_symbols_ = 0;
  bool full_reresolve_ = false;
  std::unordered_map<std::string, int64_t> reuse_ids_;
  std::map<std::string, double> timings_;
  json breakdown_ = json::object();

  void snapshot_interface(int64_t file_id, const char* table);
  void compute_affected();
};

}  // namespace proj
