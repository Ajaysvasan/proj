#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace proj {

// gitignore-style wildcard match of `text` against `pattern`.
//   *   any run of characters except '/'
//   ?   any single character except '/'
//   [ ] character class (supports ranges and leading '!' or '^')
//   **  in "**/", "/**/" or a trailing "/**": any number of directories
//   \x  literal x
bool glob_match(std::string_view pattern, std::string_view text);

struct IgnoreRule {
  std::string pattern;  // normalized: no leading '!' or '/', no trailing '/'
  std::string base;     // project-relative directory the rule applies under ("" = root)
  std::string source;   // e.g. ".gitignore", "default", "--exclude"
  // For rules from a .gitignore *above* the project root (inside an enclosing
  // git repository): path from that file's directory to the project root.
  // Project paths are matched as "<prefix>/<rel_path>".
  std::string prefix;
  bool negate = false;
  bool dir_only = false;
  bool anchored = false;  // pattern contains '/', so it matches the path relative to base
};

// Ordered list of ignore rules; the last matching rule decides, as in git.
class IgnoreRules {
 public:
  // Parses gitignore syntax (comments, blank lines, '!', trailing '/', escapes).
  void add_text(std::string_view text, std::string_view base, std::string_view source, std::string_view prefix = "");
  void add_pattern(std::string_view pattern, std::string_view base, std::string_view source,
                   std::string_view prefix = "");

  // Returns the deciding rule when `rel_path` is ignored, nullptr otherwise.
  const IgnoreRule* match(std::string_view rel_path, bool is_dir) const;

  size_t size() const { return rules_.size(); }
  // Drops rules added after the first `n` (used to scope nested .gitignore files).
  void truncate(size_t n) { rules_.resize(n); }

 private:
  std::vector<IgnoreRule> rules_;
};

}  // namespace proj
