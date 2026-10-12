// Java extractor.
//
// Extracts package and imports, classes/interfaces/enums/records, methods and
// constructors, declared types of fields, parameters and locals (used to
// resolve method calls), throw/catch/throws, `main` entry points, Spring MVC
// and JAX-RS route mappings, and SpringApplication.run() starts.
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "parse/extractors.hpp"
#include "parse/ts_util.hpp"

namespace proj {
namespace {

using ts::field;
using ts::is;

struct Scope {
  const void* node;
  int symbol;
};

struct TryCtx {
  const void* body;
  int scope;
  std::vector<int> catches;
};

struct ClassRoutes {
  std::vector<std::string> prefixes;  // from @RequestMapping / @Path on the class
  bool controller = false;
  std::string framework;
};

const std::unordered_map<std::string_view, std::string_view> kSpringMappings = {
    {"GetMapping", "GET"},       {"PostMapping", "POST"}, {"PutMapping", "PUT"},
    {"DeleteMapping", "DELETE"}, {"PatchMapping", "PATCH"}, {"RequestMapping", "ANY"}};
const std::unordered_set<std::string_view> kJaxRsMethods = {"GET", "POST", "PUT", "DELETE", "PATCH", "HEAD", "OPTIONS"};
const std::unordered_set<std::string_view> kCatchAll = {"Exception", "Throwable", "java.lang.Exception", "java.lang.Throwable"};

std::string simple_name(std::string s) {
  auto lt = s.find('<');
  if (lt != std::string::npos) s = s.substr(0, lt);
  auto dot = s.rfind('.');
  return dot == std::string::npos ? s : s.substr(dot + 1);
}

class JavaExtractor {
 public:
  JavaExtractor(const ExtractInput& in, FileFacts& out) : in_(in), out_(out), src_(in.source) {}

  void run() {
    add_module_symbol(in_, out_);
    scopes_.push_back({ts::node_id(in_.root), 0});
    out_.depth_limited = ts::walk(in_.root, [this](TSNode n) { return enter(n); }, [this](TSNode n) { leave(n); });
  }

 private:
  const ExtractInput& in_;
  FileFacts& out_;
  std::string_view src_;
  std::vector<Scope> scopes_;
  std::unordered_map<const void*, TryCtx> pending_try_;
  std::vector<TryCtx> tries_;
  std::vector<std::pair<const void*, std::string>> catch_params_;
  std::unordered_map<int, ClassRoutes> class_routes_;
  // Inner call node -> index of the outer call whose receiver it produces.
  std::unordered_map<const void*, int> awaiting_receiver_;

  std::string_view text(TSNode n) const { return ts::text(n, src_); }
  int scope() const { return scopes_.back().symbol; }
  const SymbolFact& symbol(int i) const { return out_.symbols[static_cast<size_t>(i)]; }

  int enclosing_class() const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      const auto& k = symbol(it->symbol).kind;
      if (k == "class" || k == "interface" || k == "enum") return it->symbol;
    }
    return -1;
  }

  int add_symbol(TSNode n, std::string name, std::string kind, int parent, bool push) {
    SymbolFact s;
    std::string prefix = parent > 0 ? symbol(parent).qualified_name : out_.module_name;
    s.qualified_name = prefix.empty() ? name : prefix + "." + name;
    s.name = std::move(name);
    s.kind = std::move(kind);
    s.span = ts::span(n);
    s.parent = parent;
    out_.symbols.push_back(std::move(s));
    int idx = static_cast<int>(out_.symbols.size()) - 1;
    if (push) scopes_.push_back({ts::node_id(n), idx});
    return idx;
  }

  void add_hint(int sc, std::string name, std::string type, const char* kind) {
    if (name.empty() || type.empty()) return;
    out_.type_hints.push_back({sc, std::move(name), std::move(type), kind});
  }

  std::optional<std::string> string_value(TSNode n) const {
    if (!is(n, "string_literal")) return std::nullopt;
    auto t = text(n);
    if (t.size() >= 2 && t.front() == '"' && t.back() == '"') return std::string(t.substr(1, t.size() - 2));
    return std::nullopt;
  }

  TSNode modifiers(TSNode n) const { return ts::child_of_type(n, "modifiers"); }

  bool has_modifier(TSNode n, std::string_view word) const {
    TSNode m = modifiers(n);
    if (!m.id) return false;
    uint32_t c = ts_node_child_count(m);
    for (uint32_t i = 0; i < c; ++i) {
      if (text(ts_node_child(m, i)) == word) return true;
    }
    return false;
  }

  std::string visibility(TSNode n) const {
    for (const char* v : {"public", "protected", "private"}) {
      if (has_modifier(n, v)) return v;
    }
    return "package";
  }

  struct Annotation {
    std::string name;
    std::vector<std::string> values;  // "value"/"path" string(s), or the single positional string
    std::string method;               // RequestMapping(method = RequestMethod.X)
    TSNode node;
  };

  std::vector<Annotation> annotations(TSNode n) const {
    std::vector<Annotation> out;
    TSNode m = modifiers(n);
    if (!m.id) return out;
    uint32_t c = ts_node_named_child_count(m);
    for (uint32_t i = 0; i < c; ++i) {
      TSNode a = ts_node_named_child(m, i);
      if (!is(a, "annotation") && !is(a, "marker_annotation")) continue;
      Annotation ann;
      ann.node = a;
      ann.name = simple_name(std::string(text(field(a, "name"))));
      TSNode args = field(a, "arguments");
      uint32_t k = args.id ? ts_node_named_child_count(args) : 0;
      for (uint32_t j = 0; j < k; ++j) {
        TSNode arg = ts_node_named_child(args, j);
        if (is(arg, "element_value_pair")) {
          std::string key(text(field(arg, "key")));
          TSNode v = field(arg, "value");
          if (key == "value" || key == "path") collect_strings(v, ann.values);
          if (key == "method") ann.method = simple_name(std::string(text(v)));
        } else {
          collect_strings(arg, ann.values);
        }
      }
      out.push_back(std::move(ann));
    }
    return out;
  }

  void collect_strings(TSNode v, std::vector<std::string>& out) const {
    if (auto s = string_value(v)) {
      out.push_back(*s);
    } else if (is(v, "element_value_array_initializer")) {
      uint32_t c = ts_node_named_child_count(v);
      for (uint32_t i = 0; i < c; ++i) {
        if (auto s = string_value(ts_node_named_child(v, i))) out.push_back(*s);
      }
    }
  }

  // ---- traversal --------------------------------------------------------------

  bool enter(TSNode n) {
    if (auto it = pending_try_.find(ts::node_id(n)); it != pending_try_.end()) tries_.push_back(it->second);
    auto t = ts::type(n);
    if (t == "line_comment" || t == "block_comment" || t == "string_literal") return false;
    if (t == "package_declaration") on_package(n);
    else if (t == "import_declaration") on_import(n);
    else if (t == "class_declaration" || t == "record_declaration") on_type(n, "class");
    else if (t == "interface_declaration" || t == "annotation_type_declaration") on_type(n, "interface");
    else if (t == "enum_declaration") on_type(n, "enum");
    else if (t == "method_declaration") on_method(n, false);
    else if (t == "constructor_declaration" || t == "compact_constructor_declaration") on_method(n, true);
    else if (t == "field_declaration") on_declaration(n, "field");
    else if (t == "local_variable_declaration") on_declaration(n, "local");
    else if (t == "formal_parameter" || t == "catch_formal_parameter") on_parameter(n);
    else if (t == "enhanced_for_statement") on_foreach(n);
    else if (t == "method_invocation") on_invocation(n);
    else if (t == "object_creation_expression") on_new(n);
    else if (t == "explicit_constructor_invocation") on_explicit_ctor(n);
    else if (t == "lambda_expression") on_lambda(n);
    else if (t == "resource") on_resource(n);
    else if (t == "instanceof_expression") on_instanceof(n);
    else if (t == "throw_statement") on_throw(n);
    else if (t == "try_statement" || t == "try_with_resources_statement") on_try(n);
    else if (t == "catch_clause") on_catch(n);
    return true;
  }

  void leave(TSNode n) {
    const void* id = ts::node_id(n);
    if (scopes_.size() > 1 && scopes_.back().node == id) scopes_.pop_back();
    if (!tries_.empty() && tries_.back().body == id) tries_.pop_back();
    if (!catch_params_.empty() && catch_params_.back().first == id) catch_params_.pop_back();
  }

  void on_package(TSNode n) {
    uint32_t c = ts_node_named_child_count(n);
    for (uint32_t i = 0; i < c; ++i) {
      TSNode ch = ts_node_named_child(n, i);
      if (is(ch, "scoped_identifier") || is(ch, "identifier")) out_.module_name = std::string(text(ch));
    }
  }

  void on_import(TSNode n) {
    bool is_static = false, wildcard = false;
    std::string name;
    uint32_t c = ts_node_child_count(n);
    for (uint32_t i = 0; i < c; ++i) {
      TSNode ch = ts_node_child(n, i);
      if (is(ch, "static")) is_static = true;
      else if (is(ch, "asterisk")) wildcard = true;
      else if (is(ch, "scoped_identifier") || is(ch, "identifier")) name = std::string(text(ch));
    }
    if (name.empty()) return;
    ImportFact imp;
    imp.kind = is_static ? "static_import" : "import";
    imp.specifier = wildcard ? name + ".*" : name;
    imp.pos = ts::start_pos(n);
    if (!wildcard) imp.bindings.push_back({simple_name(name), name, "named"});
    out_.imports.push_back(std::move(imp));
  }

  void on_type(TSNode n, const char* kind) {
    std::string name(text(field(n, "name")));
    if (name.empty()) return;
    int idx = add_symbol(n, name, kind, scope(), true);
    auto& s = out_.symbols[static_cast<size_t>(idx)];
    s.visibility = visibility(n);
    s.exported = s.visibility == "public";
    s.is_static = has_modifier(n, "static");
    if (is(n, "record_declaration")) s.signature = "record";
    TSNode super = field(n, "superclass");
    if (super.id && ts_node_named_child_count(super) > 0) {
      TSNode t = ts_node_named_child(super, 0);
      out_.inherits.push_back({idx, simple_or_scoped(t), "extends", ts::start_pos(t)});
    }
    add_type_list(field(n, "interfaces"), idx, "implements");
    TSNode ext = ts::child_of_type(n, "extends_interfaces");
    if (ext.id) add_type_list(ext, idx, "extends");
    if (std::string_view(kind) == "class") on_class_annotations(n, idx);
  }

  void add_type_list(TSNode holder, int idx, const char* kind) {
    if (!holder.id) return;
    TSNode list = ts::child_of_type(holder, "type_list");
    if (!list.id) list = holder;
    uint32_t c = ts_node_named_child_count(list);
    for (uint32_t i = 0; i < c; ++i) {
      TSNode t = ts_node_named_child(list, i);
      out_.inherits.push_back({idx, simple_or_scoped(t), kind, ts::start_pos(t)});
    }
  }

  std::string simple_or_scoped(TSNode t) const {
    if (is(t, "generic_type")) t = ts_node_named_child(t, 0);
    return std::string(text(t));
  }

  void on_class_annotations(TSNode n, int cls) {
    ClassRoutes cr;
    for (const auto& a : annotations(n)) {
      if (a.name == "RestController" || a.name == "Controller") {
        cr.controller = true;
        cr.framework = "spring";
      } else if (a.name == "RequestMapping") {
        cr.prefixes = a.values;
        if (cr.framework.empty()) cr.framework = "spring";
      } else if (a.name == "Path") {
        cr.prefixes = a.values;
        cr.controller = true;
        cr.framework = "jaxrs";
      } else if (a.name == "SpringBootApplication") {
        out_.entries.push_back({"framework_app", "@SpringBootApplication class", cls, ts::start_pos(n).line});
      }
    }
    if (cr.controller || !cr.prefixes.empty()) class_routes_[cls] = cr;
  }

  int count_params(TSNode params) const {
    int count = 0;
    uint32_t c = ts_node_named_child_count(params);
    for (uint32_t i = 0; i < c; ++i) {
      TSNode p = ts_node_named_child(params, i);
      if (is(p, "spread_parameter")) return -1;
      if (is(p, "formal_parameter")) ++count;
    }
    return count;
  }

  void on_method(TSNode n, bool constructor) {
    int cls = scope();
    std::string name(text(field(n, "name")));
    if (name.empty()) return;
    bool has_body = field(n, "body").id != nullptr;
    int idx = add_symbol(n, name, constructor ? "constructor" : "method", cls, true);
    auto& s = out_.symbols[static_cast<size_t>(idx)];
    s.visibility = visibility(n);
    s.exported = s.visibility == "public";
    s.is_static = has_modifier(n, "static");
    s.is_definition = has_body || constructor;
    TSNode params = field(n, "parameters");
    s.param_count = params.id ? count_params(params) : 0;
    if (!constructor) {
      TSNode ret = field(n, "type");
      s.signature = std::string(text(ret)) + " " + name + ts::clip(text(params), 80);
      if (ret.id && !is(ret, "void_type")) add_hint(idx, "#return", std::string(text(ret)), "return");
    }
    if (!constructor && name == "main" && s.is_static && s.param_count == 1 &&
        text(params).find("String") != std::string_view::npos) {
      out_.entries.push_back({"main_function", "public static void main(String[])", idx, ts::start_pos(n).line});
    }
    TSNode throws = ts::child_of_type(n, "throws");
    uint32_t tc = throws.id ? ts_node_named_child_count(throws) : 0;
    for (uint32_t i = 0; i < tc; ++i) {
      TSNode t = ts_node_named_child(throws, i);
      ExceptionFact f;
      f.kind = "declares";
      f.type = simple_name(std::string(text(t)));
      f.text = "throws " + std::string(text(t));
      f.scope = idx;
      f.pos = ts::start_pos(t);
      out_.exceptions.push_back(std::move(f));
    }
    if (!constructor) on_method_annotations(n, idx, cls);
  }

  void on_method_annotations(TSNode n, int method, int cls) {
    auto it = class_routes_.find(cls);
    std::string jaxrs_method;
    std::vector<std::string> jaxrs_paths;
    bool jaxrs_path_seen = false;
    for (const auto& a : annotations(n)) {
      if (auto m = kSpringMappings.find(a.name); m != kSpringMappings.end()) {
        std::string verb(m->second);
        if (a.name == "RequestMapping" && !a.method.empty()) verb = a.method;
        std::vector<std::string> paths = a.values.empty() ? std::vector<std::string>{""} : a.values;
        for (const auto& p : paths) add_route(it == class_routes_.end() ? nullptr : &it->second, "spring", verb, p, a.node, method, cls);
      } else if (kJaxRsMethods.count(a.name)) {
        jaxrs_method = a.name;
      } else if (a.name == "Path") {
        jaxrs_paths = a.values;
        jaxrs_path_seen = true;
      }
    }
    if (!jaxrs_method.empty()) {
      if (!jaxrs_path_seen) jaxrs_paths = {""};
      for (const auto& p : jaxrs_paths) {
        add_route(it == class_routes_.end() ? nullptr : &it->second, "jaxrs", jaxrs_method, p, n, method, cls);
      }
    }
  }

  void add_route(const ClassRoutes* cr, const char* framework, const std::string& verb, const std::string& path,
                 TSNode at, int method, int cls) {
    std::vector<std::string> prefixes = cr && !cr->prefixes.empty() ? cr->prefixes : std::vector<std::string>{""};
    for (const auto& prefix : prefixes) {
      RouteFact r;
      r.framework = framework;
      r.method = verb;
      r.path = path.empty() || path[0] == '/' ? path : "/" + path;
      r.prefix = prefix.empty() || prefix[0] == '/' ? prefix : "/" + prefix;
      r.registrar = symbol(cls).name;
      r.scope = cls;
      r.pos = ts::start_pos(at);
      r.confidence = cr && cr->controller ? "recognized" : "heuristic";
      if (!cr || !cr->controller) r.note = "mapping on a class without a controller annotation";
      r.has_handler = true;
      r.handler.kind = "inline";
      r.handler.name = symbol(method).name;
      r.handler.symbol = method;
      r.handler.text = symbol(method).qualified_name;
      r.handler.pos = symbol(method).span.start;
      out_.routes.push_back(std::move(r));
    }
  }

  // ---- declarations and type hints ------------------------------------------

  void on_declaration(TSNode n, const char* kind) {
    std::string type(text(field(n, "type")));
    int sc = std::string_view(kind) == "field" ? enclosing_class() : scope();
    if (sc < 0) return;
    uint32_t c = ts_node_named_child_count(n);
    for (uint32_t i = 0; i < c; ++i) {
      TSNode d = ts_node_named_child(n, i);
      if (!is(d, "variable_declarator")) continue;
      std::string name(text(field(d, "name")));
      std::string t = type;
      if (t == "var") {
        TSNode v = field(d, "value");
        if (is(v, "object_creation_expression")) {
          t = std::string(text(field(v, "type")));
        } else if (is(v, "method_invocation")) {
          TSNode obj = field(v, "object");
          t = "@call:" + (obj.id ? ts::clip(text(obj), 100) + "." : std::string()) + std::string(text(field(v, "name")));
        } else {
          t = "?";
        }
      }
      add_hint(sc, name, t, kind);
    }
  }

  void on_parameter(TSNode n) {
    TSNode name = field(n, "name");
    std::string type(text(field(n, "type")));
    if (is(n, "catch_formal_parameter")) {
      TSNode ct = ts::child_of_type(n, "catch_type");
      type = ct.id ? std::string(text(ct)) : "?";
      if (name.id) catch_params_.push_back({ts::node_id(ts_node_parent(n)), std::string(text(name))});
    }
    if (name.id) add_hint(scope(), std::string(text(name)), type.empty() ? "?" : type, "param");
  }

  void on_lambda(TSNode n) {
    TSNode params = field(n, "parameters");
    if (is(params, "identifier")) {
      add_hint(scope(), std::string(text(params)), "?", "param");
    } else if (is(params, "inferred_parameters")) {
      uint32_t c = ts_node_named_child_count(params);
      for (uint32_t i = 0; i < c; ++i) add_hint(scope(), std::string(text(ts_node_named_child(params, i))), "?", "param");
    }
    // Typed lambda parameters are formal_parameter nodes, handled by on_parameter.
  }

  void on_resource(TSNode n) {
    TSNode name = field(n, "name");
    TSNode type = field(n, "type");
    if (name.id) add_hint(scope(), std::string(text(name)), type.id ? std::string(text(type)) : "?", "local");
  }

  void on_instanceof(TSNode n) {
    TSNode name = field(n, "name");  // x instanceof Foo f
    TSNode type = field(n, "right");
    if (name.id && type.id) add_hint(scope(), std::string(text(name)), std::string(text(type)), "local");
  }

  // Receiver whose static type is written in the code: `new X()` or `(X) expr`.
  std::string written_type(TSNode obj) const {
    while (is(obj, "parenthesized_expression")) obj = ts_node_named_child(obj, 0);
    if (is(obj, "object_creation_expression")) return simple_name(std::string(text(field(obj, "type"))));
    if (is(obj, "cast_expression")) return std::string(text(field(obj, "type")));
    return "";
  }

  void link_receiver(TSNode n, int index) {
    if (auto it = awaiting_receiver_.find(ts::node_id(n)); it != awaiting_receiver_.end()) {
      out_.calls[static_cast<size_t>(it->second)].receiver_call = index;
      awaiting_receiver_.erase(it);
    }
  }

  void on_foreach(TSNode n) {
    TSNode name = field(n, "name");
    TSNode type = field(n, "type");
    if (name.id) add_hint(scope(), std::string(text(name)), type.id ? std::string(text(type)) : "?", "local");
  }

  // ---- calls ------------------------------------------------------------------

  int arg_count(TSNode args) const {
    int count = 0;
    uint32_t c = args.id ? ts_node_named_child_count(args) : 0;
    for (uint32_t i = 0; i < c; ++i) {
      if (!is(ts_node_named_child(args, i), "line_comment") && !is(ts_node_named_child(args, i), "block_comment")) ++count;
    }
    return count;
  }

  void on_invocation(TSNode n) {
    CallFact c;
    c.caller = scope();
    c.kind = "calls";
    c.name = std::string(text(field(n, "name")));
    c.pos = ts::start_pos(n);
    c.arg_count = arg_count(field(n, "arguments"));
    TSNode obj = field(n, "object");
    c.text = ts::clip(obj.id ? std::string(text(obj)) + "." + c.name : c.name);
    if (!obj.id) {
      c.receiver_kind = "none";
    } else if (is(obj, "this")) {
      c.receiver_kind = "this";
      c.receiver = "this";
    } else if (is(obj, "super")) {
      c.receiver_kind = "super";
      c.receiver = "super";
    } else if (is(obj, "identifier")) {
      c.receiver_kind = "identifier";
      c.receiver = std::string(text(obj));
    } else if (is(obj, "field_access") && is(field(obj, "object"), "this")) {
      c.receiver_kind = "this_member";
      c.receiver = std::string(text(field(obj, "field")));
    } else if (is(obj, "field_access") || is(obj, "scoped_identifier")) {
      c.receiver_kind = "qualified";
      c.receiver = ts::short_text(obj, src_);
    } else if (std::string typed = written_type(obj); !typed.empty()) {
      c.receiver_kind = "typed";
      c.receiver = typed;
    } else if (is(obj, "method_invocation")) {
      c.receiver_kind = "call";
      c.receiver = ts::short_text(obj, src_);
    } else {
      c.receiver_kind = "expression";
      c.receiver = ts::short_text(obj, src_);
    }
    if (c.receiver == "SpringApplication" && c.name == "run") {
      out_.entries.push_back({"server_start", "SpringApplication.run()", scope(), c.pos.line});
    }
    bool chained = c.receiver_kind == "call";
    out_.calls.push_back(std::move(c));
    int index = static_cast<int>(out_.calls.size()) - 1;
    if (chained) awaiting_receiver_[ts::node_id(obj)] = index;
    link_receiver(n, index);
  }

  void on_new(TSNode n) {
    CallFact c;
    c.caller = scope();
    c.kind = "instantiates";
    TSNode type = field(n, "type");
    c.name = simple_name(std::string(text(type)));
    c.text = ts::clip("new " + std::string(text(type)));
    c.pos = ts::start_pos(n);
    c.arg_count = arg_count(field(n, "arguments"));
    c.receiver_kind = "none";
    if (!c.name.empty()) out_.calls.push_back(std::move(c));
  }

  void on_explicit_ctor(TSNode n) {
    TSNode ctor = field(n, "constructor");
    CallFact c;
    c.caller = scope();
    c.kind = "calls";
    c.pos = ts::start_pos(n);
    c.arg_count = arg_count(field(n, "arguments"));
    if (is(ctor, "super")) {
      c.name = "constructor";
      c.receiver = "super";
      c.receiver_kind = "super";
      c.text = "super(...)";
    } else {
      int cls = enclosing_class();
      if (cls < 0) return;
      c.name = symbol(cls).name;
      c.receiver = "this";
      c.receiver_kind = "this";
      c.text = "this(...)";
    }
    out_.calls.push_back(std::move(c));
  }

  // ---- exceptions ---------------------------------------------------------------

  void on_try(TSNode n) {
    TSNode body = field(n, "body");
    if (!body.id) return;
    TryCtx ctx{ts::node_id(body), scope(), {}};
    uint32_t c = ts_node_named_child_count(n);
    for (uint32_t i = 0; i < c; ++i) {
      TSNode clause = ts_node_named_child(n, i);
      if (!is(clause, "catch_clause")) continue;
      TSNode param = ts::child_of_type(clause, "catch_formal_parameter");
      TSNode types = param.id ? ts::child_of_type(param, "catch_type") : TSNode{};
      uint32_t k = types.id ? ts_node_named_child_count(types) : 0;
      for (uint32_t j = 0; j < k; ++j) {
        ExceptionFact f;
        f.kind = "catch";
        f.type = simple_name(std::string(text(ts_node_named_child(types, j))));
        f.text = ts::short_text(param, src_);
        f.scope = scope();
        f.pos = ts::start_pos(clause);
        out_.exceptions.push_back(std::move(f));
        ctx.catches.push_back(static_cast<int>(out_.exceptions.size()) - 1);
      }
    }
    if (!ctx.catches.empty()) pending_try_[ctx.body] = ctx;
  }

  void on_catch(TSNode) {}

  void on_throw(TSNode n) {
    TSNode e = ts_node_named_child(n, 0);
    ExceptionFact f;
    f.kind = "throw";
    f.scope = scope();
    f.pos = ts::start_pos(n);
    f.text = ts::short_text(e, src_);
    if (is(e, "object_creation_expression")) {
      f.type = simple_name(std::string(text(field(e, "type"))));
    } else if (is(e, "identifier") && !catch_params_.empty() && catch_params_.back().second == text(e)) {
      f.kind = "rethrow";
    }
    for (auto it = tries_.rbegin(); it != tries_.rend() && f.handler < 0; ++it) {
      if (it->scope != f.scope) break;
      for (int c : it->catches) {
        const auto& t = out_.exceptions[static_cast<size_t>(c)].type;
        if (!f.type.empty() && t == f.type) {
          f.handler = c;
          f.handler_match = "exact";
          break;
        }
        if (kCatchAll.count(t)) {
          f.handler = c;
          f.handler_match = "catch_all";
          break;
        }
      }
    }
    out_.exceptions.push_back(std::move(f));
  }
};

}  // namespace

void extract_java(const ExtractInput& in, FileFacts& out) {
  JavaExtractor x(in, out);
  x.run();
}

}  // namespace proj
