// Configuration file extractors (XML and YAML).
//
// Legacy servers often declare their HTTP endpoints outside the code: servlet
// descriptors (web.xml), Struts action mappings, Spring XML handler mappings,
// OpenAPI documents, serverless.yml, or project-specific route files that name
// the class or function handling a URL. Both syntaxes are converted to one
// small tree (ConfigNode). Recognizers then emit ordinary RouteFacts whose
// handler is a reference by name (a class, Class#method, module.function, a
// source file or a Spring bean). The resolver finds those definitions in the
// project and, for handler classes, the method serving each HTTP method
// (doGet/doPost, execute, handleRequest, get/post, handle, ...).
//
// Known formats are recognized exactly. Other files are searched for entries
// that pair a URL path with a handler reference; those routes are heuristic.
// Configuration files also get "configures" edges for the classes a container
// creates or invokes (servlets, filters, listeners, Spring beans).
#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/paths.hpp"
#include "parse/extractors.hpp"
#include "parse/ts_util.hpp"

namespace proj {
namespace {

using ts::field;
using ts::is;

constexpr int kMaxDepth = 200;
constexpr size_t kMaxNodes = 200000;
constexpr size_t kMaxRoutes = 5000;
constexpr size_t kMaxValue = 300;

struct ConfigNode {
  enum class Kind { Element, Mapping, Sequence, Scalar, Empty };
  Kind kind = Kind::Empty;
  std::string name;  // XML local element name, YAML mapping key ("" for sequence items and documents)
  std::string text;  // XML character data or YAML scalar, trimmed
  std::vector<std::pair<std::string, std::string>> attrs;  // XML attributes (local names)
  std::vector<ConfigNode> children;  // XML child elements, YAML mapping entries or sequence items
  Pos pos;
};

std::string trim(std::string_view s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string_view::npos) return "";
  size_t e = s.find_last_not_of(" \t\r\n");
  return std::string(s.substr(b, e - b + 1));
}

std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string upper(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

// Keys compare without case, '-', '_' and '.': url-pattern == urlPattern == url_pattern.
std::string norm_key(std::string_view k) {
  std::string out;
  for (char c : k) {
    if (std::isalnum(static_cast<unsigned char>(c))) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

std::string local_name(std::string_view n) {
  auto c = n.rfind(':');
  return std::string(c == std::string_view::npos ? n : n.substr(c + 1));
}

void append_utf8(std::string& out, unsigned long cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x110000) {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

// "&amp;", "&#38;", "&#x26;" -> "&"; unknown references are kept as written.
std::string decode_entity(std::string_view ref) {
  if (ref == "&amp;") return "&";
  if (ref == "&lt;") return "<";
  if (ref == "&gt;") return ">";
  if (ref == "&quot;") return "\"";
  if (ref == "&apos;") return "'";
  if (ref.size() > 3 && ref[1] == '#' && ref.back() == ';') {
    bool hex = ref[2] == 'x' || ref[2] == 'X';
    std::string digits(ref.substr(hex ? 3 : 2, ref.size() - (hex ? 4 : 3)));
    char* end = nullptr;
    unsigned long cp = std::strtoul(digits.c_str(), &end, hex ? 16 : 10);
    if (end && *end == '\0' && !digits.empty()) {
      std::string out;
      append_utf8(out, cp);
      return out;
    }
  }
  return std::string(ref);
}

std::string decode_entities(std::string_view s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '&') {
      size_t semi = s.find(';', i);
      if (semi != std::string_view::npos && semi - i <= 10) {
        out += decode_entity(s.substr(i, semi - i + 1));
        i = semi;
        continue;
      }
    }
    out += s[i];
  }
  return out;
}

// ---- XML -> ConfigNode ---------------------------------------------------------

class XmlTree {
 public:
  explicit XmlTree(std::string_view src) : src_(src) {}

  // Returns false when the document has no root element.
  bool build(TSNode document, ConfigNode& root, std::string& doctype) {
    uint32_t c = ts_node_named_child_count(document);
    bool found = false;
    for (uint32_t i = 0; i < c; ++i) {
      TSNode ch = ts_node_named_child(document, i);
      if (is(ch, "prolog")) {
        TSNode dt = ts::child_of_type(ch, "doctypedecl");
        if (dt.id) doctype = std::string(text(dt));
      } else if (is(ch, "element") && !found) {
        element(ch, root, 0);
        found = true;
      }
    }
    return found;
  }

 private:
  std::string_view src_;
  size_t nodes_ = 0;

  std::string_view text(TSNode n) const { return ts::text(n, src_); }

  void element(TSNode el, ConfigNode& out, int depth) {
    out.kind = ConfigNode::Kind::Element;
    out.pos = ts::start_pos(el);
    ++nodes_;
    std::string chars;
    uint32_t c = ts_node_named_child_count(el);
    for (uint32_t i = 0; i < c; ++i) {
      TSNode part = ts_node_named_child(el, i);
      if (is(part, "STag") || is(part, "EmptyElemTag")) {
        tag(part, out);
      } else if (is(part, "content")) {
        uint32_t k = ts_node_named_child_count(part);
        for (uint32_t j = 0; j < k; ++j) {
          TSNode item = ts_node_named_child(part, j);
          if (is(item, "element")) {
            if (depth + 1 >= kMaxDepth || nodes_ >= kMaxNodes) continue;
            ConfigNode child;
            element(item, child, depth + 1);
            out.children.push_back(std::move(child));
          } else if (is(item, "CharData")) {
            chars += text(item);
          } else if (is(item, "CDSect")) {
            TSNode data = ts::child_of_type(item, "CData");
            if (data.id) chars += text(data);
          } else if (is(item, "EntityRef") || is(item, "CharRef")) {
            chars += decode_entity(text(item));
          }
        }
      }
    }
    out.text = trim(chars);
  }

  void tag(TSNode t, ConfigNode& out) {
    uint32_t c = ts_node_named_child_count(t);
    for (uint32_t i = 0; i < c; ++i) {
      TSNode ch = ts_node_named_child(t, i);
      if (is(ch, "Name") && out.name.empty()) {
        out.name = local_name(text(ch));
      } else if (is(ch, "Attribute")) {
        TSNode name = ts::child_of_type(ch, "Name");
        TSNode value = ts::child_of_type(ch, "AttValue");
        if (!name.id || !value.id) continue;
        std::string_view raw = text(value);
        if (raw.size() >= 2) raw = raw.substr(1, raw.size() - 2);
        std::string attr = local_name(text(name));
        // Namespace declarations are not configuration values.
        if (text(name).rfind("xmlns", 0) == 0) continue;
        out.attrs.emplace_back(attr, decode_entities(raw));
      }
    }
  }
};

// ---- YAML -> ConfigNode --------------------------------------------------------

class YamlTree {
 public:
  explicit YamlTree(std::string_view src) : src_(src) {}

  // One ConfigNode per document.
  void build(TSNode stream, std::vector<ConfigNode>& docs) {
    uint32_t c = ts_node_named_child_count(stream);
    for (uint32_t i = 0; i < c && nodes_ < kMaxNodes; ++i) {
      TSNode doc = ts_node_named_child(stream, i);
      if (!is(doc, "document")) continue;
      uint32_t k = ts_node_named_child_count(doc);
      for (uint32_t j = 0; j < k; ++j) {
        TSNode v = ts_node_named_child(doc, j);
        if (is(v, "block_node") || is(v, "flow_node")) {
          ConfigNode root;
          root.pos = ts::start_pos(v);
          value(v, root, 0);
          docs.push_back(std::move(root));
          break;
        }
      }
    }
  }

 private:
  std::string_view src_;
  size_t nodes_ = 0;

  std::string_view text(TSNode n) const { return ts::text(n, src_); }

  // The value inside a block_node / flow_node, skipping anchors, tags and comments.
  TSNode content(TSNode n) const {
    if (!is(n, "block_node") && !is(n, "flow_node")) return n;
    uint32_t c = ts_node_named_child_count(n);
    for (uint32_t i = 0; i < c; ++i) {
      TSNode ch = ts_node_named_child(n, i);
      if (is(ch, "anchor") || is(ch, "tag") || is(ch, "comment")) continue;
      return ch;
    }
    return TSNode{};
  }

  void value(TSNode n, ConfigNode& out, int depth) {
    ++nodes_;
    TSNode v = content(n);
    if (!v.id) return;
    if (depth >= kMaxDepth || nodes_ >= kMaxNodes) return;
    if (is(v, "block_mapping") || is(v, "flow_mapping")) {
      out.kind = ConfigNode::Kind::Mapping;
      uint32_t c = ts_node_named_child_count(v);
      for (uint32_t i = 0; i < c; ++i) {
        TSNode pair = ts_node_named_child(v, i);
        if (is(pair, "block_mapping_pair") || is(pair, "flow_pair")) {
          out.children.push_back(entry(field(pair, "key"), field(pair, "value"), ts::start_pos(pair), depth));
        } else if (is(pair, "flow_node")) {
          out.children.push_back(entry(pair, TSNode{}, ts::start_pos(pair), depth));
        }
      }
    } else if (is(v, "block_sequence") || is(v, "flow_sequence")) {
      out.kind = ConfigNode::Kind::Sequence;
      uint32_t c = ts_node_named_child_count(v);
      for (uint32_t i = 0; i < c; ++i) {
        TSNode item = ts_node_named_child(v, i);
        ConfigNode child;
        child.pos = ts::start_pos(item);
        if (is(item, "block_sequence_item")) {
          TSNode inner = ts_node_named_child(item, 0);
          if (inner.id && !is(inner, "comment")) value(inner, child, depth + 1);
        } else if (is(item, "flow_node")) {
          value(item, child, depth + 1);
        } else if (is(item, "flow_pair")) {
          child.kind = ConfigNode::Kind::Mapping;
          child.children.push_back(entry(field(item, "key"), field(item, "value"), ts::start_pos(item), depth + 1));
        } else {
          continue;
        }
        out.children.push_back(std::move(child));
      }
    } else {
      out.kind = ConfigNode::Kind::Scalar;
      out.text = scalar(v);
    }
  }

  ConfigNode entry(TSNode key, TSNode val, Pos pos, int depth) {
    ConfigNode e;
    e.pos = pos;
    TSNode k = content(key);
    e.name = k.id ? scalar(k) : "";
    if (e.name.size() > kMaxValue) e.name.resize(kMaxValue);
    if (val.id) value(val, e, depth + 1);
    return e;
  }

  std::string scalar(TSNode s) const {
    std::string_view raw = text(s);
    if (is(s, "double_quote_scalar")) return unescape_double(raw);
    if (is(s, "single_quote_scalar")) {
      std::string out;
      std::string_view body = raw.size() >= 2 ? raw.substr(1, raw.size() - 2) : raw;
      for (size_t i = 0; i < body.size(); ++i) {
        out += body[i];
        if (body[i] == '\'' && i + 1 < body.size() && body[i + 1] == '\'') ++i;
      }
      return out;
    }
    if (is(s, "block_scalar")) return block_scalar(raw);
    if (is(s, "alias")) return std::string(raw);
    // Plain scalars: folded lines become single spaces.
    std::string out;
    bool space = false;
    for (char c : raw) {
      if (c == '\n' || c == '\r') {
        space = true;
        continue;
      }
      if (space) {
        if (c == ' ' || c == '\t') continue;
        out += ' ';
        space = false;
      }
      out += c;
    }
    return trim(out);
  }

  static std::string unescape_double(std::string_view raw) {
    std::string_view body = raw.size() >= 2 ? raw.substr(1, raw.size() - 2) : raw;
    std::string out;
    for (size_t i = 0; i < body.size(); ++i) {
      char c = body[i];
      if (c != '\\' || i + 1 >= body.size()) {
        out += c;
        continue;
      }
      char e = body[++i];
      switch (e) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case '0': out += '\0'; break;
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        default: out += '\\'; out += e; break;
      }
    }
    return out;
  }

  // "|" keeps line breaks, ">" folds them; indentation is removed.
  static std::string block_scalar(std::string_view raw) {
    bool folded = !raw.empty() && raw[0] == '>';
    size_t nl = raw.find('\n');
    if (nl == std::string_view::npos) return "";
    std::string out;
    size_t pos = nl + 1;
    while (pos <= raw.size()) {
      size_t end = raw.find('\n', pos);
      std::string line = trim(raw.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos));
      if (!out.empty()) out += folded ? ' ' : '\n';
      out += line;
      if (end == std::string_view::npos) break;
      pos = end + 1;
    }
    return trim(out);
  }
};

// ---- field access ----------------------------------------------------------------

bool is_leaf(const ConfigNode& n) {
  return n.kind == ConfigNode::Kind::Scalar ||
         (n.kind == ConfigNode::Kind::Element && n.children.empty() && n.attrs.empty());
}

// A scalar value of `n` under `key`: an XML attribute, a text-only child
// element, or a YAML scalar entry.
const std::string* value_of(const ConfigNode& n, std::string_view key) {
  std::string nk = norm_key(key);
  for (const auto& [k, v] : n.attrs) {
    if (norm_key(k) == nk) return &v;
  }
  for (const auto& c : n.children) {
    if (!c.name.empty() && is_leaf(c) && norm_key(c.name) == nk) return &c.text;
  }
  return nullptr;
}

std::string value_or(const ConfigNode& n, std::string_view key, std::string fallback = "") {
  const std::string* v = value_of(n, key);
  return v ? *v : fallback;
}

// Every value under `key`: repeated XML elements and YAML sequences of scalars.
std::vector<std::string> values_of(const ConfigNode& n, std::string_view key) {
  std::vector<std::string> out;
  std::string nk = norm_key(key);
  for (const auto& [k, v] : n.attrs) {
    if (norm_key(k) == nk) out.push_back(v);
  }
  for (const auto& c : n.children) {
    if (c.name.empty() || norm_key(c.name) != nk) continue;
    if (is_leaf(c)) {
      out.push_back(c.text);
    } else if (c.kind == ConfigNode::Kind::Sequence) {
      for (const auto& item : c.children) {
        if (item.kind == ConfigNode::Kind::Scalar) out.push_back(item.text);
      }
    }
  }
  return out;
}

const ConfigNode* child_of(const ConfigNode& n, std::string_view key) {
  std::string nk = norm_key(key);
  for (const auto& c : n.children) {
    if (!c.name.empty() && norm_key(c.name) == nk) return &c;
  }
  return nullptr;
}

std::vector<std::string> split_any(std::string_view s, std::string_view seps) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (seps.find(c) != std::string_view::npos) {
      if (!trim(cur).empty()) out.push_back(trim(cur));
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!trim(cur).empty()) out.push_back(trim(cur));
  return out;
}

// ---- values that name code --------------------------------------------------------

bool ident_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$'; }
bool ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$'; }

bool is_identifier(std::string_view s) {
  if (s.empty() || !ident_start(s[0])) return false;
  return std::all_of(s.begin(), s.end(), ident_char);
}

bool is_upper_start(std::string_view s) { return !s.empty() && std::isupper(static_cast<unsigned char>(s[0])); }

const std::unordered_set<std::string> kNotCode = {"true", "false", "null", "none", "yes", "no", "on", "off",
                                                  "nil", "undefined", "default", "auto", "any", "all"};

const std::vector<std::string> kSourceExtensions = {".java", ".py", ".c", ".cc", ".cpp", ".cxx", ".h", ".hpp",
                                                    ".hh", ".js", ".mjs", ".cjs", ".ts", ".jsp", ".jspx"};

bool ends_with(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Classifies a configuration value that names code. RefFact kinds:
//   class      a class: com.acme.UserServlet, app.handlers.UserHandler, api::UserHandler, UserHandler
//   qualified  a function or method: com.acme.Users#list, com.acme.Users.list, app.views.home,
//              app.views:home, api::Users::list (name = full name, receiver = member)
//   name       a bare function or method name (simple names only when `simple` is set)
//   file       a source file, optionally with a function: handlers/users.py, handlers/users.get
std::optional<RefFact> code_ref(const std::string& raw, Pos pos, bool simple) {
  std::string v = trim(raw);
  if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front()) v = v.substr(1, v.size() - 2);
  if (v.empty() || v.size() > kMaxValue) return std::nullopt;
  for (char c : v) {
    if (std::isspace(static_cast<unsigned char>(c)) || c == '{' || c == '}' || c == '(' || c == '<' || c == '>' ||
        c == '=' || c == ',' || c == '*' || c == '?' || c == '@' || c == '%' || c == '\\') {
      return std::nullopt;
    }
  }
  if (kNotCode.count(lower(v))) return std::nullopt;
  RefFact ref;
  ref.pos = pos;
  ref.text = v;
  // Source files: handlers/users.py, src/main/java/com/acme/Users.java, WEB-INF/jsp/home.jsp
  if (v.find("://") != std::string::npos) return std::nullopt;
  for (const auto& ext : kSourceExtensions) {
    if (ends_with(lower(v), ext) && v.size() > ext.size()) {
      ref.kind = "file";
      ref.name = v[0] == '/' ? v.substr(1) : v;
      if (ref.name.rfind("./", 0) == 0) ref.name = ref.name.substr(2);
      return ref;
    }
  }
  if (v.find('/') != std::string::npos) {
    // dir/module.function (serverless style)
    auto slash = v.rfind('/');
    auto dot = v.rfind('.');
    if (dot != std::string::npos && dot > slash && is_identifier(v.substr(dot + 1))) {
      std::string module = v.substr(0, dot);
      if (module[0] == '/') module = module.substr(1);
      if (module.rfind("./", 0) == 0) module = module.substr(2);
      ref.kind = "file";
      ref.name = module;
      ref.receiver = v.substr(dot + 1);
      return ref;
    }
    return std::nullopt;
  }
  // C++: ns::Class, ns::Class::method, ::function
  if (v.find("::") != std::string::npos) {
    std::string body = v.rfind("::", 0) == 0 ? v.substr(2) : v;
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
      size_t sep = body.find("::", start);
      parts.push_back(body.substr(start, sep == std::string::npos ? std::string::npos : sep - start));
      if (sep == std::string::npos) break;
      start = sep + 2;
    }
    if (!std::all_of(parts.begin(), parts.end(), [](const std::string& p) { return is_identifier(p); })) return std::nullopt;
    if (is_upper_start(parts.back())) {
      ref.kind = "class";
      ref.name = body;
    } else {
      ref.kind = "qualified";
      ref.name = body;
      ref.receiver = parts.back();
    }
    return ref;
  }
  // Class#method, module:function, inner classes Outer$Inner
  std::string norm = v;
  std::string member;
  if (auto hash = norm.find('#'); hash != std::string::npos) {
    member = norm.substr(hash + 1);
    norm = norm.substr(0, hash);
    if (!is_identifier(member)) return std::nullopt;
  } else if (auto colon = norm.find(':'); colon != std::string::npos) {
    if (norm.find(':', colon + 1) != std::string::npos) return std::nullopt;
    member = norm.substr(colon + 1);
    norm = norm.substr(0, colon);
    if (!is_identifier(member)) return std::nullopt;
  }
  std::replace(norm.begin(), norm.end(), '$', '.');
  std::vector<std::string> parts = split_any(norm, ".");
  if (parts.empty() || !std::all_of(parts.begin(), parts.end(), [](const std::string& p) { return is_identifier(p); })) {
    return std::nullopt;
  }
  if (std::count(norm.begin(), norm.end(), '.') + 1 != static_cast<long>(parts.size())) return std::nullopt;
  if (!member.empty()) {
    ref.kind = "qualified";
    ref.name = norm + "." + member;
    ref.receiver = member;
    return ref;
  }
  if (parts.size() == 1) {
    if (!simple) return std::nullopt;
    ref.kind = is_upper_start(parts[0]) ? "class" : "name";
    ref.name = parts[0];
    return ref;
  }
  if (is_upper_start(parts.back())) {
    ref.kind = "class";
    ref.name = norm;
  } else {
    ref.kind = "qualified";
    ref.name = norm;
    ref.receiver = parts.back();
  }
  return ref;
}

// A class name in a Java-centric descriptor (web.xml, Struts, Spring): the value as written.
std::optional<RefFact> class_ref(const std::string& raw, Pos pos) {
  std::string v = trim(raw);
  if (v.empty()) return std::nullopt;
  auto ref = code_ref(v, pos, true);
  if (!ref) return std::nullopt;
  if (ref->kind == "name") ref->kind = "class";  // lower-case class names are unusual but legal
  if (ref->kind == "qualified" && ref->receiver == v.substr(v.rfind('.') + 1)) {
    ref->kind = "class";  // com.acme.legacy.users (lower-case class name)
    ref->receiver.clear();
  }
  return ref;
}

bool looks_like_route_path(std::string_view p) {
  if (p.empty() || p[0] != '/' || p.size() > 500) return false;
  if (p.rfind("//", 0) == 0) return false;
  for (char c : p) {
    if (std::isspace(static_cast<unsigned char>(c))) return false;
  }
  return true;
}

std::string join_path(const std::string& prefix, const std::string& path) {
  if (prefix.empty() || prefix == "/") return path.empty() ? "/" : (path[0] == '/' || path[0] == '*' ? path : "/" + path);
  std::string p = prefix.back() == '/' ? prefix.substr(0, prefix.size() - 1) : prefix;
  if (p[0] != '/') p = "/" + p;
  if (path.empty() || path == "/") return p;
  return p + (path[0] == '/' ? path : "/" + path);
}

std::optional<std::string> http_verb(std::string_view raw) {
  std::string v = upper(trim(raw));
  static const std::unordered_set<std::string> kVerbs = {"GET", "POST", "PUT", "DELETE", "PATCH", "HEAD", "OPTIONS", "TRACE"};
  if (kVerbs.count(v)) return v;
  if (v == "*" || v == "ANY" || v == "ALL") return std::string("ANY");
  return std::nullopt;
}

const std::vector<std::string> kVerbKeys = {"method", "methods", "httpmethod", "httpmethods", "verb", "verbs",
                                            "requestmethod", "requestmethods"};

std::vector<std::string> verbs_of(const ConfigNode& n) {
  std::vector<std::string> out;
  for (const auto& key : kVerbKeys) {
    for (const auto& v : values_of(n, key)) {
      for (const auto& part : split_any(v, ",|; []")) {
        if (auto verb = http_verb(part); verb && std::find(out.begin(), out.end(), *verb) == out.end()) out.push_back(*verb);
      }
    }
  }
  return out;
}

// Servlet url-pattern matching (exact, /prefix/*, *.ext, /*) of a filter
// pattern against a servlet mapping pattern.
bool servlet_pattern_covers(const std::string& filter, const std::string& route) {
  if (filter == "/*" || filter == route) return true;
  if (filter.size() >= 2 && ends_with(filter, "/*")) {
    std::string prefix = filter.substr(0, filter.size() - 2);
    return route == prefix || route.rfind(prefix + "/", 0) == 0;
  }
  if (filter.rfind("*.", 0) == 0) return ends_with(route, filter.substr(1));
  return false;
}

// ---- recognizers -------------------------------------------------------------------

class ConfigExtractor {
 public:
  ConfigExtractor(const ExtractInput& in, FileFacts& out) : in_(in), out_(out) {}

  void run_xml(const ConfigNode& root, const std::string& doctype) {
    std::string name = lower(root.name);
    if (name == "web-app" || name == "web-fragment") {
      web_xml(root);
      out_.entries.push_back({"deployment_descriptor", "servlet container deployment descriptor (" + file_name() + ")", 0, 1});
      return;
    }
    if (name == "struts" || doctype.find("Struts Configuration 2") != std::string::npos) {
      struts2(root);
    } else if (name == "struts-config") {
      struts1(root);
    } else if (name == "beans") {
      spring_beans(root);
    } else {
      generic(root, "", 0);
    }
    add_config_entry();
  }

  void run_yaml(const std::vector<ConfigNode>& docs) {
    for (const auto& doc : docs) {
      if (doc.kind != ConfigNode::Kind::Mapping) {
        generic(doc, "", 0);
      } else if ((child_of(doc, "openapi") || child_of(doc, "swagger")) && child_of(doc, "paths")) {
        openapi(doc);
      } else if (child_of(doc, "functions") && (child_of(doc, "provider") || child_of(doc, "service"))) {
        serverless(doc);
      } else {
        generic(doc, "", 0);
      }
    }
    add_config_entry();
  }

 private:
  const ExtractInput& in_;
  FileFacts& out_;
  std::unordered_map<std::string, int> frameworks_;  // routes per framework

  std::string file_name() const { return paths::base_name(in_.rel_path); }

  void add_config_entry() {
    if (out_.routes.empty()) return;
    std::string reason;
    for (const auto& [fw, n] : frameworks_) {
      if (!reason.empty()) reason += ", ";
      reason += std::to_string(n) + " " + fw;
    }
    out_.entries.push_back({"route_config", "configuration file declares routes (" + reason + ")", 0, 1});
  }

  bool full() const { return out_.routes.size() >= kMaxRoutes; }

  RouteFact& add_route(const std::string& framework, const std::string& method, const std::string& path,
                       const std::string& registrar, Pos pos, const std::string& confidence) {
    RouteFact r;
    r.framework = framework;
    r.method = method;
    r.path = path;
    r.registrar = registrar;
    r.scope = 0;
    r.pos = pos;
    r.confidence = confidence;
    out_.routes.push_back(std::move(r));
    ++frameworks_[framework];
    return out_.routes.back();
  }

  static void set_handler(RouteFact& r, const RefFact& ref) {
    r.has_handler = true;
    r.handler = ref;
  }

  // An edge from configuration to the code it wires: the container creates the
  // class (`member` empty) or invokes `member` on it.
  void configures(int from, const RefFact& cls, const std::string& member, const std::string& what) {
    CallFact c;
    c.caller = from;
    c.kind = "configures";
    c.receiver_kind = cls.kind == "bean" ? "config_bean" : "config_class";
    c.receiver = cls.name;
    std::string simple = cls.name.substr(cls.name.find_last_of(".:") == std::string::npos ? 0 : cls.name.find_last_of(".:") + 1);
    c.name = member.empty() ? simple : member;
    c.text = ts::clip(what, 120);
    c.pos = cls.pos;
    c.arg_count = -1;
    if (!c.name.empty()) out_.calls.push_back(std::move(c));
  }

  // ---- web.xml ---------------------------------------------------------------------

  void web_xml(const ConfigNode& root) {
    struct Servlet {
      std::optional<RefFact> cls;
      std::string jsp;
      Pos pos;
    };
    std::unordered_map<std::string, Servlet> servlets;
    std::unordered_map<std::string, RefFact> filters;
    struct FilterMap {
      std::string filter;
      std::vector<std::string> patterns, servlets;
    };
    std::vector<FilterMap> filter_maps;
    for (const auto& c : root.children) {
      std::string tag = lower(c.name);
      if (tag == "servlet") {
        std::string name = value_or(c, "servlet-name");
        Servlet s;
        s.pos = c.pos;
        if (const std::string* cls = value_of(c, "servlet-class")) {
          s.cls = class_ref(*cls, c.pos);
          if (s.cls) {
            configures(0, *s.cls, "", "servlet '" + name + "' (" + *cls + ")");
            std::string startup = value_or(c, "load-on-startup");
            if (!startup.empty() && startup[0] != '-') {
              configures(0, *s.cls, "init", "servlet '" + name + "' is loaded on startup: init()");
            }
          }
        }
        s.jsp = value_or(c, "jsp-file");
        if (!name.empty()) servlets[name] = std::move(s);
      } else if (tag == "filter") {
        std::string name = value_or(c, "filter-name");
        if (const std::string* cls = value_of(c, "filter-class")) {
          if (auto ref = class_ref(*cls, c.pos)) {
            configures(0, *ref, "", "filter '" + name + "' (" + *cls + ")");
            if (!name.empty()) filters[name] = *ref;
          }
        }
      } else if (tag == "listener") {
        if (const std::string* cls = value_of(c, "listener-class")) {
          if (auto ref = class_ref(*cls, c.pos)) {
            configures(0, *ref, "", "listener " + *cls);
            configures(0, *ref, "contextInitialized", "listener " + *cls + ": contextInitialized() at startup");
          }
        }
      } else if (tag == "filter-mapping") {
        filter_maps.push_back({value_or(c, "filter-name"), values_of(c, "url-pattern"), values_of(c, "servlet-name")});
      }
    }
    for (const auto& c : root.children) {
      if (lower(c.name) != "servlet-mapping" || full()) continue;
      std::string name = value_or(c, "servlet-name");
      for (const auto& raw : values_of(c, "url-pattern")) {
        std::string pattern = trim(raw);
        std::string path = pattern.empty() ? "/" : (pattern[0] == '/' || pattern[0] == '*' ? pattern : "/" + pattern);
        RouteFact& r = add_route("servlet", "ANY", path, name, c.pos, "recognized");
        auto it = servlets.find(name);
        if (it == servlets.end()) {
          r.note = "servlet '" + name + "' is not declared in this descriptor";
        } else if (it->second.cls) {
          set_handler(r, *it->second.cls);
        } else if (!it->second.jsp.empty()) {
          RefFact jsp;
          jsp.kind = "file";
          jsp.name = it->second.jsp[0] == '/' ? it->second.jsp.substr(1) : it->second.jsp;
          jsp.text = it->second.jsp;
          jsp.pos = it->second.pos;
          set_handler(r, jsp);
        } else {
          r.note = "servlet '" + name + "' declares no servlet-class or jsp-file";
        }
        // Filters run before the servlet in declaration order.
        for (const auto& fm : filter_maps) {
          bool applies = std::find(fm.servlets.begin(), fm.servlets.end(), name) != fm.servlets.end() ||
                         std::find(fm.servlets.begin(), fm.servlets.end(), "*") != fm.servlets.end();
          for (const auto& fp : fm.patterns) applies = applies || servlet_pattern_covers(trim(fp), pattern);
          if (!applies) continue;
          auto f = filters.find(fm.filter);
          RefFact mw;
          mw.pos = c.pos;
          if (f != filters.end()) {
            mw = f->second;
            mw.receiver = "doFilter";
            mw.text = "filter '" + fm.filter + "' (" + f->second.name + ")";
          } else {
            mw.kind = "other";
            mw.name = fm.filter;
            mw.text = "filter '" + fm.filter + "' (not declared in this descriptor)";
          }
          r.middleware.push_back(std::move(mw));
        }
      }
    }
  }

  // ---- Struts ------------------------------------------------------------------------

  void struts2(const ConfigNode& root) {
    for (const auto& pkg : root.children) {
      if (lower(pkg.name) != "package") continue;
      std::string ns = value_or(pkg, "namespace");
      std::string pkg_name = value_or(pkg, "name");
      for (const auto& a : pkg.children) {
        if (lower(a.name) != "action" || full()) continue;
        std::string name = value_or(a, "name");
        if (name.empty()) continue;
        RouteFact& r = add_route("struts2", "ANY", join_path(ns, name), pkg_name.empty() ? "struts" : pkg_name, a.pos,
                                 "recognized");
        std::string method = value_or(a, "method");
        if (const std::string* cls = value_of(a, "class")) {
          if (auto ref = class_ref(*cls, a.pos)) {
            if (!method.empty() && method.find('{') == std::string::npos) ref->receiver = method;
            set_handler(r, *ref);
          }
        } else {
          r.note = "no action class: Struts uses ActionSupport, which only returns its result";
        }
        if (method.find('{') != std::string::npos || name.find('*') != std::string::npos) {
          r.note = "wildcard action mapping: the method is chosen from the request URL at runtime";
        }
      }
    }
  }

  void struts1(const ConfigNode& root) {
    const ConfigNode* mappings = child_of(root, "action-mappings");
    if (!mappings) return;
    for (const auto& a : mappings->children) {
      if (lower(a.name) != "action" || full()) continue;
      std::string path = value_or(a, "path");
      if (path.empty()) continue;
      RouteFact& r = add_route("struts", "ANY", path[0] == '/' ? path : "/" + path, "action-mappings", a.pos, "recognized");
      if (const std::string* type = value_of(a, "type")) {
        if (auto ref = class_ref(*type, a.pos)) set_handler(r, *ref);
        std::string param = value_or(a, "parameter");
        if (!param.empty() && type->find("Dispatch") != std::string::npos) {
          r.note = "dispatch action: the method is named by request parameter '" + param + "' at runtime";
        }
      } else if (const std::string* fwd = value_of(a, "forward")) {
        RefFact view;
        view.kind = "file";
        view.name = (*fwd)[0] == '/' ? fwd->substr(1) : *fwd;
        view.text = *fwd;
        view.pos = a.pos;
        set_handler(r, view);
      }
    }
  }

  // ---- Spring XML ----------------------------------------------------------------------

  void spring_beans(const ConfigNode& root) {
    std::unordered_map<std::string, RefFact> bean_class;  // id or alias -> class
    std::unordered_map<std::string, int> bean_symbol;
    auto names_of = [](const ConfigNode& b) {
      std::vector<std::string> names;
      if (const std::string* id = value_of(b, "id")) names.push_back(trim(*id));
      if (const std::string* nm = value_of(b, "name")) {
        for (auto& n : split_any(*nm, ",; ")) names.push_back(n);
      }
      return names;
    };
    // Beans first, so URL mappings and references within the file can use them.
    for (const auto& b : root.children) {
      if (lower(b.name) != "bean") continue;
      auto names = names_of(b);
      const std::string* cls = value_of(b, "class");
      std::optional<RefFact> ref = cls ? class_ref(*cls, b.pos) : std::nullopt;
      std::string id;
      for (const auto& n : names) {
        if (n.empty() || n[0] == '/') continue;
        id = n;
        break;
      }
      int sym = 0;
      if (!id.empty()) {
        SymbolFact s;
        s.name = id;
        s.qualified_name = id;
        s.kind = "bean";
        s.parent = 0;
        s.span = {b.pos, b.pos};
        s.exported = true;
        s.container = cls ? trim(*cls) : "";
        s.signature = cls ? "bean " + id + " : " + trim(*cls) : "bean " + id;
        out_.symbols.push_back(std::move(s));
        sym = static_cast<int>(out_.symbols.size()) - 1;
      }
      for (const auto& n : names) {
        if (ref) bean_class[n] = *ref;
        if (sym > 0) bean_symbol[n] = sym;
      }
      if (ref) {
        configures(sym, *ref, "", "bean " + (id.empty() ? std::string("(anonymous)") : "'" + id + "'") + " (" + ref->name + ")");
        if (const std::string* init = value_of(b, "init-method")) {
          if (is_identifier(trim(*init))) configures(sym, *ref, trim(*init), "bean init-method " + trim(*init) + "()");
        }
      }
      // Injected collaborators: <property ref="..."/>, <property><ref bean="..."/></property>, constructor-arg.
      for (const auto& p : b.children) {
        std::string tag = lower(p.name);
        if (tag != "property" && tag != "constructor-arg") continue;
        std::vector<std::string> refs;
        if (const std::string* r = value_of(p, "ref")) refs.push_back(trim(*r));
        for (const auto& inner : p.children) {
          if (lower(inner.name) != "ref") continue;
          for (const char* k : {"bean", "local", "parent"}) {
            if (const std::string* r = value_of(inner, k)) refs.push_back(trim(*r));
          }
        }
        for (const auto& r : refs) {
          if (r.empty()) continue;
          RefFact target;
          target.kind = "bean";
          target.name = r;
          target.text = r;
          target.pos = p.pos;
          configures(sym, target, "", "injects bean '" + r + "' into " + (tag == "property" ? "property '" + value_or(p, "name") + "'" : std::string("constructor")));
        }
      }
    }
    auto handler_for_bean = [&](const std::string& bean, Pos pos) {
      RefFact ref;
      auto it = bean_class.find(bean);
      if (it != bean_class.end()) {
        ref = it->second;
      } else {
        ref.kind = "bean";
        ref.name = bean;
      }
      ref.text = bean;
      ref.pos = pos;
      return ref;
    };
    for (const auto& b : root.children) {
      std::string tag = lower(b.name);
      if (tag == "view-controller") {
        std::string path = value_or(b, "path");
        if (!path.empty() && !full()) {
          RouteFact& r = add_route("spring_xml", "GET", path, "mvc:view-controller", b.pos, "recognized");
          r.note = "view controller: renders view '" + value_or(b, "view-name") + "' without a handler class";
        }
        continue;
      }
      if (tag != "bean") continue;
      // BeanNameUrlHandlerMapping: <bean name="/hello.htm" class="...">
      for (const auto& n : names_of(b)) {
        if (n.empty() || n[0] != '/' || full()) continue;
        RouteFact& r = add_route("spring_xml", "ANY", n, "bean name", b.pos, "recognized");
        if (const std::string* cls = value_of(b, "class")) {
          if (auto ref = class_ref(*cls, b.pos)) set_handler(r, *ref);
        }
      }
      // SimpleUrlHandlerMapping: <property name="mappings"> / <property name="urlMap">
      std::string cls = value_or(b, "class");
      if (cls.find("UrlHandlerMapping") == std::string::npos) continue;
      for (const auto& p : b.children) {
        if (lower(p.name) != "property") continue;
        for (const auto& holder : p.children) {
          std::string h = lower(holder.name);
          if (h == "props") {
            for (const auto& prop : holder.children) {
              std::string key = value_or(prop, "key");
              if (lower(prop.name) != "prop" || !looks_like_route_path(key) || full()) continue;
              RouteFact& r = add_route("spring_xml", "ANY", key, "SimpleUrlHandlerMapping", prop.pos, "recognized");
              if (!prop.text.empty()) set_handler(r, handler_for_bean(prop.text, prop.pos));
            }
          } else if (h == "map") {
            for (const auto& e : holder.children) {
              std::string key = value_or(e, "key");
              if (lower(e.name) != "entry" || !looks_like_route_path(key) || full()) continue;
              RouteFact& r = add_route("spring_xml", "ANY", key, "SimpleUrlHandlerMapping", e.pos, "recognized");
              std::string bean = value_or(e, "value-ref");
              for (const auto& inner : e.children) {
                if (lower(inner.name) == "ref" && bean.empty()) bean = value_or(inner, "bean");
              }
              if (!bean.empty()) set_handler(r, handler_for_bean(bean, e.pos));
            }
          } else if (h == "value") {
            // Properties text: /hello.htm=helloController
            for (const auto& line : split_any(holder.text, "\n")) {
              auto eq = line.find('=');
              if (eq == std::string::npos || full()) continue;
              std::string key = trim(line.substr(0, eq));
              std::string bean = trim(line.substr(eq + 1));
              if (!looks_like_route_path(key) || bean.empty()) continue;
              RouteFact& r = add_route("spring_xml", "ANY", key, "SimpleUrlHandlerMapping", holder.pos, "recognized");
              set_handler(r, handler_for_bean(bean, holder.pos));
            }
          }
        }
      }
    }
  }

  // ---- OpenAPI / Swagger -------------------------------------------------------------

  void openapi(const ConfigNode& doc) {
    std::string base = value_or(doc, "basePath");
    if (const ConfigNode* servers = child_of(doc, "servers"); servers && !servers->children.empty()) {
      std::string url = value_or(servers->children[0], "url");
      auto scheme = url.find("://");
      if (scheme != std::string::npos) {
        auto slash = url.find('/', scheme + 3);
        url = slash == std::string::npos ? "" : url.substr(slash);
      }
      if (base.empty() && looks_like_route_path(url)) base = url;
    }
    const ConfigNode* paths = child_of(doc, "paths");
    for (const auto& p : paths->children) {
      if (!looks_like_route_path(p.name) || p.kind != ConfigNode::Kind::Mapping) continue;
      std::string path_controller = value_or(p, "x-openapi-router-controller", value_or(p, "x-swagger-router-controller"));
      for (const auto& op : p.children) {
        auto verb = http_verb(op.name);
        if (!verb || *verb == "ANY" || op.kind != ConfigNode::Kind::Mapping || full()) continue;
        RouteFact& r = add_route("openapi", *verb, join_path(base, p.name), "paths", op.pos, "recognized");
        std::string op_id = value_or(op, "operationId");
        std::string controller =
            value_or(op, "x-openapi-router-controller", value_or(op, "x-swagger-router-controller", path_controller));
        if (op_id.empty()) {
          r.note = "operation has no operationId";
          continue;
        }
        std::optional<RefFact> ref = code_ref(controller.empty() ? op_id : controller + "." + op_id, op.pos, true);
        if (!ref) {
          r.note = "operationId '" + op_id + "' does not name code";
          continue;
        }
        if (ref->kind == "name" || ref->kind == "class") {
          ref->kind = "name";
          r.confidence = "heuristic";
          r.note = "operationId '" + op_id + "' is matched by function name";
        }
        set_handler(r, *ref);
      }
    }
  }

  // ---- serverless.yml ----------------------------------------------------------------

  void serverless(const ConfigNode& doc) {
    const ConfigNode* functions = child_of(doc, "functions");
    for (const auto& fn : functions->children) {
      if (fn.kind != ConfigNode::Kind::Mapping) continue;
      std::string handler = value_or(fn, "handler");
      const ConfigNode* events = child_of(fn, "events");
      if (!events || events->kind != ConfigNode::Kind::Sequence) continue;
      for (const auto& ev : events->children) {
        for (const auto& kind : {"http", "httpApi"}) {
          const ConfigNode* http = child_of(ev, kind);
          if (!http || full()) continue;
          std::string method = "ANY", path;
          if (http->kind == ConfigNode::Kind::Scalar) {
            auto parts = split_any(http->text, " ");
            if (parts.size() == 2) {
              method = http_verb(parts[0]).value_or("ANY");
              path = parts[1];
            } else if (parts.size() == 1) {
              path = parts[0];
            }
          } else {
            path = value_or(*http, "path");
            method = http_verb(value_or(*http, "method", "ANY")).value_or("ANY");
          }
          if (path.empty() || path == "*") continue;
          RouteFact& r = add_route("serverless", method, path[0] == '/' ? path : "/" + path, fn.name, ev.pos, "recognized");
          std::string h = handler;
          // Java handlers: com.acme.Handler or com.acme.Handler::handleRequest
          if (auto sep = h.find("::"); sep != std::string::npos && h.find('.') != std::string::npos && h.find('/') == std::string::npos) {
            h = h.substr(0, sep) + "#" + h.substr(sep + 2);
          }
          if (auto ref = code_ref(h, fn.pos, false)) {
            set_handler(r, *ref);
          } else {
            r.note = "handler '" + handler + "' does not name code";
          }
        }
      }
    }
  }

  // ---- generic: an entry pairing a URL path with a handler reference ------------------

  const std::vector<std::string> kPathKeys = {"path", "url", "uri", "route", "pattern", "urlpattern", "endpoint",
                                              "mapping", "urlmapping", "requestmapping"};
  const std::vector<std::string> kStrongHandlerKeys = {
      "handler",  "handlerclass", "controller",  "controllerclass", "servlet", "servletclass", "action",
      "actionclass", "class",     "classname",   "callback",        "function", "func",        "operationid",
      "view",     "viewclass",    "resource",    "resourceclass",   "target",  "bean",         "impl",
      "implementation", "executor", "processor", "command",         "handlerref", "endpointclass", "delegate"};
  const std::vector<std::string> kWeakHandlerKeys = {"type", "service", "component", "provider"};
  const std::vector<std::string> kPrefixKeys = {"prefix", "basepath", "base", "contextpath", "mount", "root"};

  // Returns the handler reference and the key it was found under.
  std::optional<std::pair<RefFact, std::string>> handler_in(const ConfigNode& n) const {
    for (const auto& key : kStrongHandlerKeys) {
      for (const auto& [k, v] : n.attrs) {
        if (norm_key(k) == key) {
          if (auto ref = code_ref(v, n.pos, true)) return std::make_pair(*ref, k);
        }
      }
      for (const auto& c : n.children) {
        if (!c.name.empty() && is_leaf(c) && norm_key(c.name) == key) {
          if (auto ref = code_ref(c.text, c.pos, true)) return std::make_pair(*ref, c.name);
        }
      }
    }
    for (const auto& key : kWeakHandlerKeys) {
      const std::string* v = value_of(n, key);
      if (!v) continue;
      auto ref = code_ref(*v, n.pos, false);
      if (ref && (ref->kind == "class" || ref->kind == "qualified")) return std::make_pair(*ref, key);
    }
    return std::nullopt;
  }

  void generic(const ConfigNode& n, const std::string& prefix, int depth) {
    if (depth > kMaxDepth || full()) return;
    std::string inner_prefix = prefix;
    for (const auto& key : kPrefixKeys) {
      const std::string* v = value_of(n, key);
      if (v && looks_like_route_path(*v)) {
        inner_prefix = join_path(prefix, *v);
        break;
      }
    }
    bool emitted = false;
    for (const auto& key : kPathKeys) {
      const std::string* path = value_of(n, key);
      if (!path || !looks_like_route_path(*path)) continue;
      auto handler = handler_in(n);
      if (!handler) break;
      std::vector<std::string> verbs = verbs_of(n);
      if (verbs.empty()) verbs.push_back("ANY");
      for (const auto& verb : verbs) {
        RouteFact& r = add_route("config", verb, join_path(prefix, *path), n.name.empty() ? file_name() : n.name, n.pos,
                                 "heuristic");
        set_handler(r, handler->first);
        r.note = "declared in " + file_name() + " by '" + key + "' and '" + handler->second + "'";
      }
      emitted = true;
      break;
    }
    // Path-keyed maps: { "/users": com.acme.Users, "/orders": { post: ..., handler: ... } }
    if (!emitted && (n.kind == ConfigNode::Kind::Mapping || n.kind == ConfigNode::Kind::Element)) {
      for (const auto& c : n.children) {
        if (!looks_like_route_path(c.name) || full()) continue;
        std::string path = join_path(inner_prefix, c.name);
        if (c.kind == ConfigNode::Kind::Scalar) {
          if (auto ref = code_ref(c.text, c.pos, true)) {
            RouteFact& r = add_route("config", "ANY", path, n.name.empty() ? file_name() : n.name, c.pos, "heuristic");
            set_handler(r, *ref);
            r.note = "declared in " + file_name() + " as '" + c.name + "'";
          }
        } else if (c.kind == ConfigNode::Kind::Mapping) {
          if (auto h = handler_in(c)) {
            std::vector<std::string> verbs = verbs_of(c);
            if (verbs.empty()) verbs.push_back("ANY");
            for (const auto& verb : verbs) {
              RouteFact& r = add_route("config", verb, path, c.name, c.pos, "heuristic");
              set_handler(r, h->first);
              r.note = "declared in " + file_name() + " under '" + c.name + "'";
            }
          } else {
            for (const auto& op : c.children) {
              auto verb = http_verb(op.name);
              if (!verb || full()) continue;
              std::optional<RefFact> ref;
              if (op.kind == ConfigNode::Kind::Scalar) ref = code_ref(op.text, op.pos, true);
              else if (auto h2 = handler_in(op)) ref = h2->first;
              if (!ref) continue;
              RouteFact& r = add_route("config", *verb, path, c.name, op.pos, "heuristic");
              set_handler(r, *ref);
              r.note = "declared in " + file_name() + " under '" + c.name + "'";
            }
          }
        }
      }
    }
    for (const auto& c : n.children) {
      if (c.kind == ConfigNode::Kind::Mapping || c.kind == ConfigNode::Kind::Sequence ||
          (c.kind == ConfigNode::Kind::Element && (!c.children.empty() || !c.attrs.empty()))) {
        if (looks_like_route_path(c.name)) continue;  // handled above
        generic(c, inner_prefix, depth + 1);
      }
    }
  }
};

}  // namespace

void extract_xml(const ExtractInput& in, FileFacts& out) {
  add_module_symbol(in, out);
  ConfigNode root;
  std::string doctype;
  XmlTree tree(in.source);
  if (!tree.build(in.root, root, doctype)) return;
  ConfigExtractor x(in, out);
  x.run_xml(root, doctype);
}

void extract_yaml(const ExtractInput& in, FileFacts& out) {
  add_module_symbol(in, out);
  std::vector<ConfigNode> docs;
  YamlTree tree(in.source);
  tree.build(in.root, docs);
  ConfigExtractor x(in, out);
  x.run_yaml(docs);
}

}  // namespace proj
