// Python extractor.
//
// Extracts functions, classes and methods, imports (absolute, relative,
// aliased, star), calls, constructor/annotation type hints, raise/except
// sites, `if __name__ == "__main__"` guards, and Flask / FastAPI / Django
// route registrations.
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
      } else if (is(item, "aliased_import")) {
        imp.specifier = std::string(text(field(item, "name")));
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

  // Django: urlpatterns = [path('users/<int:id>/', views.user_detail), ...]
  void try_django_path(TSNode call, const std::string& name) {
    if (name != "path" && name != "re_path") return;
    TSNode list = ts_node_parent(call);
    if (!is(list, "list")) return;
    TSNode assign = ts_node_parent(list);
    if (!is(assign, "assignment") || text(field(assign, "left")) != "urlpatterns") return;
    auto args = positional_args(call);
    if (args.size() < 2) return;
    auto route = string_value(args[0]);
    if (!route) return;
    if (is(args[1], "call")) return;  // include(...) mounts are not resolved
    RouteFact r;
    r.framework = "django";
    r.method = "ANY";
    r.path = "/" + *route;
    r.registrar = "urlpatterns";
    r.scope = scope();
    r.pos = ts::start_pos(call);
    r.confidence = "recognized";
    r.has_handler = true;
    r.handler.pos = ts::start_pos(args[1]);
    r.handler.text = ts::short_text(args[1], src_);
    if (is(args[1], "identifier")) {
      r.handler.kind = "identifier";
      r.handler.name = std::string(text(args[1]));
    } else if (is(args[1], "attribute")) {
      TSNode target = args[1];
      // views.UserView.as_view -> UserView
      if (text(field(target, "attribute")) == "as_view") target = field(target, "object");
      r.handler.kind = is(target, "attribute") ? "member" : "identifier";
      if (is(target, "attribute")) {
        r.handler.name = std::string(text(field(target, "attribute")));
        r.handler.receiver = ts::short_text(field(target, "object"), src_);
      } else {
        r.handler.name = std::string(text(target));
      }
    } else {
      r.handler.kind = "other";
    }
    out_.routes.push_back(std::move(r));
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

  void on_if(TSNode n) {
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
