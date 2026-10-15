// Java extractor.
//
// Extracts package and imports, classes/interfaces/enums/records, methods and
// constructors, declared types of fields, parameters and locals (used to
// resolve method calls), throw/catch/throws, `main` entry points, Spring MVC
// and JAX-RS route mappings, SpringApplication.run() starts, and routes
// registered in code without annotations: @WebServlet, servlet containers
// (ServletContext/Jetty/Tomcat addServlet + mappings), com.sun.net.httpserver
// createContext, and Spark/Javalin-style get("/path", handler) calls.
#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "parse/extractors.hpp"
#include "parse/route_util.hpp"
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
// Methods of anonymous handler classes that serve a registered route.
const std::unordered_set<std::string> kHandlerMethods = {"handle", "handleRequest", "service", "doGet", "doPost",
                                                         "run", "call", "apply", "accept"};

std::string simple_name(std::string s) {
  auto lt = s.find('<');
  if (lt != std::string::npos) s = s.substr(0, lt);
  auto dot = s.rfind('.');
  return dot == std::string::npos ? s : s.substr(dot + 1);
}

// A declared type in the form used to compare overloads: annotations, `final`
// and type arguments removed, package dropped, array dimensions kept
// ("@NotNull final java.util.List<String>[]" -> "List[]").
std::string overload_type(std::string_view raw) {
  std::string flat;
  int depth = 0;
  for (char c : raw) {
    if (c == '<') ++depth;
    else if (c == '>') --depth;
    else if (depth == 0) flat += c;
  }
  std::string kept;
  size_t i = 0;
  while (i < flat.size()) {
    while (i < flat.size() && std::isspace(static_cast<unsigned char>(flat[i]))) ++i;
    size_t start = i;
    while (i < flat.size() && !std::isspace(static_cast<unsigned char>(flat[i]))) ++i;
    std::string tok = flat.substr(start, i - start);
    if (tok.empty() || tok[0] == '@' || tok == "final") continue;
    kept += tok;
  }
  std::string dims;
  while (kept.size() >= 2 && kept.compare(kept.size() - 2, 2, "[]") == 0) {
    dims += "[]";
    kept.resize(kept.size() - 2);
  }
  auto dot = kept.rfind('.');
  if (dot != std::string::npos) kept = kept.substr(dot + 1);
  return kept + dims;
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
  // Code-registered routes: handler lambdas and anonymous handler classes are
  // seen after the registration call; they complete the route when entered.
  std::unordered_map<const void*, int> pending_lambda_;  // lambda node -> route index
  std::unordered_map<const void*, int> pending_anon_;    // object creation with a class body -> route index
  std::unordered_set<const void*> handler_refs_;          // method references recorded as route handlers
  std::unordered_map<std::string, RefFact> servlet_names_;  // servlet name (Tomcat, ServletContext) -> class
  std::unordered_map<std::string, RefFact> servlet_vars_;   // ServletRegistration / ServletHolder variables -> class
  std::string web_framework_;  // spark|javalin, from imports

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
          if (key == "value" || key == "path" || key == "urlPatterns") collect_strings(v, ann.values);
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
    else if (t == "method_reference") on_method_ref(n);
    else if (t == "resource") on_resource(n);
    else if (t == "instanceof_expression") on_instanceof(n);
    else if (t == "throw_statement") on_throw(n);
    else if (t == "if_statement") on_if_dispatch(n);
    else if (t == "switch_expression" || t == "switch_statement") on_switch_dispatch(n);
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
    if (name.rfind("spark.", 0) == 0) web_framework_ = "spark";
    else if (name.rfind("io.javalin", 0) == 0) web_framework_ = "javalin";
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
      } else if (a.name == "WebServlet") {
        // Servlet 3 annotation: the container routes these URL patterns to the class.
        for (const auto& pattern : a.values) {
          RefFact self;
          self.kind = "class";
          self.name = symbol(cls).qualified_name;
          self.text = symbol(cls).name;
          self.pos = ts::start_pos(n);
          int r = add_code_route("servlet", "ANY", pattern, a.node, symbol(cls).name, "recognized");
          out_.routes[static_cast<size_t>(r)].scope = cls;
          set_handler(r, self);
        }
      } else if (a.name == "WebListener") {
        out_.entries.push_back({"framework_app", "@WebListener: the servlet container calls it at startup", cls,
                                ts::start_pos(n).line});
      }
    }
    if (cr.controller || !cr.prefixes.empty()) class_routes_[cls] = cr;
  }

  std::string param_types(TSNode params) const {
    std::string out;
    uint32_t c = ts_node_named_child_count(params);
    for (uint32_t i = 0; i < c; ++i) {
      TSNode p = ts_node_named_child(params, i);
      std::string t;
      if (is(p, "formal_parameter")) {
        t = overload_type(text(field(p, "type")));
        if (ts::child_of_type(p, "dimensions").id) t += "[]";  // String args[]
      } else if (is(p, "spread_parameter")) {
        uint32_t k = ts_node_named_child_count(p);
        for (uint32_t j = 0; j < k; ++j) {
          TSNode part = ts_node_named_child(p, j);
          if (!is(part, "modifiers") && !is(part, "variable_declarator") && !is(part, "annotation")) {
            t = overload_type(text(part)) + "...";
            break;
          }
        }
      } else {
        continue;
      }
      if (t.empty()) t = "?";
      out += (out.empty() ? "" : ",") + t;
    }
    return out;
  }

  // Static type of a call argument: literals, `new X()`, casts, `$name` for a
  // variable or field the resolver looks up, "?" when unknown.
  std::string arg_type(TSNode a) const {
    while (is(a, "parenthesized_expression") && ts_node_named_child_count(a) > 0) a = ts_node_named_child(a, 0);
    auto t = ts::type(a);
    std::string_view v = text(a);
    if (t == "string_literal" || t == "text_block") return "String";
    if (t == "decimal_integer_literal" || t == "hex_integer_literal" || t == "octal_integer_literal" ||
        t == "binary_integer_literal") {
      return !v.empty() && (v.back() == 'L' || v.back() == 'l') ? "long" : "int";
    }
    if (t == "decimal_floating_point_literal" || t == "hex_floating_point_literal") {
      return !v.empty() && (v.back() == 'f' || v.back() == 'F') ? "float" : "double";
    }
    if (t == "true" || t == "false" || t == "instanceof_expression") return "boolean";
    if (t == "character_literal") return "char";
    if (t == "null_literal") return "null";
    if (t == "class_literal") return "Class";
    if (t == "object_creation_expression") {
      if (ts::child_of_type(a, "class_body").id) return "?";
      return overload_type(text(field(a, "type")));
    }
    if (t == "array_creation_expression") return overload_type(text(field(a, "type"))) + "[]";
    if (t == "cast_expression") return overload_type(text(field(a, "type")));
    if (t == "identifier") return "$" + std::string(v);
    if (t == "this") return "$this";
    if (t == "field_access" && is(field(a, "object"), "this")) return "$this." + std::string(text(field(a, "field")));
    if (t == "unary_expression") {
      std::string op(text(field(a, "operator")));
      if (op == "!") return "boolean";
      std::string inner = arg_type(field(a, "operand"));
      return op == "-" || op == "+" ? inner : "?";
    }
    if (t == "binary_expression") {
      std::string op(text(field(a, "operator")));
      if (op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=" || op == "&&" || op == "||") {
        return "boolean";
      }
      std::string l = arg_type(field(a, "left")), r = arg_type(field(a, "right"));
      if (op == "+" && (l == "String" || r == "String")) return "String";
      static const std::vector<std::string> kRank = {"int", "long", "float", "double"};
      auto rank = [&](const std::string& x) {
        auto it = std::find(kRank.begin(), kRank.end(), x);
        return it == kRank.end() ? -1 : static_cast<int>(it - kRank.begin());
      };
      if (rank(l) >= 0 && rank(r) >= 0) return kRank[static_cast<size_t>(std::max(rank(l), rank(r)))];
      return "?";
    }
    return "?";
  }

  std::string arg_types(TSNode args) const {
    std::string out;
    bool known = false;
    int n = 0;
    uint32_t c = args.id ? ts_node_named_child_count(args) : 0;
    for (uint32_t i = 0; i < c; ++i) {
      TSNode a = ts_node_named_child(args, i);
      if (is(a, "line_comment") || is(a, "block_comment")) continue;
      std::string t = arg_type(a);
      if (t != "?") known = true;
      out += (n++ ? "," : "") + t;
    }
    return known ? out : "";
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
    // A method of an anonymous handler class: new HttpHandler() { public void handle(...) {...} }
    TSNode body = ts_node_parent(n);
    std::optional<int> anon_route;
    if (is(body, "class_body")) {
      auto it = pending_anon_.find(ts::node_id(ts_node_parent(body)));
      if (it != pending_anon_.end() && kHandlerMethods.count(name)) {
        anon_route = it->second;
        pending_anon_.erase(it);
      }
    }
    bool has_body = field(n, "body").id != nullptr;
    int idx = add_symbol(n, name, constructor ? "constructor" : "method", cls, true);
    auto& s = out_.symbols[static_cast<size_t>(idx)];
    s.visibility = visibility(n);
    s.exported = s.visibility == "public";
    s.is_static = has_modifier(n, "static");
    s.is_definition = has_body || constructor;
    TSNode params = field(n, "parameters");
    s.param_count = params.id ? count_params(params) : 0;
    if (params.id) s.param_types = param_types(params);
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
    if (anon_route) {
      auto& r = out_.routes[static_cast<size_t>(*anon_route)];
      r.has_handler = true;
      r.handler.kind = "inline";
      r.handler.name = name;
      r.handler.symbol = idx;
      r.handler.text = "anonymous class: " + name + "()";
      r.handler.pos = ts::start_pos(n);
    }
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
      // ServletHolder holder = new ServletHolder(new X()) -> addServlet(holder, "/path") serves X.
      if (std::string_view(kind) == "local" && is(field(d, "value"), "object_creation_expression") &&
          simple_name(std::string(text(field(field(d, "value"), "type")))) == "ServletHolder") {
        if (auto ref = class_arg(field(d, "value"))) servlet_vars_[name] = *ref;
      }
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
    if (auto it = pending_lambda_.find(ts::node_id(n)); it != pending_lambda_.end()) {
      // A route handler lambda: its body gets a symbol so calls inside it form the route's flow.
      auto& r = out_.routes[static_cast<size_t>(it->second)];
      std::string label = r.method + " " + r.path;
      int idx = add_symbol(n, label, "route_handler", scope(), true);
      auto& route = out_.routes[static_cast<size_t>(it->second)];
      route.has_handler = true;
      route.handler.kind = "inline";
      route.handler.name = label;
      route.handler.symbol = idx;
      route.handler.text = "lambda";
      route.handler.pos = ts::start_pos(n);
      pending_lambda_.erase(it);
    }
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
    c.arg_types = arg_types(field(n, "arguments"));
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
    on_route_registration(n, c);
    bool chained = c.receiver_kind == "call";
    out_.calls.push_back(std::move(c));
    int index = static_cast<int>(out_.calls.size()) - 1;
    if (chained) awaiting_receiver_[ts::node_id(obj)] = index;
    link_receiver(n, index);
  }

  // A method reference (this::m, Type::m, obj::m, Type::new) stands for the
  // lambda that calls it, and calls in a lambda body belong to the enclosing
  // method, so it is a call from there. The argument count is not visible:
  // overloads sharing the name stay ambiguous.
  void on_method_ref(TSNode n) {
    if (handler_refs_.count(ts::node_id(n))) return;  // a route handler, recorded with its route
    uint32_t count = ts_node_named_child_count(n);
    TSNode owner = count > 0 ? ts_node_named_child(n, 0) : TSNode{};
    TSNode member = count > 1 ? ts_node_named_child(n, count - 1) : TSNode{};
    if (!owner.id) return;
    CallFact c;
    c.caller = scope();
    c.pos = ts::start_pos(n);
    c.text = ts::clip(std::string(text(n)));
    if (!is(member, "identifier")) {
      // Type::new (a constructor reference); array constructors (int[]::new) are skipped.
      bool ctor = false;
      for (uint32_t i = 0; i < ts_node_child_count(n); ++i) ctor = ctor || is(ts_node_child(n, i), "new");
      if (!ctor || is(owner, "array_type")) return;
      c.kind = "instantiates";
      c.name = simple_name(std::string(text(owner)));
      c.receiver_kind = "none";
      if (!c.name.empty()) out_.calls.push_back(std::move(c));
      return;
    }
    c.kind = "calls";
    c.name = std::string(text(member));
    if (is(owner, "generic_type")) owner = ts_node_named_child(owner, 0);  // List<String>::size
    if (is(owner, "this")) {
      c.receiver_kind = "this";
      c.receiver = "this";
    } else if (is(owner, "super")) {
      c.receiver_kind = "super";
      c.receiver = "super";
    } else if (is(owner, "identifier") || is(owner, "type_identifier")) {
      c.receiver_kind = "identifier";  // a variable, or a class (static or unbound method)
      c.receiver = std::string(text(owner));
    } else if (is(owner, "field_access") && is(field(owner, "object"), "this")) {
      c.receiver_kind = "this_member";
      c.receiver = std::string(text(field(owner, "field")));
    } else if (is(owner, "field_access") || is(owner, "scoped_identifier") || is(owner, "scoped_type_identifier")) {
      c.receiver_kind = "qualified";
      c.receiver = ts::short_text(owner, src_);
    } else if (std::string typed = written_type(owner); !typed.empty()) {
      c.receiver_kind = "typed";
      c.receiver = typed;
    } else if (is(owner, "method_invocation")) {
      c.receiver_kind = "call";
      c.receiver = ts::short_text(owner, src_);
    } else {
      c.receiver_kind = "expression";
      c.receiver = ts::short_text(owner, src_);
    }
    bool chained = c.receiver_kind == "call";
    out_.calls.push_back(std::move(c));
    if (chained) awaiting_receiver_[ts::node_id(owner)] = static_cast<int>(out_.calls.size()) - 1;
  }

  // ---- routes registered in code ------------------------------------------------

  std::vector<TSNode> args_of(TSNode call) const {
    std::vector<TSNode> out;
    TSNode list = field(call, "arguments");
    uint32_t c = list.id ? ts_node_named_child_count(list) : 0;
    for (uint32_t i = 0; i < c; ++i) {
      TSNode a = ts_node_named_child(list, i);
      if (!is(a, "line_comment") && !is(a, "block_comment")) out.push_back(a);
    }
    return out;
  }

  int add_code_route(const std::string& framework, const std::string& method, const std::string& path, TSNode at,
                     const std::string& registrar, const std::string& confidence) {
    RouteFact r;
    r.framework = framework;
    r.method = method;
    r.path = path.empty() || path[0] == '/' || path[0] == '*' ? path : "/" + path;
    r.registrar = registrar;
    r.scope = scope();
    r.pos = ts::start_pos(at);
    r.confidence = confidence;
    out_.routes.push_back(std::move(r));
    return static_cast<int>(out_.routes.size()) - 1;
  }

  void set_handler(int route, const RefFact& ref) {
    auto& r = out_.routes[static_cast<size_t>(route)];
    r.has_handler = true;
    r.handler = ref;
  }

  // A class named by an argument: X.class, new X(), new ServletHolder(new X() | X.class), "com.acme.X".
  std::optional<RefFact> class_arg(TSNode a) const {
    RefFact ref;
    ref.kind = "class";
    ref.pos = ts::start_pos(a);
    ref.text = ts::short_text(a, src_);
    if (is(a, "class_literal")) {
      TSNode t = ts_node_named_child(a, 0);
      ref.name = simple_or_scoped(t);
      return ref;
    }
    if (is(a, "object_creation_expression")) {
      std::string type = simple_or_scoped(field(a, "type"));
      std::string simple = simple_name(type);
      if (simple == "ServletHolder" || simple == "FilterHolder") {
        for (TSNode inner : args_of(a)) {
          if (auto r = class_arg(inner)) return r;
        }
        return std::nullopt;
      }
      if (ts::child_of_type(a, "class_body").id) return std::nullopt;  // anonymous class
      ref.name = type;
      return ref;
    }
    if (auto s = string_value(a); s && s->find('.') != std::string::npos && s->find('/') == std::string::npos &&
                                    s->find(' ') == std::string::npos) {
      ref.name = *s;
      return ref;
    }
    if (is(a, "identifier")) {
      std::string var(text(a));
      if (auto it = servlet_vars_.find(var); it != servlet_vars_.end()) return it->second;
      // new ServletHolder(servletContainer): the variable's declared type.
      for (auto it = out_.type_hints.rbegin(); it != out_.type_hints.rend(); ++it) {
        if (it->name != var || it->type == "?" || it->type.rfind("@call:", 0) == 0) continue;
        bool visible = std::any_of(scopes_.begin(), scopes_.end(), [&](const Scope& s) { return s.symbol == it->scope; });
        if (!visible) continue;
        ref.name = it->type;
        return ref;
      }
    }
    return std::nullopt;
  }

  // The handler argument of a registration: lambda, method reference, handler object, variable.
  void set_handler_from(int route, TSNode h) {
    if (is(h, "lambda_expression")) {
      pending_lambda_[ts::node_id(h)] = route;
      return;
    }
    if (is(h, "object_creation_expression") && ts::child_of_type(h, "class_body").id) {
      pending_anon_[ts::node_id(h)] = route;
      return;
    }
    RefFact ref;
    ref.pos = ts::start_pos(h);
    ref.text = ts::short_text(h, src_);
    if (is(h, "method_reference")) {
      // Type::method, this::method, obj::method
      handler_refs_.insert(ts::node_id(h));
      uint32_t c = ts_node_named_child_count(h);
      TSNode owner = c > 0 ? ts_node_named_child(h, 0) : TSNode{};
      TSNode member = c > 1 ? ts_node_named_child(h, c - 1) : TSNode{};
      if (!owner.id || !member.id) return;
      ref.kind = "method_ref";
      ref.name = std::string(text(member));
      ref.receiver = is(owner, "this") ? "this" : (is(owner, "super") ? "super" : simple_or_scoped(owner));
      if (is(owner, "identifier") && !std::isupper(static_cast<unsigned char>(text(owner)[0]))) {
        // obj::method on a variable: the variable's declared type owns the method.
        ref.kind = "member";
        ref.receiver = std::string(text(owner));
      }
    } else if (auto cls = class_arg(h)) {
      ref = *cls;
    } else if (is(h, "identifier")) {
      ref.kind = "identifier";
      ref.name = std::string(text(h));
    } else {
      out_.routes[static_cast<size_t>(route)].note = "handler expression is not statically resolvable";
      return;
    }
    set_handler(route, ref);
  }

  void on_route_registration(TSNode call, const CallFact& c) {
    const std::string& m = c.name;
    auto args = args_of(call);
    if (args.empty()) return;
    std::string registrar = c.receiver.empty() ? "(static)" : c.receiver;
    // com.sun.net.httpserver: server.createContext("/path", handler)
    if (m == "createContext" && args.size() >= 2) {
      auto path = string_value(args[0]);
      if (!path || path->empty() || (*path)[0] != '/') return;
      int r = add_code_route("httpserver", "ANY", *path, call, registrar, "recognized");
      set_handler_from(r, args[1]);
      return;
    }
    // Jetty: context.addServlet(holder | X.class | "com.x.X", "/path"); handler.addServletWithMapping(X.class, "/path")
    // Servlet API / Tomcat: ctx.addServlet("name", servlet) -> addMapping / addServletMapping(Decoded)
    if (m == "addServlet" || m == "addServletWithMapping") {
      std::optional<std::string> path, name;
      std::optional<RefFact> servlet;
      for (TSNode a : args) {
        auto s = string_value(a);
        if (s && !s->empty() && ((*s)[0] == '/' || (*s)[0] == '*')) {
          path = s;
        } else if (auto ref = class_arg(a); ref && !servlet) {
          servlet = ref;
        } else if (s && !name) {
          name = s;
        } else if (is(a, "identifier") && !servlet && ts::node_id(a) == ts::node_id(args.back())) {
          RefFact var;
          var.kind = "identifier";
          var.name = std::string(text(a));
          var.text = var.name;
          var.pos = ts::start_pos(a);
          servlet = var;
        }
      }
      if (servlet && name) servlet_names_[*name] = *servlet;
      if (servlet) {
        TSNode decl = ts_node_parent(call);
        if (is(decl, "variable_declarator")) servlet_vars_[std::string(text(field(decl, "name")))] = *servlet;
      }
      if (path) {
        int r = add_code_route("servlet", "ANY", *path, call, registrar, "recognized");
        if (servlet) set_handler(r, *servlet);
        else out_.routes[static_cast<size_t>(r)].note = "servlet argument is not statically resolvable";
      }
      return;
    }
    // registration.addMapping("/a", "/b"), with the registration from addServlet(...)
    if (m == "addMapping") {
      std::optional<RefFact> servlet;
      TSNode obj = field(call, "object");
      if (is(obj, "method_invocation") && text(field(obj, "name")) == "addServlet") {
        for (TSNode a : args_of(obj)) {
          if (auto ref = class_arg(a)) servlet = ref;
        }
      } else if (is(obj, "identifier")) {
        if (auto it = servlet_vars_.find(std::string(text(obj))); it != servlet_vars_.end()) servlet = it->second;
      }
      for (TSNode a : args) {
        auto s = string_value(a);
        if (!s || s->empty()) continue;
        int r = add_code_route("servlet", "ANY", *s, call, registrar, "recognized");
        if (servlet) set_handler(r, *servlet);
        else out_.routes[static_cast<size_t>(r)].note = "servlet registration is not statically resolvable";
      }
      return;
    }
    // Tomcat: context.addServletMappingDecoded("/path", "name")
    if ((m == "addServletMappingDecoded" || m == "addServletMapping") && args.size() >= 2) {
      auto path = string_value(args[0]);
      auto name = string_value(args[1]);
      if (!path || !name) return;
      int r = add_code_route("servlet", "ANY", *path, call, registrar, "recognized");
      if (auto it = servlet_names_.find(*name); it != servlet_names_.end()) set_handler(r, it->second);
      else out_.routes[static_cast<size_t>(r)].note = "servlet '" + *name + "' is not registered in this file";
      return;
    }
    // Handler maps of front controllers: handlers.put("/login", new LoginCommand())
    if ((m == "put" || m == "register" || m == "map" || m == "bind") && args.size() >= 2) {
      std::string lr;
      for (char ch : c.receiver) lr += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      bool table = false;
      for (const char* w : {"route", "handler", "dispatch", "command", "action", "controller", "mapping", "endpoint"}) {
        if (lr.find(w) != std::string::npos) table = true;
      }
      auto path = string_value(args[0]);
      if (table && path && route_util::is_route_path(*path)) {
        int r = add_code_route("dispatch_table", "ANY", *path, call, registrar, "heuristic");
        out_.routes[static_cast<size_t>(r)].note = "entry of handler map '" + c.receiver + "'";
        set_handler_from(r, args[1]);
      }
      return;
    }
    // Spark / Javalin style: get("/path", handler), app.post("/path", ctx -> ...)
    static const std::unordered_map<std::string_view, std::string_view> kVerbs = {
        {"get", "GET"}, {"post", "POST"}, {"put", "PUT"}, {"delete", "DELETE"},
        {"patch", "PATCH"}, {"head", "HEAD"}, {"options", "OPTIONS"}};
    auto verb = kVerbs.find(m);
    if (verb == kVerbs.end() || args.size() < 2) return;
    auto path = string_value(args[0]);
    if (!path || path->empty() || (*path)[0] != '/') return;
    static const std::unordered_set<std::string_view> kRegistrars = {"app", "router", "server", "http", "routes",
                                                                     "api", "javalin", "spark", "route", "web"};
    bool known = !web_framework_.empty();
    if (!known && !kRegistrars.count(c.receiver)) return;
    int r = add_code_route(known ? web_framework_ : "java_http", std::string(verb->second), *path, call, registrar,
                           known ? "recognized" : "heuristic");
    if (!known) out_.routes[static_cast<size_t>(r)].note = "receiver '" + c.receiver + "' looks like an HTTP router";
    set_handler_from(r, args.back());
  }

  // ---- dispatch on the request path (front controller servlets, handlers) -------

  void path_literals(TSNode n, std::vector<std::string>& paths, std::vector<std::string>& methods, int depth) const {
    if (!n.id || depth > 8) return;
    if (auto s = string_value(n)) {
      if (route_util::names_resource(*s)) paths.push_back(*s);
      else if (auto m = route_util::http_method_name(*s)) methods.push_back(*m);
      return;
    }
    if (is(n, "lambda_expression") || is(n, "block")) return;
    uint32_t c = ts_node_named_child_count(n);
    for (uint32_t i = 0; i < c; ++i) path_literals(ts_node_named_child(n, i), paths, methods, depth + 1);
  }

  // The handler of a dispatch branch: a call that handles the request, else the
  // first call that is not output or a request accessor, or a handler object.
  std::optional<RefFact> branch_handler(TSNode body) const {
    std::optional<RefFact> first, preferred;
    std::vector<TSNode> stack{body};
    int visited = 0;
    while (!stack.empty() && visited < 200 && !preferred) {
      TSNode n = stack.back();
      stack.pop_back();
      ++visited;
      if ((is(n, "if_statement") || is(n, "switch_expression")) && ts::node_id(n) != ts::node_id(body)) continue;
      if (is(n, "lambda_expression") || is(n, "class_body")) continue;
      if (is(n, "method_invocation")) {
        std::string name(text(field(n, "name")));
        TSNode obj = field(n, "object");
        if (!name.empty() && !route_util::is_noise_call(name)) {
          RefFact ref;
          ref.pos = ts::start_pos(n);
          ref.text = ts::short_text(n, src_);
          ref.name = name;
          if (!obj.id || is(obj, "this")) {
            ref.kind = "method_ref";
            ref.receiver = "this";
          } else if (is(obj, "identifier") && std::isupper(static_cast<unsigned char>(text(obj)[0]))) {
            ref.kind = "method_ref";
            ref.receiver = std::string(text(obj));
          } else if (is(obj, "identifier")) {
            ref.kind = "member";
            ref.receiver = std::string(text(obj));
          } else if (is(obj, "field_access") && is(field(obj, "object"), "this")) {
            ref.kind = "member";
            ref.receiver = std::string(text(field(obj, "field")));
          } else if (std::string typed = written_type(obj); !typed.empty()) {
            ref.kind = "method_ref";
            ref.receiver = typed;
          }
          if (!ref.kind.empty()) {
            if (route_util::is_handler_like(name)) preferred = ref;
            else if (!first) first = ref;
          }
        }
      } else if (is(n, "object_creation_expression") && !ts::child_of_type(n, "class_body").id) {
        RefFact ref;
        ref.kind = "class";
        ref.name = simple_or_scoped(field(n, "type"));
        ref.pos = ts::start_pos(n);
        ref.text = ts::short_text(n, src_);
        if (!first) first = ref;
      }
      uint32_t c = ts_node_named_child_count(n);
      for (uint32_t i = c; i-- > 0;) stack.push_back(ts_node_named_child(n, i));
    }
    return preferred ? preferred : first;
  }

  std::string enclosing_method_name() const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      const auto& k = symbol(it->symbol).kind;
      if (k == "method" || k == "constructor" || k == "route_handler") return symbol(it->symbol).name;
    }
    return "(class body)";
  }

  void add_dispatch_routes(const std::vector<std::string>& paths, std::vector<std::string> methods, TSNode at,
                           const RefFact& handler) {
    if (methods.empty()) methods.push_back("ANY");
    std::string fn = enclosing_method_name();
    for (const auto& p : paths) {
      for (const auto& m : methods) {
        int r = add_code_route("dispatch", m, p, at, fn, "heuristic");
        out_.routes[static_cast<size_t>(r)].note = "branch on the request path in " + fn + "()";
        set_handler(r, handler);
      }
    }
  }

  // if ("/users".equals(path)) listUsers(req, resp); else if (uri.startsWith("/orders")) ...
  void on_if_dispatch(TSNode n) {
    TSNode cond = field(n, "condition");
    if (!route_util::mentions_request(text(cond))) return;
    std::vector<std::string> paths, methods;
    path_literals(cond, paths, methods, 0);
    if (paths.empty() || paths.size() > 4) return;
    auto handler = branch_handler(field(n, "consequence"));
    if (handler) add_dispatch_routes(paths, methods, n, *handler);
  }

  // switch (path) { case "/users": listUsers(req); break; case "/orders" -> orders(req); }
  void on_switch_dispatch(TSNode n) {
    TSNode subject = field(n, "condition");
    if (!route_util::mentions_request(text(subject))) return;
    TSNode body = field(n, "body");
    uint32_t c = body.id ? ts_node_named_child_count(body) : 0;
    for (uint32_t i = 0; i < c; ++i) {
      TSNode group = ts_node_named_child(body, i);
      if (!is(group, "switch_block_statement_group") && !is(group, "switch_rule")) continue;
      std::vector<std::string> paths, methods;
      uint32_t k = ts_node_named_child_count(group);
      for (uint32_t j = 0; j < k; ++j) {
        TSNode part = ts_node_named_child(group, j);
        if (is(part, "switch_label")) path_literals(part, paths, methods, 0);
      }
      if (paths.empty()) continue;
      auto handler = branch_handler(group);
      if (handler) add_dispatch_routes(paths, {}, group, *handler);
    }
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
    c.arg_types = arg_types(field(n, "arguments"));
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
    c.arg_types = arg_types(field(n, "arguments"));
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
