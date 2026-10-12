#include "core/paths.hpp"

#include <cctype>
#include <cstdlib>
#include <pwd.h>
#include <stdexcept>
#include <unistd.h>
#include <vector>

namespace proj::paths {
namespace {

std::string home_dir() {
  if (const char* h = std::getenv("HOME"); h && *h) return h;
  if (struct passwd* pw = getpwuid(getuid()); pw && pw->pw_dir) return pw->pw_dir;
  throw std::runtime_error("cannot determine the home directory (HOME is not set)");
}

std::vector<std::string_view> split(std::string_view s) {
  std::vector<std::string_view> parts;
  size_t start = 0;
  while (start <= s.size()) {
    size_t end = s.find('/', start);
    if (end == std::string_view::npos) end = s.size();
    parts.push_back(s.substr(start, end - start));
    start = end + 1;
  }
  return parts;
}

}  // namespace

fs::path default_data_dir() {
  if (const char* p = std::getenv("PROJ_HOME"); p && *p) return fs::path(p);
#if defined(__APPLE__)
  return fs::path(home_dir()) / "Library" / "Application Support" / "proj";
#else
  if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x && x[0] == '/') {
    return fs::path(x) / "proj";
  }
  return fs::path(home_dir()) / ".local" / "share" / "proj";
#endif
}

std::optional<std::string> normalize_relative(std::string_view rel) {
  if (!rel.empty() && rel.front() == '/') return std::nullopt;
  std::vector<std::string_view> out;
  for (auto part : split(rel)) {
    if (part.empty() || part == ".") continue;
    if (part == "..") {
      if (out.empty()) return std::nullopt;
      out.pop_back();
      continue;
    }
    out.push_back(part);
  }
  std::string result;
  for (size_t i = 0; i < out.size(); ++i) {
    if (i) result += '/';
    result += out[i];
  }
  return result;
}

std::optional<std::string> join_relative(std::string_view dir, std::string_view rel) {
  if (!rel.empty() && rel.front() == '/') return std::nullopt;
  std::string joined(dir);
  if (!joined.empty()) joined += '/';
  joined += rel;
  return normalize_relative(joined);
}

std::string parent_dir(std::string_view rel) {
  auto pos = rel.rfind('/');
  return pos == std::string_view::npos ? std::string() : std::string(rel.substr(0, pos));
}

std::string base_name(std::string_view rel) {
  auto pos = rel.rfind('/');
  return std::string(pos == std::string_view::npos ? rel : rel.substr(pos + 1));
}

std::string extension(std::string_view name) {
  auto base = base_name(name);
  auto pos = base.rfind('.');
  if (pos == std::string::npos || pos == 0) return "";
  std::string ext = base.substr(pos);
  for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return ext;
}

std::string stem(std::string_view name) {
  auto base = base_name(name);
  auto pos = base.rfind('.');
  if (pos == std::string::npos || pos == 0) return base;
  return base.substr(0, pos);
}

std::optional<std::string> relative_to_root(const fs::path& root, const fs::path& path) {
  auto r = root.lexically_normal();
  auto p = path.lexically_normal();
  auto rit = r.begin();
  auto pit = p.begin();
  for (; rit != r.end(); ++rit) {
    if (rit->empty()) continue;  // trailing separator
    if (pit == p.end() || *rit != *pit) return std::nullopt;
    ++pit;
  }
  std::string rel;
  for (; pit != p.end(); ++pit) {
    if (pit->empty()) continue;
    if (*pit == "..") return std::nullopt;
    if (!rel.empty()) rel += '/';
    rel += pit->string();
  }
  return rel;
}

}  // namespace proj::paths
