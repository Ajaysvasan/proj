#include "parse/ts_util.hpp"

namespace proj::ts {

std::string clip(std::string_view s, size_t max) {
  std::string out;
  out.reserve(std::min(s.size(), max + 4));
  bool space = false;
  for (char c : s) {
    if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
      space = !out.empty();
      continue;
    }
    if (space) {
      out += ' ';
      space = false;
    }
    if (out.size() >= max) {
      // Do not cut a UTF-8 sequence in half.
      while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80) out.pop_back();
      if (!out.empty() && (static_cast<unsigned char>(out.back()) & 0x80)) out.pop_back();
      out += "\xE2\x80\xA6";  // …
      return out;
    }
    out += c;
  }
  return out;
}

std::string short_text(TSNode n, std::string_view src, size_t max) { return clip(text(n, src), max); }

TSNode child_of_type(TSNode n, const char* t) {
  uint32_t count = ts_node_named_child_count(n);
  for (uint32_t i = 0; i < count; ++i) {
    TSNode c = ts_node_named_child(n, i);
    if (is(c, t)) return c;
  }
  return TSNode{};
}

}  // namespace proj::ts
