#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "index/registry.hpp"
#include "scan/ignore.hpp"
#include "scan/language.hpp"

namespace proj {

struct ScanOptions {
  std::vector<std::string> extra_excludes;    // gitignore-syntax patterns from the user
  std::vector<std::string> include_defaults;  // default-excluded names to scan anyway (e.g. "vendor")
  bool default_excludes = true;
  bool use_gitignore = true;
  uint64_t max_file_size = 1024 * 1024;  // bytes; larger source files are skipped
  int64_t max_files = 200000;            // stop scanning after this many files
  int max_depth = 64;                    // directory nesting limit
};

// Status values stored in files.status. See docs/INDEX_SCHEMA.md.
namespace file_status {
inline constexpr const char* kPending = "pending";  // eligible; handed to the parser
inline constexpr const char* kParsed = "parsed";
inline constexpr const char* kSyntaxErrors = "syntax_errors";
inline constexpr const char* kParseFailed = "parse_failed";
inline constexpr const char* kUnsupported = "unsupported";
inline constexpr const char* kExcluded = "excluded";
inline constexpr const char* kTooLarge = "too_large";
inline constexpr const char* kBinary = "binary";
inline constexpr const char* kUnreadable = "unreadable";
inline constexpr const char* kSymlink = "symlink";
}  // namespace file_status

struct ScanEntry {
  std::string rel_path;             // '/'-separated, relative to the project root
  std::filesystem::path abs_path;
  uint64_t size = 0;
  int64_t mtime_s = 0;
  Language language = Language::None;
  std::string status;               // one of file_status::*
  std::string note;                 // e.g. the exclusion rule that matched
};

struct ScanStats {
  int64_t files_seen = 0;
  int64_t dirs_excluded = 0;
  int64_t dirs_unreadable = 0;
  int64_t dirs_symlink = 0;
  int64_t special_files = 0;
  bool file_limit_reached = false;
  bool depth_limit_reached = false;
};

// Walks a project directory without following symbolic links, applying
// default exclusions, .gitignore files, .git/info/exclude, .projignore and
// user patterns. Entries are visited in a deterministic (sorted) order and
// nothing is read beyond directory listings and ignore files.
class Scanner {
 public:
  Scanner(std::filesystem::path root, ScanOptions options, DiagnosticSink& diagnostics);

  void scan(const std::function<void(const ScanEntry&)>& visit);
  const ScanStats& stats() const { return stats_; }

  static const std::vector<std::string>& default_excluded_dirs();
  static const std::vector<std::string>& default_excluded_files();

 private:
  std::filesystem::path root_;
  ScanOptions options_;
  DiagnosticSink& diagnostics_;
  IgnoreRules rules_;
  ScanStats stats_;

  void walk_dir(const std::filesystem::path& abs, const std::string& rel, int depth,
                const std::function<void(const ScanEntry&)>& visit);
  void load_ignore_file(const std::filesystem::path& file, const std::string& base, const std::string& source,
                        const std::string& prefix = "");
  void load_enclosing_repository_rules();
};

}  // namespace proj
