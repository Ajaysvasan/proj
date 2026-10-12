#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "db/sqlite.hpp"
#include "parse/facts.hpp"
#include "scan/scanner.hpp"

namespace proj {

struct FileRecord {
  std::string rel_path;
  std::string language;  // empty when unsupported
  uint64_t size = 0;
  int64_t mtime_s = 0;
  std::string fingerprint;
  std::string status;
  int error_count = 0;
  uint32_t first_error_line = 0;
  uint32_t line_count = 0;
  std::string module_name;
  int extractor_version = 0;
  std::string note;
};

struct WriteCounts {
  int64_t symbols = 0;
  int64_t calls = 0;
  int64_t imports = 0;
  int64_t routes = 0;
  int64_t exceptions = 0;
  int64_t entries = 0;
};

// Persists per-file facts into a fresh index database. Cross-file
// relationships are written in a "pending" state and resolved later by the
// Resolver, which works from the database rather than from memory.
class FactWriter {
 public:
  explicit FactWriter(db::Database& db);

  int64_t insert_file(const FileRecord& file);
  // Updates a file row in place (refresh), keeping its id so references from
  // other files stay valid.
  void update_file(int64_t file_id, const FileRecord& file);
  // Writes every fact for one file. Local fact indexes are mapped to row ids.
  // `reuse_ids` (refresh) maps symbol identity keys (see symbol_key) of the
  // file's previous symbols to their ids; matching symbols keep their id so
  // references from other files stay valid. Reused ids are appended to `reused`.
  WriteCounts write_facts(int64_t file_id, const std::string& language, const FileFacts& facts,
                          const std::unordered_map<std::string, int64_t>* reuse_ids = nullptr,
                          std::vector<int64_t>* reused = nullptr);

  // Identity of a symbol within its file: kind, qualified name, parameter count
  // and occurrence among symbols sharing those three.
  static std::string symbol_key(const std::string& kind, const std::string& qualified_name, int param_count,
                                int ordinal);

 private:
  db::Database& db_;
  db::Statement file_, link_receiver_, file_update_, symbol_, symbol_with_id_, edge_, dep_, binding_, export_, hint_, router_, mount_, route_, middleware_,
      exception_, entry_;
};

}  // namespace proj
