#pragma once

#include <tree_sitter/api.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>

#include "parse/facts.hpp"

// Small helpers over the Tree-sitter C API shared by all extractors.
namespace proj::ts {

inline constexpr size_t kMaxTextLen = 160;
inline constexpr uint32_t kMaxTreeDepth = 1500;

inline bool is(TSNode n, const char* type) { return !ts_node_is_null(n) && std::strcmp(ts_node_type(n), type) == 0; }
inline std::string_view type(TSNode n) { return ts_node_is_null(n) ? std::string_view() : ts_node_type(n); }

inline TSNode field(TSNode n, const char* name) {
  return ts_node_child_by_field_name(n, name, static_cast<uint32_t>(std::strlen(name)));
}

inline std::string_view text(TSNode n, std::string_view src) {
  if (ts_node_is_null(n)) return {};
  uint32_t s = ts_node_start_byte(n), e = ts_node_end_byte(n);
  if (s > src.size() || e > src.size() || e < s) return {};
  return src.substr(s, e - s);
}

// Text collapsed to one line and truncated to `max` bytes (with "…").
std::string short_text(TSNode n, std::string_view src, size_t max = kMaxTextLen);
std::string clip(std::string_view s, size_t max = kMaxTextLen);

inline Pos start_pos(TSNode n) {
  TSPoint p = ts_node_start_point(n);
  return {p.row + 1, p.column + 1};
}
inline Pos end_pos(TSNode n) {
  TSPoint p = ts_node_end_point(n);
  return {p.row + 1, p.column + 1};
}
inline Span span(TSNode n) { return {start_pos(n), end_pos(n)}; }

// First named child with the given type, or a null node.
TSNode child_of_type(TSNode n, const char* type);

// Iterative pre/post-order traversal of named nodes. `enter` returns whether
// to descend into the node's children; `leave` is called exactly once for
// every entered node. Subtrees deeper than `max_depth` are skipped; returns
// true if that happened.
// Owns a Tree-sitter cursor (heap-allocated internally) so it is released on
// every exit path, including exceptions thrown by extractor callbacks.
struct CursorGuard {
  TSTreeCursor cursor;
  explicit CursorGuard(TSNode root) : cursor(ts_tree_cursor_new(root)) {}
  ~CursorGuard() { ts_tree_cursor_delete(&cursor); }
  CursorGuard(const CursorGuard&) = delete;
  CursorGuard& operator=(const CursorGuard&) = delete;
};

template <class Enter, class Leave>
bool walk(TSNode root, Enter&& enter, Leave&& leave, uint32_t max_depth = kMaxTreeDepth) {
  bool limited = false;
  CursorGuard guard(root);
  TSTreeCursor& cur = guard.cursor;
  uint32_t depth = 0;
  while (true) {
    TSNode node = ts_tree_cursor_current_node(&cur);
    bool named = ts_node_is_named(node);
    bool descend = named && enter(node);
    if (descend && depth >= max_depth && ts_node_child_count(node) > 0) {
      limited = true;
      descend = false;
    }
    if (descend && ts_tree_cursor_goto_first_child(&cur)) {
      ++depth;
      continue;
    }
    if (named) leave(node);
    while (true) {
      if (depth == 0) return limited;
      if (ts_tree_cursor_goto_next_sibling(&cur)) break;
      ts_tree_cursor_goto_parent(&cur);
      --depth;
      TSNode parent = ts_tree_cursor_current_node(&cur);
      if (ts_node_is_named(parent)) leave(parent);
    }
  }
}

// Stable identity of a node within one tree.
inline const void* node_id(TSNode n) { return n.id; }

}  // namespace proj::ts
