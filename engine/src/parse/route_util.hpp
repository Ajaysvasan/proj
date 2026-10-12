#pragma once

// Helpers shared by the extractors that recognize routes registered without a
// framework: route tables, registration calls and if/switch dispatch on the
// request path.
#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

namespace proj::route_util {

// A string literal that looks like an HTTP route ("/api/users/:id", "/") and
// not a filesystem path, a format string or prose.
inline bool is_route_path(std::string_view s) {
  if (s.empty() || s[0] != '/' || s.size() > 300) return false;
  if (s.size() > 1 && s[1] == '/') return false;  // "//host" or a comment marker
  for (char c : s) {
    if (std::isspace(static_cast<unsigned char>(c)) || c == '%' || c == '\n') return false;
  }
  static const std::string_view kFilesystem[] = {"/dev/", "/etc/", "/proc/", "/sys/", "/tmp/", "/var/", "/usr/",
                                                 "/bin/", "/sbin/", "/lib", "/opt/", "/home/", "/root/", "/mnt/",
                                                 "/boot/", "/run/", "/Users/", "/Library/", "/System/"};
  for (auto prefix : kFilesystem) {
    if (s.rfind(prefix, 0) == 0) return false;
  }
  return true;
}

// "GET", "post", "HTTP_GET", "drogon::Get", "Method::POST", "http::verb::put", "kDelete" -> "GET"/"POST"/...
inline std::optional<std::string> http_method_name(std::string_view token) {
  std::string t(token);
  auto sep = t.find_last_of(":.>");
  if (sep != std::string::npos) t = t.substr(sep + 1);
  std::string up;
  for (char c : t) up += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  for (const char* prefix : {"HTTP_METHOD_", "HTTP_", "METHOD_", "HTTP"}) {
    std::string p(prefix);
    if (up.rfind(p, 0) == 0 && up.size() > p.size()) {
      up = up.substr(p.size());
      break;
    }
  }
  if (up.size() > 1 && up[0] == 'K' && t.size() > 1 && t[0] == 'k' && std::isupper(static_cast<unsigned char>(t[1]))) {
    up = up.substr(1);  // kGet
  }
  static const std::unordered_set<std::string> kVerbs = {"GET", "POST", "PUT", "DELETE", "PATCH", "HEAD", "OPTIONS"};
  if (kVerbs.count(up)) return up;
  return std::nullopt;
}

// Calls that are never the handler of a dispatch branch: logging, output,
// string comparison, request accessors.
inline bool is_noise_call(std::string_view name) {
  static const std::unordered_set<std::string_view> kNoise = {
      "printf",     "fprintf",      "sprintf",     "snprintf",    "puts",          "fputs",       "perror",
      "strcmp",     "strncmp",      "strcasecmp",  "strncasecmp", "strlen",        "strstr",      "memcmp",
      "memcpy",     "memset",       "strcpy",      "strncpy",     "free",          "malloc",      "calloc",
      "assert",     "log",          "syslog",      "print",       "println",       "debug",       "info",
      "warn",       "warning",      "error",       "trace",       "equals",        "equalsIgnoreCase",
      "startsWith", "endsWith",     "contains",    "matches",     "compare",       "compareTo",   "getParameter",
      "getMethod",  "getPathInfo",  "getRequestURI", "getServletPath", "getWriter", "getOutputStream",
      "setStatus",  "setContentType", "setHeader", "addHeader",   "write",         "flush",       "close",
      "send_response", "send_header", "end_headers", "send_error", "len",         "str",         "isinstance",
      "startswith", "endswith",     "lower",       "upper",       "strip",         "split",       "getLogger",
      "format",     "append",       "size",        "length",      "empty",         "c_str",       "substr",
      "starts_with", "ends_with",   "toString",    "valueOf",     "parseInt",      "respond",     "reply",
      "send",       "sendResponse", "jsonify",     "make_response", "write_json",  "writeJson",   "json_response",
      "set_content", "setBody",     "encode",      "decode",      "rsplit",        "rstrip",      "lstrip"};
  return kNoise.count(name) > 0;
}

// A literal compared in a dispatch branch names a resource: "/users", not "/*"
// (classpath wildcards) or "/".
inline bool names_resource(std::string_view s) {
  return is_route_path(s) && std::any_of(s.begin(), s.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)); });
}

// A dispatch condition that tests the request: `uri == "/x"`, `"/x".equals(path)`,
// `self.path.startswith("/x")`, `strcmp(req->url, "/x") == 0`.
inline bool mentions_request(std::string_view condition) {
  std::string l;
  for (char c : condition) l += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (const char* w : {"path", "uri", "url", "route", "action", "command", "cmd", "endpoint", "request", "req",
                        "target", "location", "servlet", "resource"}) {
    if (l.find(w) != std::string::npos) return true;
  }
  return false;
}

// Calls whose name says they handle the request; preferred over the first call in a branch.
inline bool is_handler_like(std::string_view name) {
  std::string l;
  for (char c : name) l += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (const char* w : {"handle", "serve", "process", "dispatch", "route", "render", "controller", "action"}) {
    if (l.find(w) != std::string::npos) return true;
  }
  return l.rfind("on", 0) == 0 || l.rfind("do", 0) == 0;
}

}  // namespace proj::route_util
