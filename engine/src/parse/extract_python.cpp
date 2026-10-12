// Python extractor.
//
// Extracts functions, classes and methods, imports (absolute, relative,
// aliased, star), calls, constructor/annotation type hints, raise/except
// sites, `if __name__ == "__main__"` guards, and route registrations: Flask
// (decorators, add_url_rule, MethodView), FastAPI, Django (path/re_path/url,
// class-based views, include() prefixes), Tornado and webapp2 handler lists,
// web.py URL tuples, Falcon add_route, Pyramid add_route/add_view/@view_config
// and Bottle decorators. Handler classes are dispatched to their HTTP method
// methods (get/post, on_get, GET) by the resolver.
#include <cctype>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/paths.hpp"
#include "parse/extractors.hpp"
#include "parse/route_util.hpp"
#include "parse/ts_util.hpp"

namespace proj {
namespace {

using ts::field;
using ts::is;

const std::unordered_set<std::string_view> kBuiltins = {
    "print", "len", "range", "str", "int", "float", "bool", "list", "dict", "set", "tuple", "frozenset", "bytes",
    "isinstance", "issubclass", "hasattr", "getattr", "setattr", "delattr", "type", "enumerate", "zip", "map",
    "filter", "sorted", "reversed", "min", "max", "sum", "any", "all", "open", "repr", "iter", "next", "id", "abs",
    "round", "format", "vars", "dir", "callable", "hash", "object", "super", "input", "divmod", "pow", "ord", "chr",
    "hex", "oct", "bin", "globals", "locals", "staticmethod", "classmethod", "property", "slice", "memoryview",
    "bytearray", "complex", "compile", "eval", "exec", "breakpoint", "aiter", "anext"};

const std::unordered_set<std::string_view> kCatchAll = {"Exception", "BaseException"};

struct Scope {
  const void* node;
  int symbol;
};

struct TryCtx {
  const void* body;
  int scope;
  std::vector<int> catches;  // exception fact indexes, one per handled type
};

std::string upper(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

std::string last_part(const std::string& dotted) {
  auto p = dotted.rfind('.');
  return p == std::string::npos ? dotted : dotted.substr(p + 1);
}

// A URL regex as used by Tornado, webapp2, web.py and Django url()/re_path(),
// shown as a route path: "^/users/(?P<id>\d+)/?$" -> "/users/{id}".
std::string regex_path(const std::string& p) {
  std::string out;
  size_t i = 0;
  if (!p.empty() && p[0] == '^') i = 1;
  auto skip_quantifier = [&]() {
    while (i < p.size() && (p[i] == '?' || p[i] == '*' || p[i] == '+')) ++i;
    if (i < p.size() && p[i] == '{') {
      auto close = p.find('}', i);
      if (close != std::string::npos) i = close + 1;
    }
  };
  while (i < p.size()) {
    char c = p[i];
    if (c == '$' && i + 1 == p.size()) break;
    if (c == '/' && i + 1 < p.size() && p[i + 1] == '?') {  // optional slash
      i += 2;
      continue;
    }
    if (c == '\\' && i + 1 < p.size()) {
      char n = p[i + 1];
      i += 2;
      if (std::isalpha(static_cast<unsigned char>(n))) {  // \d, \w, ...: a parameter
        out += "{param}";
        skip_quantifier();
      } else {
        out += n;
      }
      continue;
    }
    if (c == '(') {
      int depth = 1;
      size_t j = i + 1;
      while (j < p.size() && depth > 0) {
        if (p[j] == '\\') {
          j += 2;
          continue;
        }
        if (p[j] == '(') ++depth;
        else if (p[j] == ')') --depth;
        ++j;
      }
      std::string group = p.substr(i, j - i);
      if (group.rfind("(?P<", 0) == 0 && group.find('>') != std::string::npos) {
        out += "{" + group.substr(4, group.find('>') - 4) + "}";
      } else {
        out += "{param}";
      }
      i = j;
      skip_quantifier();
      continue;
    }
    if (c == '[') {
      auto close = p.find(']', i);
      i = close == std::string::npos ? p.size() : close + 1;
      out += "{param}";
      skip_quantifier();
      continue;
    }
    if (c == '.' && i + 1 < p.size() && (p[i + 1] == '*' || p[i + 1] == '+')) {
      out += "*";
      i += 2;
      continue;
    }
    if (c == '?' || c == '+' || c == '*') {
      ++i;
      continue;
    }
    out += c;
    ++i;
  }
  if (out.empty() || out[0] != '/') out = "/" + out;
  std::string clean;
  for (char ch : out) {
    if (ch == '/' && !clean.empty() && clean.back() == '/') continue;
    clean += ch;
  }
  return clean;
}

bool looks_like_regex(const std::string& p) {
  return p.find_first_of("^$()[]\\+") != std::string::npos;
}

class PyExtractor {
 public:
  PyExtractor(const ExtractInput& in, FileFacts& out) : in_(in), out_(out), src_(in.source) {}

  void run() {
    add_module_symbol(in_, out_);
    out_.module_name = module_name(in_.rel_path);
    scopes_.push_back({ts::node_id(in_.root), 0});
    if (paths::base_name(in_.rel_path) == "__main__.py") {
      out_.entries.push_back({"module_main", "package __main__.py module", 0, 1});
    }
    out_.depth_limited = ts::walk(in_.root, [this](TSNode n) { return enter(n); }, [this](TSNode n) { leave(n); });
  }

 private:
  const ExtractInput& in_;
  FileFacts& out_;
  std::string_view src_;
  std::vector<Scope> scopes_;
  std::unordered_map<const void*, TryCtx> pending_try_;
  std::vector<TryCtx> tries_;
  std::vector<std::pair<const void*, std::string>> except_aliases_;
  std::unordered_map<std::string, std::string> routers_;  // variable -> framework
  std::unordered_set<std::string> imported_;  // top-level modules imported by this file
  std::unordered_map<std::string, TSNode> lists_;  // module-level list/tuple assignments (handler lists)
  std::unordered_map<const void*, int> awaiting_receiver_;  // inner call node -> outer call index

  std::string_view text(TSNode n) const { return ts::text(n, src_); }
  int scope() const { return scopes_.back().symbol; }
  const SymbolFact& symbol(int i) const { return out_.symbols[static_cast<size_t>(i)]; }

  static std::string module_name(std::string_view rel) {
    std::string m(rel);
    for (const char* ext : {".pyi", ".py"}) {
      std::string e(ext);
      if (m.size() > e.size() && m.compare(m.size() - e.size(), e.size(), e) == 0) {
        m.resize(m.size() - e.size());
        break;
      }
    }
    for (auto& c : m) {
      if (c == '/') c = '.';
    }
    const std::string init = ".__init__";
    if (m.size() > init.size() && m.compare(m.size() - init.size(), init.size(), init) == 0) m.resize(m.size() - init.size());
    return m;
  }

  int add_symbol(TSNode n, std::string name, std::string kind, int parent, bool push) {
    SymbolFact s;
    s.qualified_name = parent > 0 ? symbol(parent).qualified_name + "." + name : out_.module_name + "." + name;
    s.name = std::move(name);
    s.kind = std::move(kind);
    s.span = ts::span(n);
    s.parent = parent;
    if (!s.name.empty() && s.name[0] == '_' && s.name.rfind("__", 0) != 0) s.visibility = "private";
    out_.symbols.push_back(std::move(s));
    int idx = static_cast<int>(out_.symbols.size()) - 1;
    if (push) scopes_.push_back({ts::node_id(n), idx});
    return idx;
  }

  void add_hint(int sc, std::string name, std::string type, std::string kind) {
    if (name.empty() || type.empty()) return;
    out_.type_hints.push_back({sc, std::move(name), std::move(type), std::move(kind)});
  }

  int enclosing_class() const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      if (symbol(it->symbol).kind == "class") return it->symbol;
    }
    return -1;
  }

  std::optional<std::string> string_value(TSNode n) const {
    if (!is(n, "string")) return std::nullopt;
    if (ts::child_of_type(n, "interpolation").id) return std::nullopt;
    std::string out;
    uint32_t c = ts_node_named_child_count(n);
    for (uint32_t i = 0; i < c; ++i) {
      TSNode ch = ts_node_named_child(n, i);
      if (is(ch, "string_content")) out += text(ch);
    }
    return out;
  }

  std::vector<TSNode> positional_args(TSNode call) const {
    std::vector<TSNode> out;
    TSNode list = field(call, "arguments");
    uint32_t c = ts_node_named_child_count(list);
    for (uint32_t i = 0; i < c; ++i) {
      TSNode a = ts_node_named_child(list, i);
      if (!is(a, "keyword_argument") && !is(a, "comment")) out.push_back(a);
    }
    return out;
  }

  TSNode keyword_arg(TSNode call, std::string_view name) const {
    TSNode list = field(call, "arguments");
    uint32_t c = ts_node_named_child_count(list);
    for (uint32_t i = 0; i < c; ++i) {
      TSNode a = ts_node_named_child(list, i);
      if (is(a, "keyword_argument") && text(field(a, "name")) == name) return field(a, "value");
    }
    return TSNode{};
  }

  int arg_count(TSNode call) const {
    TSNode list = field(call, "arguments");
    int count = 0;
    uint32_t c = ts_node_named_child_count(list);
    for (uint32_t i = 0; i < c; ++i) {
      TSNode a = ts_node_named_child(list, i);
      if (is(a, "list_splat") || is(a, "dictionary_splat")) return -1;
      if (!is(a, "comment")) ++count;
    }
    return count;
  }

  std::string annotation(TSNode type_node) const {
    if (ts_node_is_null(type_node)) return "";
    TSNode t = is(type_node, "type") ? ts_node_named_child(type_node, 0) : type_node;
    if (is(t, "identifier") || is(t, "attribute")) return std::string(text(t));
    if (is(t, "generic_type")) {
      TSNode base = ts_node_named_child(t, 0);
      std::string b(text(base));
      if (b == "Optional") {
        TSNode params = ts::child_of_type(t, "type_parameter");
        if (params.id) return annotation(ts_node_named_child(params, 0));
      }
      return b;
    }
    if (is(t, "string")) {
      if (auto v = string_value(t)) return *v;  // forward reference "User"
    }
    return "";
  }

  // `Foo(...)` constructs Foo when the callee looks like a class name.
  std::string constructed_type(TSNode value) const {
    if (!is(value, "call")) return "";
    TSNode fn = field(value, "function");
    if (!is(fn, "identifier") && !is(fn, "attribute")) return "";
    std::string callee(text(fn));
    std::string last = callee.substr(callee.find_last_of('.') == std::string::npos ? 0 : callee.find_last_of('.') + 1);
    if (last.empty() || !std::isupper(static_cast<unsigned char>(last[0]))) return "";
    return callee;
  }

  // ---- traversal --------------------------------------------------------------

  bool enter(TSNode n) {
    if (auto it = pending_try_.find(ts::node_id(n)); it != pending_try_.end()) tries_.push_back(it->second);
    auto t = ts::type(n);
    if (t == "comment" || t == "string") return false;
    if (t == "function_definition") on_function(n);
    else if (t == "class_definition") on_class(n);
    else if (t == "import_statement") on_import(n);
    else if (t == "import_from_statement") on_import_from(n);
    else if (t == "call") on_call(n);
    else if (t == "assignment") on_assignment(n);
    else if (t == "raise_statement") on_raise(n);
    else if (t == "try_statement") on_try(n);
    else if (t == "except_clause") on_except(n);
    else if (t == "if_statement") on_if(n);
    else if (t == "elif_clause") on_if_dispatch(n);
    else if (t == "match_statement") on_match_dispatch(n);
    else if (t == "for_statement") on_for(n);
    else if (t == "lambda") return true;
    return true;
  }

  void leave(TSNode n) {
    const void* id = ts::node_id(n);
    if (scopes_.size() > 1 && scopes_.back().node == id) scopes_.pop_back();
    if (!tries_.empty() && tries_.back().body == id) tries_.pop_back();
    if (!except_aliases_.empty() && except_aliases_.back().first == id) except_aliases_.pop_back();
  }

  void on_function(TSNode n) {
    std::string name(text(field(n, "name")));
    if (name.empty()) return;
    int parent = scope();
    bool method = symbol(parent).kind == "class";
    std::string kind = method ? (name == "__init__" ? "constructor" : "method") : "function";
    int idx = add_symbol(n, name, kind, parent, true);
    auto& s = out_.symbols[static_cast<size_t>(idx)];
    if (parent == 0 && name[0] != '_') s.exported = true;
    int params = 0;
    bool variadic = false;
    TSNode list = field(n, "parameters");
    uint32_t c = ts_node_named_child_count(list);
    for (uint32_t i = 0; i < c; ++i) {
      TSNode p = ts_node_named_child(list, i);
      std::string pname, ptype;
      if (is(p, "identifier")) pname = std::string(text(p));
      else if (is(p, "typed_parameter")) {
        TSNode id = ts::child_of_type(p, "identifier");
        pname = std::string(text(id));
        ptype = annotation(field(p, "type"));
        if (!id.id) variadic = true;
      } else if (is(p, "default_parameter") || is(p, "typed_default_parameter")) {
        pname = std::string(text(field(p, "name")));
        ptype = annotation(field(p, "type"));
      } else if (is(p, "list_splat_pattern") || is(p, "dictionary_splat_pattern")) {
        variadic = true;
        pname = std::string(text(ts::child_of_type(p, "identifier")));
      }
      if (pname.empty()) continue;
      if (method && i == 0 && (pname == "self" || pname == "cls")) continue;
      if (!is(p, "list_splat_pattern") && !is(p, "dictionary_splat_pattern")) ++params;
      add_hint(idx, pname, ptype.empty() ? "?" : ptype, "param");
    }
    out_.symbols[static_cast<size_t>(idx)].param_count = variadic ? -1 : params;
    std::string returns = annotation(field(n, "return_type"));
    if (!returns.empty() && returns != "None") add_hint(idx, "#return", returns, "return");
    for (TSNode d : decorators(n)) {
      std::string dt(text(d));
      if (dt == "@staticmethod" || dt == "@classmethod") out_.symbols[static_cast<size_t>(idx)].is_static = true;
    }
    on_route_decorators(n, idx);
  }

  std::vector<TSNode> decorators(TSNode def) const {
    std::vector<TSNode> out;
    TSNode parent = ts_node_parent(def);
    if (!is(parent, "decorated_definition")) return out;
    uint32_t c = ts_node_named_child_count(parent);
    for (uint32_t i = 0; i < c; ++i) {
      TSNode d = ts_node_named_child(parent, i);
      if (is(d, "decorator")) out.push_back(d);
    }
    return out;
  }

  void on_class(TSNode n) {
    std::string name(text(field(n, "name")));
    if (name.empty()) return;
    int idx = add_symbol(n, name, "class", scope(), true);
    if (symbol(idx).parent == 0 && name[0] != '_') out_.symbols[static_cast<size_t>(idx)].exported = true;
    TSNode bases = field(n, "superclasses");
    uint32_t c = bases.id ? ts_node_named_child_count(bases) : 0;
    for (uint32_t i = 0; i < c; ++i) {
      TSNode b = ts_node_named_child(bases, i);
      if (is(b, "identifier") || is(b, "attribute")) {
        out_.inherits.push_back({idx, std::string(text(b)), "extends", ts::start_pos(b)});
      }
    }
  }

  // ---- imports ------------------------------------------------------------------

  void on_import(TSNode n) {
    uint32_t c = ts_node_named_child_count(n);
    for (uint32_t i = 0; i < c; ++i) {
      TSNode item = ts_node_named_child(n, i);
      ImportFact imp;
      imp.kind = "import";
      imp.pos = ts::start_pos(n);
      imp.scope = scope();
      if (is(item, "dotted_name")) {
        imp.specifier = std::string(text(item));
        imp.bindings.push_back({imp.specifier, "*", "module"});
        imported_.insert(imp.specifier.substr(0, imp.specifier.find('.')));
      } else if (is(item, "aliased_import")) {
        imp.specifier = std::string(text(field(item, "name")));
        imported_.insert(imp.specifier.substr(0, imp.specifier.find('.')));
        imp.bindings.push_back({std::string(text(field(item, "alias"))), "*", "module"});
      } else {
        continue;
      }
      out_.imports.push_back(std::move(imp));
    }
  }

  void on_import_from(TSNode n) {
    ImportFact imp;
    imp.kind = "from_import";
    imp.pos = ts::start_pos(n);
    imp.scope = scope();
    TSNode module = field(n, "module_name");
    imp.specifier = std::string(text(module));
    imported_.insert(imp.specifier.substr(0, imp.specifier.find('.')));
    uint32_t c = ts_node_named_child_count(n);
    for (uint32_t i = 0; i < c; ++i) {
      TSNode item = ts_node_named_child(n, i);
      if (ts::node_id(item) == ts::node_id(module)) continue;
      if (is(item, "dotted_name")) {
        std::string name(text(item));
        imp.bindings.push_back({name, name, "named"});
      } else if (is(item, "aliased_import")) {
        imp.bindings.push_back({std::string(text(field(item, "alias"))), std::string(text(field(item, "name"))), "named"});
      } else if (is(item, "wildcard_import")) {
        imp.bindings.push_back({"*", "*", "wildcard"});
      }
    }
    out_.imports.push_back(std::move(imp));
  }

  // ---- calls ----------------------------------------------------------------

  void on_call(TSNode n) {
    TSNode fn = field(n, "function");
    on_route_call(n, fn);
    CallFact c;
    c.caller = scope();
    c.kind = "calls";
    c.pos = ts::start_pos(n);
    c.arg_count = arg_count(n);
    c.text = ts::short_text(fn, src_);
    if (is(fn, "identifier")) {
      c.name = std::string(text(fn));
      c.receiver_kind = "none";
      try_django_path(n, c.name);
      if (kBuiltins.count(c.name)) return;
    } else if (is(fn, "attribute")) {
      TSNode obj = field(fn, "object");
      c.name = std::string(text(field(fn, "attribute")));
      c.receiver = ts::short_text(obj, src_);
      if (is(obj, "identifier") && (text(obj) == "self" || text(obj) == "cls")) {
        c.receiver_kind = "this";
      } else if (is(obj, "call") && text(field(obj, "function")) == "super") {
        c.receiver_kind = "super";
        c.receiver = "super";
      } else if (is(obj, "identifier")) {
        c.receiver_kind = "identifier";
      } else if (is(obj, "attribute") && is(field(obj, "object"), "identifier") &&
                 (text(field(obj, "object")) == "self" || text(field(obj, "object")) == "cls")) {
        c.receiver_kind = "this_member";
        c.receiver = std::string(text(field(obj, "attribute")));
      } else if (is(obj, "attribute") && dotted(obj)) {
        c.receiver_kind = "qualified";
      } else if (is(obj, "call") || (is(obj, "await") && is(ts_node_named_child(obj, 0), "call"))) {
        c.receiver_kind = "call";  // chained: x.f().g(); resolved through f's return annotation
        if (is(obj, "await")) obj = ts_node_named_child(obj, 0);
      } else {
        c.receiver_kind = "expression";
      }
      on_framework_call(n, obj, c.name);
    } else {
      c.receiver_kind = "expression";
      c.receiver = c.text;
    }
    bool chained = c.receiver_kind == "call";
    out_.calls.push_back(std::move(c));
    int index = static_cast<int>(out_.calls.size()) - 1;
    if (chained) awaiting_receiver_[ts::node_id(field(fn, "object"))] = index;
    if (chained && is(field(fn, "object"), "await")) awaiting_receiver_[ts::node_id(ts_node_named_child(field(fn, "object"), 0))] = index;
    if (auto it = awaiting_receiver_.find(ts::node_id(n)); it != awaiting_receiver_.end()) {
      out_.calls[static_cast<size_t>(it->second)].receiver_call = index;
      awaiting_receiver_.erase(it);
    }
  }

  bool dotted(TSNode n) const {
    while (is(n, "attribute")) n = field(n, "object");
    return is(n, "identifier");
  }

  // register_blueprint / include_router mounts and app.run()/uvicorn.run() starts.
  void on_framework_call(TSNode call, TSNode obj, const std::string& method) {
    std::string receiver(text(obj));
    if (method == "register_blueprint" || method == "include_router") {
      auto args = positional_args(call);
      if (args.empty()) return;
      MountFact m;
      m.framework = method == "register_blueprint" ? "flask" : "fastapi";
      m.registrar = receiver;
      TSNode prefix = keyword_arg(call, method == "register_blueprint" ? "url_prefix" : "prefix");
      if (prefix.id) {
        if (auto v = string_value(prefix)) {
          m.has_prefix = true;
          m.prefix = *v;
        }
      }
      m.scope = scope();
      m.pos = ts::start_pos(call);
      m.target.pos = ts::start_pos(args[0]);
      m.target.text = ts::short_text(args[0], src_);
      if (is(args[0], "identifier")) {
        m.target.kind = "identifier";
        m.target.name = std::string(text(args[0]));
      } else if (is(args[0], "attribute")) {
        m.target.kind = "member";
        m.target.name = std::string(text(field(args[0], "attribute")));
        m.target.receiver = ts::short_text(field(args[0], "object"), src_);
      } else {
        m.target.kind = "other";
      }
      out_.mounts.push_back(std::move(m));
      return;
    }
    if (method == "run" && (routers_.count(receiver) || receiver == "uvicorn" || receiver == "app")) {
      std::string reason = receiver == "uvicorn" ? "uvicorn.run()" : receiver + ".run()";
      out_.entries.push_back({"server_start", reason, scope(), ts::start_pos(call).line});
    }
  }

  // Django: urlpatterns = [path('users/<int:id>/', views.user_detail), re_path(r'^x/$', View.as_view()),
  //                        url(r'^api/', include('app.urls')), ...]
  void try_django_path(TSNode call, const std::string& name) {
    if (name != "path" && name != "re_path" && name != "url") return;
    TSNode list = ts_node_parent(call);
    if (!is(list, "list") && !is(list, "tuple")) return;
    TSNode assign = ts_node_parent(list);
    if (is(assign, "binary_operator")) assign = ts_node_parent(assign);  // urlpatterns += [...]
    if ((!is(assign, "assignment") && !is(assign, "augmented_assignment")) || text(field(assign, "left")) != "urlpatterns") {
      return;
    }
    auto args = positional_args(call);
    if (args.size() < 2) return;
    auto route = string_value(args[0]);
    if (!route) return;
    std::string path = name == "path" ? "/" + *route : regex_path(*route);
    if (is(args[1], "call") && last_part(std::string(text(field(args[1], "function")))) == "include") {
      // include('app.urls') / include(app.urls): routes of that URLconf get this prefix.
      auto inc = positional_args(args[1]);
      if (inc.empty()) return;
      MountFact m;
      m.framework = "django";
      m.registrar = "urlpatterns";
      m.prefix = path;
      m.has_prefix = true;
      m.scope = scope();
      m.pos = ts::start_pos(call);
      m.target.pos = ts::start_pos(inc[0]);
      m.target.text = ts::short_text(inc[0], src_);
      if (auto mod = string_value(inc[0])) {
        m.target.kind = "module";
        m.target.name = *mod;
      } else if (is(inc[0], "identifier") || is(inc[0], "attribute")) {
        m.target.kind = "module";
        m.target.name = std::string(text(inc[0]));
      } else {
        m.target.kind = "other";
      }
      out_.mounts.push_back(std::move(m));
      return;
    }
    RouteFact r;
    r.framework = "django";
    r.method = "ANY";
    r.path = path;
    r.registrar = "urlpatterns";
    r.scope = scope();
    r.pos = ts::start_pos(call);
    r.confidence = "recognized";
    r.has_handler = true;
    r.handler = handler_ref(args[1]);
    out_.routes.push_back(std::move(r));
  }

  // A route handler argument: a function or class name, views.Class.as_view(),
  // a resource object Resource(), or a class named by a string ('module.Class').
  RefFact handler_ref(TSNode h) const {
    RefFact ref;
    ref.pos = ts::start_pos(h);
    ref.text = ts::short_text(h, src_);
    if (is(h, "call")) {
      TSNode f = field(h, "function");
      if (is(f, "attribute") && text(field(f, "attribute")) == "as_view") {
        h = field(f, "object");  // class-based view
      } else if ((is(f, "identifier") || is(f, "attribute")) &&
                 std::isupper(static_cast<unsigned char>(last_part(std::string(text(f)))[0]))) {
        ref.kind = "class";  // a handler object: Resource()
        ref.name = std::string(text(f));
        return ref;
      } else {
        ref.kind = "other";
        return ref;
      }
    }
    if (is(h, "identifier")) {
      ref.kind = "identifier";
      ref.name = std::string(text(h));
    } else if (is(h, "attribute")) {
      ref.kind = "member";
      ref.name = std::string(text(field(h, "attribute")));
      ref.receiver = ts::short_text(field(h, "object"), src_);
    } else if (auto s = string_value(h); s && !s->empty() && s->find(' ') == std::string::npos) {
      bool cls = std::isupper(static_cast<unsigned char>(last_part(*s)[0]));
      ref.kind = cls ? "class" : "qualified";
      ref.name = *s;
      if (!cls) ref.receiver = last_part(*s);
    } else {
      ref.kind = "other";
    }
    return ref;
  }

  std::string framework_from_imports(const std::string& fallback) const {
    for (const char* f : {"tornado", "webapp2", "falcon", "pyramid", "bottle", "cherrypy"}) {
      if (imported_.count(f)) return f;
    }
    return fallback;
  }

  void add_list_route(const std::string& framework, const std::string& method, const std::string& raw_path,
                      TSNode handler, const std::string& registrar, TSNode at, bool recognized) {
    std::string path = looks_like_regex(raw_path) ? regex_path(raw_path) : raw_path;
    if (path.empty() || path[0] != '/') return;
    RouteFact r;
    r.framework = framework;
    r.method = method;
    r.path = path;
    r.registrar = registrar;
    r.scope = scope();
    r.pos = ts::start_pos(at);
    r.confidence = recognized ? "recognized" : "heuristic";
    if (!recognized) r.note = "route list passed to " + registrar;
    r.has_handler = true;
    r.handler = handler_ref(handler);
    out_.routes.push_back(std::move(r));
  }

  // [(r"/path", Handler, ...), url(r"/x", Handler), webapp2.Route('/x', handler=H)]
  void handler_list(TSNode list, const std::string& framework, const std::string& registrar, bool recognized) {
    if (is(list, "identifier")) {
      auto it = lists_.find(std::string(text(list)));
      if (it == lists_.end()) return;
      list = it->second;
    }
    if (!is(list, "list") && !is(list, "tuple")) return;
    uint32_t c = ts_node_named_child_count(list);
    for (uint32_t i = 0; i < c; ++i) {
      TSNode item = ts_node_named_child(list, i);
      TSNode path, handler;
      if (is(item, "tuple") || is(item, "list")) {
        if (ts_node_named_child_count(item) < 2) continue;
        path = ts_node_named_child(item, 0);
        handler = ts_node_named_child(item, 1);
      } else if (is(item, "call")) {
        std::string callee = last_part(std::string(text(field(item, "function"))));
        if (callee != "url" && callee != "URLSpec" && callee != "Route" && callee != "route") continue;
        auto args = positional_args(item);
        if (args.empty()) continue;
        path = args[0];
        handler = args.size() > 1 ? args[1] : keyword_arg(item, "handler");
      } else {
        continue;
      }
      if (!handler.id) continue;
      auto p = string_value(path);
      if (!p) continue;
      add_list_route(framework, "ANY", *p, handler, registrar, item, recognized);
    }
  }

  std::vector<std::string> methods_arg(TSNode call, const char* key) const {
    std::vector<std::string> out;
    TSNode m = keyword_arg(call, key);
    if (!m.id) return out;
    if (auto v = string_value(m)) {
      out.push_back(upper(*v));
    } else if (is(m, "list") || is(m, "tuple")) {
      uint32_t c = ts_node_named_child_count(m);
      for (uint32_t i = 0; i < c; ++i) {
        if (auto v = string_value(ts_node_named_child(m, i))) out.push_back(upper(*v));
      }
    }
    return out;
  }

  // Route registrations made by calls (not decorators).
  void on_route_call(TSNode call, TSNode fn) {
    if (!is(fn, "identifier") && !is(fn, "attribute")) return;
    std::string callee(text(fn));
    std::string last = last_part(callee);
    std::string receiver = is(fn, "attribute") ? std::string(text(field(fn, "object"))) : "";
    auto args = positional_args(call);
    // Standard library servers: HTTPServer(addr, Handler) -> Handler.do_GET/do_POST; make_server(host, port, app)
    if (last == "HTTPServer" || last == "ThreadingHTTPServer" || last == "TCPServer" || last == "ThreadingTCPServer" ||
        last == "make_server") {
      size_t index = last == "make_server" ? 2 : 1;
      if (args.size() > index) {
        add_list_route(last == "make_server" ? "wsgi" : "http_server", "ANY", "/*", args[index], callee, call, true);
      }
      return;
    }
    // tornado.web.Application([...]), webapp2.WSGIApplication([...]), app.add_handlers(host, [...])
    if (last == "Application" || last == "WSGIApplication" || last == "add_handlers") {
      std::string fw = framework_from_imports("python_handlers");
      bool recognized = fw == "tornado" || fw == "webapp2";
      TSNode list = last == "add_handlers" ? (args.size() > 1 ? args[1] : TSNode{}) : (args.empty() ? keyword_arg(call, "handlers") : args[0]);
      if (list.id) handler_list(list, fw, callee, recognized);
      return;
    }
    // Falcon: app.add_route('/things', ThingsResource()) ; Pyramid: config.add_route('home', '/')
    if (last == "add_route" && args.size() >= 2) {
      auto first = string_value(args[0]);
      if (!first) return;
      auto second = string_value(args[1]);
      if (second && !first->empty() && (*first)[0] != '/') {
        RouterFact r;  // Pyramid route name -> pattern, used by add_view / @view_config
        r.name = *first;
        r.kind = "pyramid_route";
        r.framework = "pyramid";
        r.prefix = (*second).empty() || (*second)[0] == '/' ? *second : "/" + *second;
        r.scope = scope();
        r.line = ts::start_pos(call).line;
        out_.routers.push_back(std::move(r));
        return;
      }
      if (!second && !first->empty() && (*first)[0] == '/') {
        std::string fw = imported_.count("falcon") ? "falcon" : "python_handlers";
        add_list_route(fw, "ANY", *first, args[1], receiver.empty() ? callee : receiver, call, fw == "falcon");
      }
      return;
    }
    // Pyramid: config.add_view(view, route_name='home', request_method='POST')
    if (last == "add_view") {
      TSNode route_name = keyword_arg(call, "route_name");
      auto rn = route_name.id ? string_value(route_name) : std::nullopt;
      TSNode view = !args.empty() ? args[0] : keyword_arg(call, "view");
      if (!rn || !view.id) return;
      auto methods = methods_arg(call, "request_method");
      if (methods.empty()) methods.push_back("ANY");
      for (const auto& m : methods) {
        RouteFact r;
        r.framework = "pyramid";
        r.method = m;
        r.path = "";
        r.registrar = *rn;
        r.scope = scope();
        r.pos = ts::start_pos(call);
        r.confidence = "recognized";
        r.has_handler = true;
        r.handler = handler_ref(view);
        out_.routes.push_back(std::move(r));
      }
      return;
    }
    // Flask: app.add_url_rule('/x', 'endpoint', view_func) / view_func=View.as_view('name')
    if (last == "add_url_rule" && !args.empty()) {
      auto rule = string_value(args[0]);
      TSNode view = keyword_arg(call, "view_func");
      if (!view.id && args.size() >= 3) view = args[2];
      if (!rule || !view.id) return;
      auto methods = methods_arg(call, "methods");
      bool class_view = is(view, "call");
      if (methods.empty()) methods.push_back(class_view ? "ANY" : "GET");
      auto known = routers_.find(receiver);
      for (const auto& m : methods) {
        RouteFact r;
        r.framework = known != routers_.end() ? known->second : "flask";
        r.method = m;
        r.path = *rule;
        r.registrar = receiver;
        r.scope = scope();
        r.pos = ts::start_pos(call);
        r.confidence = known != routers_.end() ? "recognized" : "heuristic";
        r.has_handler = true;
        r.handler = handler_ref(view);
        out_.routes.push_back(std::move(r));
      }
    }
  }

  // ---- assignments and type hints ---------------------------------------------

  void on_assignment(TSNode n) {
    TSNode left = field(n, "left");
    TSNode right = field(n, "right");
    std::string declared = annotation(field(n, "type"));
    std::string constructed = constructed_type(right);
    std::string type = !declared.empty() ? declared : (!constructed.empty() ? constructed : "?");
    if (type == "?") type = call_result_hint(right);
    int sc = scope();
    if (is(left, "identifier")) {
      std::string name(text(left));
      bool class_body = symbol(sc).kind == "class";
      add_hint(sc, name, type, sc == 0 ? "module" : (class_body ? "field" : "local"));
      on_router_assignment(name, right, n);
      if (is(right, "list") || is(right, "tuple")) {
        lists_[name] = right;
        if (sc == 0 && imported_.count("web")) webpy_urls(right, name);
      }
    } else if (is(left, "attribute") && is(field(left, "object"), "identifier") && text(field(left, "object")) == "self") {
      int cls = enclosing_class();
      if (type == "?" && is(right, "identifier")) type = local_type(sc, std::string(text(right)));  // self.x = param
      if (cls >= 0 && type != "?") add_hint(cls, std::string(text(field(left, "attribute"))), type, "field");
    } else if (is(left, "pattern_list") || is(left, "tuple_pattern")) {
      uint32_t c = ts_node_named_child_count(left);
      for (uint32_t i = 0; i < c; ++i) {
        TSNode id = ts_node_named_child(left, i);
        if (is(id, "identifier")) add_hint(sc, std::string(text(id)), "?", sc == 0 ? "module" : "local");
      }
    }
  }

  // "@call:<callee>" for a value produced by a call; the resolver derives its
  // type from the callee (external value, or the callee's declared return type).
  std::string call_result_hint(TSNode value) const {
    bool awaited = false;
    if (is(value, "await")) {
      value = ts_node_named_child(value, 0);
      awaited = true;
    }
    if (!is(value, "call")) return "?";
    TSNode fn = field(value, "function");
    if (!is(fn, "identifier") && !is(fn, "attribute")) return "?";
    return std::string("@call:") + (awaited ? "await " : "") + ts::clip(text(fn), 120);
  }

  // Declared type of a parameter or local of `sc` recorded so far, or "?".
  std::string local_type(int sc, const std::string& name) const {
    for (auto it = out_.type_hints.rbegin(); it != out_.type_hints.rend(); ++it) {
      if (it->scope == sc && it->name == name) return it->type;
    }
    return "?";
  }

  // web.py: urls = ('/', 'index', '/view/(\d+)', 'view')  -- path, class name, ...
  void webpy_urls(TSNode tuple, const std::string& name) {
    std::vector<TSNode> items;
    uint32_t c = ts_node_named_child_count(tuple);
    for (uint32_t i = 0; i < c; ++i) items.push_back(ts_node_named_child(tuple, i));
    if (items.size() < 2 || items.size() % 2 != 0) return;
    for (size_t i = 0; i + 1 < items.size(); i += 2) {
      auto p = string_value(items[i]);
      auto cls = string_value(items[i + 1]);
      if (!p || !cls || p->empty() || ((*p)[0] != '/' && (*p)[0] != '^')) return;
    }
    for (size_t i = 0; i + 1 < items.size(); i += 2) {
      add_list_route("webpy", "ANY", *string_value(items[i]), items[i + 1], name, items[i], true);
    }
  }

  void on_for(TSNode n) {
    TSNode left = field(n, "left");
    if (is(left, "identifier")) add_hint(scope(), std::string(text(left)), "?", "local");
  }

  // app = Flask(__name__), bp = Blueprint(..., url_prefix=...), app = FastAPI(), router = APIRouter(prefix=...)
  void on_router_assignment(const std::string& name, TSNode value, TSNode at) {
    if (!is(value, "call")) return;
    std::string callee(text(field(value, "function")));
    std::string last = callee.substr(callee.find_last_of('.') == std::string::npos ? 0 : callee.find_last_of('.') + 1);
    RouterFact r;
    r.name = name;
    r.scope = scope();
    r.line = ts::start_pos(at).line;
    if (last == "Flask") {
      r.kind = "app";
      r.framework = "flask";
    } else if (last == "Blueprint") {
      r.kind = "blueprint";
      r.framework = "flask";
      TSNode p = keyword_arg(value, "url_prefix");
      if (p.id) r.prefix = string_value(p).value_or("");
    } else if (last == "FastAPI") {
      r.kind = "app";
      r.framework = "fastapi";
    } else if (last == "APIRouter") {
      r.kind = "router";
      r.framework = "fastapi";
      TSNode p = keyword_arg(value, "prefix");
      if (p.id) r.prefix = string_value(p).value_or("");
    } else if (last == "Bottle") {
      r.kind = "app";
      r.framework = "bottle";
    } else {
      return;
    }
    routers_[name] = r.framework;
    out_.routers.push_back(std::move(r));
    if (scope() == 0) {
      // Module-level router objects get a symbol so imports of them can be followed.
      int idx = add_symbol(at, name, "variable", 0, false);
      out_.symbols[static_cast<size_t>(idx)].exported = name[0] != '_';
    }
  }

  // @app.route('/x', methods=[...]) / @router.get('/x') / @login_required
  void on_route_decorators(TSNode fn, int handler) {
    std::vector<RouteFact> routes;
    std::vector<RefFact> wrappers;
    for (TSNode d : decorators(fn)) {
      TSNode e = ts_node_named_child(d, 0);
      if (is(e, "call") && is(field(e, "function"), "identifier") && bare_route_decorator(d, e, handler, routes)) {
        continue;
      }
      if (is(e, "call") && is(field(e, "function"), "attribute")) {
        TSNode f = field(e, "function");
        std::string registrar(text(field(f, "object")));
        std::string verb(text(field(f, "attribute")));
        auto args = positional_args(e);
        std::optional<std::string> path = args.empty() ? std::nullopt : string_value(args[0]);
        std::vector<std::string> methods;
        std::string framework;
        auto known = routers_.find(registrar);
        if (verb == "route" || verb == "api_route") {
          framework = known != routers_.end() ? known->second : "flask";
          methods = methods_arg(e, "method");  // Bottle: method='POST'
          TSNode m = keyword_arg(e, "methods");
          if (is(m, "list") || is(m, "tuple")) {
            uint32_t c = ts_node_named_child_count(m);
            for (uint32_t i = 0; i < c; ++i) {
              if (auto v = string_value(ts_node_named_child(m, i))) methods.push_back(upper(*v));
            }
          }
          if (methods.empty()) methods.push_back("GET");
        } else if (verb == "get" || verb == "post" || verb == "put" || verb == "patch" || verb == "delete" ||
                   verb == "options" || verb == "head") {
          framework = known != routers_.end() ? known->second : "fastapi";
          methods.push_back(upper(verb));
        } else {
          wrappers.push_back(decorator_ref(d, e));
          continue;
        }
        if (!path) {
          out_.notes.push_back("line " + std::to_string(ts::start_pos(d).line) +
                               ": route decorator path is not a string literal");
          continue;
        }
        for (const auto& m : methods) {
          RouteFact r;
          r.framework = framework;
          r.method = m;
          r.path = *path;
          r.registrar = registrar;
          r.scope = scope_of_definition();
          r.pos = ts::start_pos(d);
          r.confidence = known != routers_.end() ? "recognized" : "heuristic";
          if (known == routers_.end()) r.note = "decorator receiver '" + registrar + "' is not a recognized app or router";
          r.has_handler = true;
          r.handler.kind = "inline";
          r.handler.name = symbol(handler).name;
          r.handler.symbol = handler;
          r.handler.text = symbol(handler).qualified_name;
          r.handler.pos = ts::start_pos(fn);
          routes.push_back(std::move(r));
        }
      } else {
        wrappers.push_back(decorator_ref(d, e));
      }
    }
    // Other decorators wrap the view function (e.g. @login_required).
    for (auto& r : routes) {
      r.middleware = wrappers;
      out_.routes.push_back(std::move(r));
    }
  }

  // Bottle: @route('/x', method='POST'), @get('/x'); Pyramid: @view_config(route_name='x', request_method='GET')
  bool bare_route_decorator(TSNode d, TSNode e, int handler, std::vector<RouteFact>& routes) {
    std::string name(text(field(e, "function")));
    auto args = positional_args(e);
    std::vector<std::string> methods;
    std::string framework, path, registrar = name;
    if (name == "view_config") {
      TSNode rn = keyword_arg(e, "route_name");
      auto route_name = rn.id ? string_value(rn) : std::nullopt;
      if (!route_name) return false;
      framework = "pyramid";
      registrar = *route_name;
      methods = methods_arg(e, "request_method");
      if (methods.empty()) methods.push_back("ANY");
    } else if (imported_.count("bottle") && (name == "route" || name == "get" || name == "post" || name == "put" ||
                                              name == "delete" || name == "patch")) {
      auto p = args.empty() ? std::nullopt : string_value(args[0]);
      if (!p) return false;
      framework = "bottle";
      path = *p;
      methods = name == "route" ? methods_arg(e, "method") : std::vector<std::string>{upper(name)};
      if (methods.empty()) methods.push_back("GET");
    } else {
      return false;
    }
    for (const auto& m : methods) {
      RouteFact r;
      r.framework = framework;
      r.method = m;
      r.path = path;
      r.registrar = registrar;
      r.scope = scope_of_definition();
      r.pos = ts::start_pos(d);
      r.confidence = "recognized";
      r.has_handler = true;
      r.handler.kind = "inline";
      r.handler.name = symbol(handler).name;
      r.handler.symbol = handler;
      r.handler.text = symbol(handler).qualified_name;
      r.handler.pos = ts::start_pos(d);
      routes.push_back(std::move(r));
    }
    return true;
  }

  int scope_of_definition() const {
    // The decorated function's own scope is on top; registration happens in its parent.
    return scopes_.size() >= 2 ? scopes_[scopes_.size() - 2].symbol : 0;
  }

  RefFact decorator_ref(TSNode d, TSNode e) const {
    RefFact ref;
    ref.pos = ts::start_pos(d);
    ref.text = ts::short_text(d, src_);
    TSNode target = is(e, "call") ? field(e, "function") : e;
    if (is(target, "identifier")) {
      ref.kind = "identifier";
      ref.name = std::string(text(target));
    } else if (is(target, "attribute")) {
      ref.kind = "member";
      ref.name = std::string(text(field(target, "attribute")));
      ref.receiver = ts::short_text(field(target, "object"), src_);
    } else {
      ref.kind = "other";
    }
    return ref;
  }

  // ---- exceptions & entry points ----------------------------------------------

  void on_try(TSNode n) {
    TSNode body = field(n, "body");
    if (!body.id) return;
    TryCtx ctx{ts::node_id(body), scope(), {}};
    uint32_t c = ts_node_named_child_count(n);
    for (uint32_t i = 0; i < c; ++i) {
      TSNode clause = ts_node_named_child(n, i);
      if (!is(clause, "except_clause") && !is(clause, "except_group_clause")) continue;
      TSNode value = field(clause, "value");
      if (is(value, "as_pattern")) value = ts_node_named_child(value, 0);
      std::vector<std::string> types;
      if (!value.id) {
        types.push_back("*");
      } else if (is(value, "tuple") || is(value, "parenthesized_expression")) {
        uint32_t k = ts_node_named_child_count(value);
        for (uint32_t j = 0; j < k; ++j) types.push_back(std::string(text(ts_node_named_child(value, j))));
      } else {
        types.push_back(std::string(text(value)));
      }
      for (const auto& t : types) {
        ExceptionFact f;
        f.kind = "catch";
        f.type = t;
        f.text = ts::short_text(value.id ? value : clause, src_);
        f.scope = scope();
        f.pos = ts::start_pos(clause);
        out_.exceptions.push_back(std::move(f));
        ctx.catches.push_back(static_cast<int>(out_.exceptions.size()) - 1);
      }
    }
    if (!ctx.catches.empty()) pending_try_[ctx.body] = ctx;
  }

  void on_except(TSNode n) {
    TSNode value = field(n, "value");
    if (is(value, "as_pattern")) {
      TSNode alias = field(value, "alias");
      TSNode id = alias.id ? ts::child_of_type(alias, "identifier") : TSNode{};
      if (id.id) except_aliases_.push_back({ts::node_id(n), std::string(text(id))});
    }
    TSNode alias = field(n, "alias");
    if (is(alias, "identifier")) except_aliases_.push_back({ts::node_id(n), std::string(text(alias))});
  }

  void on_raise(TSNode n) {
    ExceptionFact f;
    f.kind = "throw";
    f.scope = scope();
    f.pos = ts::start_pos(n);
    TSNode e = ts_node_named_child(n, 0);
    if (!e.id || ts::node_id(e) == ts::node_id(field(n, "cause"))) {
      f.kind = "rethrow";
      f.text = "raise";
    } else {
      f.text = ts::short_text(e, src_);
      if (is(e, "call")) {
        std::string callee(text(field(e, "function")));
        f.type = callee.substr(callee.find_last_of('.') == std::string::npos ? 0 : callee.find_last_of('.') + 1);
      } else if (is(e, "identifier") || is(e, "attribute")) {
        std::string name(text(e));
        if (!except_aliases_.empty() && except_aliases_.back().second == name) {
          f.kind = "rethrow";
        } else {
          f.type = name.substr(name.find_last_of('.') == std::string::npos ? 0 : name.find_last_of('.') + 1);
        }
      }
    }
    for (auto it = tries_.rbegin(); it != tries_.rend() && f.handler < 0; ++it) {
      if (it->scope != f.scope) break;
      for (int c : it->catches) {
        const auto& t = out_.exceptions[static_cast<size_t>(c)].type;
        if (t == "*" || kCatchAll.count(t)) {
          f.handler = c;
          f.handler_match = "catch_all";
          break;
        }
        if (!f.type.empty() && (t == f.type || (t.size() > f.type.size() && t.compare(t.size() - f.type.size() - 1, std::string::npos, "." + f.type) == 0))) {
          f.handler = c;
          f.handler_match = "exact";
          break;
        }
      }
    }
    out_.exceptions.push_back(std::move(f));
  }

  // ---- dispatch on the request path (http.server handlers, WSGI apps) ---------

  void path_literals(TSNode n, std::vector<std::string>& paths, std::vector<std::string>& methods, int depth) const {
    if (!n.id || depth > 8) return;
    if (auto s = string_value(n)) {
      if (route_util::names_resource(*s)) paths.push_back(*s);
      else if (auto m = route_util::http_method_name(*s)) methods.push_back(*m);
      return;
    }
    if (is(n, "lambda") || is(n, "block")) return;
    uint32_t c = ts_node_named_child_count(n);
    for (uint32_t i = 0; i < c; ++i) path_literals(ts_node_named_child(n, i), paths, methods, depth + 1);
  }

  std::optional<RefFact> branch_handler(TSNode body) const {
    std::optional<RefFact> first, preferred;
    std::vector<TSNode> stack{body};
    int visited = 0;
    while (!stack.empty() && visited < 200 && !preferred) {
      TSNode n = stack.back();
      stack.pop_back();
      ++visited;
      if ((is(n, "if_statement") || is(n, "elif_clause") || is(n, "else_clause")) && ts::node_id(n) != ts::node_id(body)) continue;
      if (is(n, "lambda") || is(n, "function_definition")) continue;
      if (is(n, "call")) {
        TSNode f = field(n, "function");
        RefFact ref;
        ref.pos = ts::start_pos(n);
        ref.text = ts::short_text(f, src_);
        std::string name;
        if (is(f, "identifier")) {
          name = std::string(text(f));
          ref.kind = std::isupper(static_cast<unsigned char>(name[0])) ? "class" : "identifier";
          ref.name = name;
        } else if (is(f, "attribute")) {
          name = std::string(text(field(f, "attribute")));
          TSNode obj = field(f, "object");
          ref.name = name;
          if (is(obj, "identifier") && (text(obj) == "self" || text(obj) == "cls")) {
            ref.kind = "method_ref";
            ref.receiver = "this";
          } else if (dotted(obj)) {
            ref.kind = "member";
            ref.receiver = std::string(text(obj));
          }
        }
        if (!ref.kind.empty() && !route_util::is_noise_call(name) && !kBuiltins.count(name)) {
          if (route_util::is_handler_like(name)) preferred = ref;
          else if (!first) first = ref;
        }
      }
      uint32_t c = ts_node_named_child_count(n);
      for (uint32_t i = c; i-- > 0;) stack.push_back(ts_node_named_child(n, i));
    }
    return preferred ? preferred : first;
  }

  void add_dispatch_routes(const std::vector<std::string>& paths, std::vector<std::string> methods, TSNode at,
                           const RefFact& handler) {
    if (methods.empty()) methods.push_back("ANY");
    std::string fn = scope() > 0 ? symbol(scope()).name : "(module)";
    for (const auto& p : paths) {
      for (const auto& m : methods) {
        RouteFact r;
        r.framework = "dispatch";
        r.method = m;
        r.path = p;
        r.registrar = fn;
        r.scope = scope();
        r.pos = ts::start_pos(at);
        r.confidence = "heuristic";
        r.note = "branch on the request path in " + fn + "()";
        r.has_handler = true;
        r.handler = handler;
        out_.routes.push_back(std::move(r));
      }
    }
  }

  // if self.path == "/users": self.list_users()  /  elif path.startswith("/orders"): ...
  void on_if_dispatch(TSNode n) {
    TSNode cond = field(n, "condition");
    if (!route_util::mentions_request(text(cond))) return;
    std::vector<std::string> paths, methods;
    path_literals(cond, paths, methods, 0);
    if (paths.empty() || paths.size() > 4) return;
    auto handler = branch_handler(field(n, "consequence"));
    if (handler) add_dispatch_routes(paths, methods, n, *handler);
  }

  // match self.path: case "/users": ...
  void on_match_dispatch(TSNode n) {
    TSNode subject = field(n, "subject");
    if (!route_util::mentions_request(text(subject))) return;
    TSNode body = field(n, "body");
    uint32_t c = body.id ? ts_node_named_child_count(body) : 0;
    for (uint32_t i = 0; i < c; ++i) {
      TSNode clause = ts_node_named_child(body, i);
      if (!is(clause, "case_clause")) continue;
      std::vector<std::string> paths, methods;
      uint32_t k = ts_node_named_child_count(clause);
      for (uint32_t j = 0; j < k; ++j) {
        TSNode part = ts_node_named_child(clause, j);
        if (is(part, "case_pattern")) path_literals(part, paths, methods, 0);
      }
      if (paths.empty()) continue;
      auto handler = branch_handler(field(clause, "consequence"));
      if (handler) add_dispatch_routes(paths, {}, clause, *handler);
    }
  }

  void on_if(TSNode n) {
    on_if_dispatch(n);
    if (scope() != 0) return;
    TSNode cond = field(n, "condition");
    if (!is(cond, "comparison_operator")) return;
    std::string c(text(cond));
    if (c.find("__name__") != std::string::npos && c.find("__main__") != std::string::npos) {
      out_.entries.push_back({"main_guard", "if __name__ == \"__main__\"", 0, ts::start_pos(n).line});
    }
  }
};

}  // namespace

void extract_python(const ExtractInput& in, FileFacts& out) {
  PyExtractor x(in, out);
  x.run();
}

}  // namespace proj
