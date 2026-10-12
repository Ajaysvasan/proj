#include "scan/ignore.hpp"

#include "core/paths.hpp"

namespace proj {
namespace {

// Matches a bracket expression starting at pattern[pi] == '['. On success,
// sets `next` to the index after ']' and `matched` to whether c is in the set.
bool match_class(std::string_view p, size_t pi, char c, size_t& next, bool& matched) {
  size_t i = pi + 1;
  bool negate = false;
  if (i < p.size() && (p[i] == '!' || p[i] == '^')) {
    negate = true;
    ++i;
  }
  bool found = false;
  bool first = true;
  while (i < p.size() && (first || p[i] != ']')) {
    first = false;
    char lo = p[i];
    if (lo == '\\' && i + 1 < p.size()) lo = p[++i];
    char hi = lo;
    if (i + 2 < p.size() && p[i + 1] == '-' && p[i + 2] != ']') {
      hi = p[i + 2];
      if (hi == '\\' && i + 3 < p.size()) hi = p[i + 3], ++i;
      i += 2;
    }
    if (c >= lo && c <= hi) found = true;
    ++i;
  }
  if (i >= p.size()) return false;  // unterminated class: treat '[' literally
  next = i + 1;
  matched = (found != negate) && c != '/';
  return true;
}

bool match_from(std::string_view p, size_t pi, std::string_view t, size_t ti, int depth) {
  if (depth > 64) return false;  // pathological pattern; refuse rather than recurse unboundedly
  while (pi < p.size()) {
    char pc = p[pi];
    if (pc == '*') {
      bool double_star = pi + 1 < p.size() && p[pi + 1] == '*';
      if (double_star) {
        bool at_segment_start = pi == 0 || p[pi - 1] == '/';
        size_t after = pi + 2;
        if (at_segment_start && after == p.size()) return true;  // trailing "**": everything below
        if (at_segment_start && p[after] == '/') {
          // "**/": zero or more whole directories.
          size_t rest = after + 1;
          if (match_from(p, rest, t, ti, depth + 1)) return true;
          for (size_t k = ti; k < t.size(); ++k) {
            if (t[k] == '/' && match_from(p, rest, t, k + 1, depth + 1)) return true;
          }
          return false;
        }
        // Not a path-segment "**": behaves like "*".
        pi = after - 1;
      }
      size_t rest = pi + 1;
      for (size_t k = ti;; ++k) {
        if (match_from(p, rest, t, k, depth + 1)) return true;
        if (k >= t.size() || t[k] == '/') return false;
      }
    }
    if (ti >= t.size()) return false;
    if (pc == '?') {
      if (t[ti] == '/') return false;
      ++pi, ++ti;
      continue;
    }
    if (pc == '[') {
      size_t next = 0;
      bool matched = false;
      if (match_class(p, pi, t[ti], next, matched)) {
        if (!matched) return false;
        pi = next, ++ti;
        continue;
      }
    }
    if (pc == '\\' && pi + 1 < p.size()) pc = p[++pi];
    if (pc != t[ti]) return false;
    ++pi, ++ti;
  }
  return ti == t.size();
}

std::string_view trim_trailing_spaces(std::string_view line) {
  while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
    if (line.size() >= 2 && line[line.size() - 2] == '\\') break;  // escaped space is kept
    line.remove_suffix(1);
  }
  return line;
}

}  // namespace

bool glob_match(std::string_view pattern, std::string_view text) { return match_from(pattern, 0, text, 0, 0); }

void IgnoreRules::add_text(std::string_view text, std::string_view base, std::string_view source,
                           std::string_view prefix) {
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string_view::npos) end = text.size();
    add_pattern(text.substr(start, end - start), base, source, prefix);
    start = end + 1;
  }
}

void IgnoreRules::add_pattern(std::string_view line, std::string_view base, std::string_view source,
                              std::string_view prefix) {
  line = trim_trailing_spaces(line);
  if (line.empty() || line.front() == '#') return;
  IgnoreRule rule;
  rule.base = std::string(base);
  rule.source = std::string(source);
  rule.prefix = std::string(prefix);
  if (line.front() == '!') {
    rule.negate = true;
    line.remove_prefix(1);
  } else if (line.size() >= 2 && line[0] == '\\' && (line[1] == '!' || line[1] == '#')) {
    line.remove_prefix(1);
  }
  if (!line.empty() && line.back() == '/') {
    rule.dir_only = true;
    line.remove_suffix(1);
  }
  if (line.find('/') != std::string_view::npos) rule.anchored = true;
  while (!line.empty() && line.front() == '/') line.remove_prefix(1);
  if (line.empty()) return;
  rule.pattern = std::string(line);
  rules_.push_back(std::move(rule));
}

const IgnoreRule* IgnoreRules::match(std::string_view rel_path, bool is_dir) const {
  const IgnoreRule* decided = nullptr;
  std::string prefixed;
  for (const auto& rule : rules_) {
    std::string_view sub = rel_path;
    if (!rule.prefix.empty()) {
      prefixed = rule.prefix + "/" + std::string(rel_path);
      sub = prefixed;
    } else if (!rule.base.empty()) {
      if (rel_path.size() <= rule.base.size() || rel_path.substr(0, rule.base.size()) != rule.base ||
          rel_path[rule.base.size()] != '/') {
        continue;
      }
      sub = rel_path.substr(rule.base.size() + 1);
    }
    if (rule.dir_only && !is_dir) continue;
    bool hit;
    if (rule.anchored) {
      hit = glob_match(rule.pattern, sub);
    } else {
      auto slash = sub.rfind('/');
      hit = glob_match(rule.pattern, slash == std::string_view::npos ? sub : sub.substr(slash + 1));
    }
    if (hit) decided = &rule;
  }
  return decided && !decided->negate ? decided : nullptr;
}

}  // namespace proj
