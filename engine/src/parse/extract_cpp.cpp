// C and C++ extractor (the C grammar produces a subset of the same nodes).
//
// Extracts #include directives, namespaces, classes/structs/unions/enums,
// function and method definitions and declarations (including out-of-line
// `A::f()` definitions), declared types of parameters, locals and fields,
// calls, `new` expressions, throw/try/catch, `main` entry points, and HTTP
// routes registered without annotations: registration calls
// (svr.Get("/x", h), Routes::Get(r, "/x", Routes::bind(&Api::f, this)),
// CROW_ROUTE(app, "/x")(...), registerHandler, mg_set_request_handler, ...),
// route tables ({"/x", handler} entries, routes["/x"] = h) and if-dispatch on
// the request path (if (uri == "/x"), strcmp(path, "/x") == 0).
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

const std::unordered_set<std::string_view> kCatchAll = {"*", "std::exception", "exception"};

class CppExtractor {
 public:
  CppExtractor(const ExtractInput& in, FileFacts& out)
      : in_(in), out_(out), src_(in.source) {}

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
  std::unordered_map<int, std::string> access_;  // class symbol -> current access label
  std::unordered_map<const void*, int> awaiting_receiver_;  // inner call node -> outer call index
  std::unordered_map<const void*, int> pending_lambda_;     // route handler lambda -> route index
  std::string http_library_;  // from #include: cpp-httplib, crow, drogon, pistache, mongoose, ...

  std::string_view text(TSNode n) const { return ts::text(n, src_); }
  int scope() const { return scopes_.back().symbol; }
  const SymbolFact& symbol(int i) const { return out_.symbols[static_cast<size_t>(i)]; }

  static bool class_kind(const std::string& k) { return k == "class" || k == "struct" || k == "union"; }

  // Qualified name of the innermost namespace/class scope ("" at file level).
  std::string scope_qname(int sc) const { return sc > 0 ? symbol(sc).qualified_name : ""; }

  int add_symbol(TSNode n, std::string name, std::string kind, int parent, bool push, const std::string& container = "") {
    SymbolFact s;
    std::string prefix = !container.empty() ? container : scope_qname(parent);
    s.qualified_name = prefix.empty() ? name : prefix + "::" + name;
    s.name = std::move(name);
    s.kind = std::move(kind);
    s.span = ts::span(n);
    s.parent = parent;
    s.container = container;
    out_.symbols.push_back(std::move(s));
    int idx = static_cast<int>(out_.symbols.size()) - 1;
    if (push) scopes_.push_back({ts::node_id(n), idx});
    return idx;
  }

  void add_hint(int sc, std::string name, std::string type, const char* kind) {
    if (name.empty() || type.empty()) return;
    out_.type_hints.push_back({sc, std::move(name), std::move(type), kind});
  }

  int enclosing_function_or_scope() const { return scope(); }

  // ---- declarator helpers ----------------------------------------------------

  // Unwraps pointer/reference/array/parenthesized declarators.
  TSNode inner_declarator(TSNode d) const {
    for (int guard = 0; guard < 16 && d.id; ++guard) {
      if (is(d, "pointer_declarator") || is(d, "reference_declarator") || is(d, "array_declarator") ||
          is(d, "parenthesized_declarator") || is(d, "attributed_declarator") || is(d, "init_declarator")) {
        TSNode next = field(d, "declarator");
        if (!next.id) next = ts_node_named_child(d, 0);
        d = next;
        continue;
      }
      break;
    }
    return d;
  }

  TSNode function_declarator(TSNode d) const {
    for (int guard = 0; guard < 16 && d.id; ++guard) {
      if (is(d, "function_declarator")) return d;
      TSNode next = field(d, "declarator");
      if (!next.id) next = ts_node_named_child(d, 0);
      if (!next.id || ts::node_id(next) == ts::node_id(d)) break;
      d = next;
    }
    return TSNode{};
  }

  int param_count(TSNode fdecl) const {
    TSNode params = field(fdecl, "parameters");
    if (!params.id) return -1;
    int count = 0;
    uint32_t c = ts_node_named_child_count(params);
    for (uint32_t i = 0; i < c; ++i) {
      TSNode p = ts_node_named_child(params, i);
      if (is(p, "variadic_parameter_declaration") || is(p, "variadic_parameter")) return -1;
      if (is(p, "parameter_declaration") || is(p, "optional_parameter_declaration")) {
        if (c == 1 && text(p) == "void") return 0;
        ++count;
      }
    }
    // "..." is an anonymous token in C.
    if (text(params).find("...") != std::string_view::npos) return -1;
    return count;
  }

  // Declared parameter types as written, without names and default values
  // ("const std::string&,int*"); the resolver normalizes them.
  std::string param_types(TSNode fdecl) const {
    TSNode params = field(fdecl, "parameters");
    if (!params.id) return "";
    std::string out;
    int n = 0;
    uint32_t c = ts_node_named_child_count(params);
    for (uint32_t i = 0; i < c; ++i) {
      TSNode p = ts_node_named_child(params, i);
      std::string t;
      if (is(p, "variadic_parameter_declaration") || is(p, "variadic_parameter")) {
        t = "...";
      } else if (is(p, "parameter_declaration") || is(p, "optional_parameter_declaration")) {
        if (c == 1 && text(p) == "void") return "";
        TSNode d = field(p, "declarator");
        TSNode name = inner_declarator(d);
        uint32_t end = (is(name, "identifier") || is(name, "field_identifier")) ? ts_node_start_byte(name) : ts_node_end_byte(p);
        if (is(p, "optional_parameter_declaration")) {
          TSNode def = field(p, "default_value");
          if (def.id && (!is(name, "identifier") || ts_node_start_byte(def) < end)) end = ts_node_start_byte(def);
        }
        uint32_t start = ts_node_start_byte(p);
        t = end > start ? std::string(src_.substr(start, end - start)) : "?";
        // trim and drop a trailing '='
        while (!t.empty() && (std::isspace(static_cast<unsigned char>(t.back())) || t.back() == '=')) t.pop_back();
        if (t.find('(') != std::string::npos) t = "?";  // function pointer
      } else {
        continue;
      }
      out += (n++ ? "," : "") + (t.empty() ? std::string("?") : t);
    }
    if (text(params).find("...") != std::string_view::npos && out.find("...") == std::string::npos) {
      out += (n ? "," : "") + std::string("...");
    }
    return out;
  }

  // Static type of a call argument (see CallFact::arg_types).
  std::string arg_type(TSNode a) const {
    while (is(a, "parenthesized_expression") && ts_node_named_child_count(a) > 0) a = ts_node_named_child(a, 0);
    auto t = ts::type(a);
    std::string_view v = text(a);
    if (t == "string_literal" || t == "concatenated_string" || t == "raw_string_literal") return "char*";
    if (t == "char_literal") return "char";
    if (t == "true" || t == "false") return "bool";
    if (t == "nullptr") return "nullptr";
    if (t == "number_literal") {
      std::string s(v);
      for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      bool hex = s.rfind("0x", 0) == 0;
      if (!hex && (s.find('.') != std::string::npos || s.find('e') != std::string::npos)) {
        return s.back() == 'f' ? "float" : "double";
      }
      bool u = s.find('u') != std::string::npos;
      size_t longs = static_cast<size_t>(std::count(s.begin(), s.end(), 'l'));
      if (longs >= 2) return u ? "unsigned long long" : "long long";
      if (longs == 1) return u ? "unsigned long" : "long";
      return u ? "unsigned" : "int";
    }
    if (t == "new_expression") return std::string(text(field(a, "type"))) + "*";
    if (t == "cast_expression") return std::string(text(field(a, "type")));
    if (t == "identifier") return "$" + std::string(v);
    if (t == "this") return "$this";
    if (t == "field_expression" && is(field(a, "argument"), "this")) return "$this." + std::string(text(field(a, "field")));
    return "?";
  }

  std::string arg_types(TSNode call) const {
    TSNode args = field(call, "arguments");
    if (!args.id) return "";
    std::string out;
    bool known = false;
    int n = 0;
    uint32_t c = ts_node_named_child_count(args);
    for (uint32_t i = 0; i < c; ++i) {
      TSNode a = ts_node_named_child(args, i);
      if (is(a, "comment")) continue;
      std::string t = arg_type(a);
      if (t != "?") known = true;
      out += (n++ ? "," : "") + t;
    }
    return known ? out : "";
  }

  std::string declarator_name(TSNode d) const {
    d = inner_declarator(d);
    if (is(d, "identifier") || is(d, "field_identifier")) return std::string(text(d));
    return "";
  }

  // ---- traversal --------------------------------------------------------------

  bool enter(TSNode n) {
    if (auto it = pending_try_.find(ts::node_id(n)); it != pending_try_.end()) tries_.push_back(it->second);
    auto t = ts::type(n);
    if (t == "comment" || t == "string_literal" || t == "raw_string_literal" || t == "char_literal") return false;
    if (t == "preproc_include") {
      on_include(n);
      return false;
    }
    if (t == "namespace_definition") on_namespace(n);
    else if (t == "class_specifier" || t == "struct_specifier" || t == "union_specifier") on_class(n);
    else if (t == "enum_specifier") on_enum(n);
    else if (t == "access_specifier") on_access(n);
    else if (t == "function_definition") on_function_definition(n);
    else if (t == "declaration" || t == "field_declaration") on_declaration(n);
    else if (t == "parameter_declaration" || t == "optional_parameter_declaration") on_parameter(n);
    else if (t == "call_expression") on_call(n);
    else if (t == "new_expression") on_new(n);
    else if (t == "throw_statement" || t == "throw_expression") on_throw(n);
    else if (t == "try_statement") on_try(n);
    else if (t == "lambda_expression") on_lambda(n);
    else if (t == "initializer_list") on_route_table(n);
    else if (t == "if_statement") on_if_dispatch(n);
    else if (t == "assignment_expression") on_route_assignment(n);
    return true;
  }

  void leave(TSNode n) {
    const void* id = ts::node_id(n);
    if (scopes_.size() > 1 && scopes_.back().node == id) scopes_.pop_back();
    if (!tries_.empty() && tries_.back().body == id) tries_.pop_back();
  }

  void on_include(TSNode n) {
    TSNode path = field(n, "path");
    std::string raw(text(path));
    if (raw.size() < 2) return;
    ImportFact imp;
    imp.kind = "include";
    imp.system = is(path, "system_lib_string");
    imp.specifier = raw.substr(1, raw.size() - 2);
    imp.pos = ts::start_pos(n);
    static const std::pair<const char*, const char*> kLibraries[] = {
        {"httplib", "cpp-httplib"}, {"crow", "crow"},         {"drogon", "drogon"},     {"pistache", "pistache"},
        {"mongoose", "mongoose"},   {"civetweb", "civetweb"}, {"microhttpd", "libmicrohttpd"},
        {"cpprest", "cpprestsdk"},  {"oatpp", "oatpp"},       {"Poco/Net", "poco"},     {"boost/beast", "beast"}};
    for (const auto& [needle, name] : kLibraries) {
      if (imp.specifier.find(needle) != std::string::npos) http_library_ = name;
    }
    out_.imports.push_back(std::move(imp));
  }

  void on_namespace(TSNode n) {
    TSNode name = field(n, "name");
    std::string full = name.id ? std::string(text(name)) : "(anonymous)";
    // namespace a::b { } introduces nested namespaces a and a::b.
    int parent = scope();
    size_t start = 0;
    int idx = parent;
    while (true) {
      size_t sep = full.find("::", start);
      std::string part = full.substr(start, sep == std::string::npos ? std::string::npos : sep - start);
      bool last = sep == std::string::npos;
      idx = add_symbol(n, part, "namespace", idx, last);
      if (last) break;
      start = sep + 2;
    }
  }

  void on_class(TSNode n) {
    TSNode body = field(n, "body");
    TSNode name = field(n, "name");
    if (!body.id) return;  // forward declaration or elaborated type use
    std::string kind = is(n, "class_specifier") ? "class" : (is(n, "struct_specifier") ? "struct" : "union");
    std::string nm = name.id ? std::string(text(name)) : "(anonymous " + kind + ")";
    if (is(name, "template_type")) nm = std::string(text(field(name, "name")));
    std::string container;
    if (is(name, "qualified_identifier")) {
      auto pos = nm.rfind("::");
      container = join(scope_qname(scope()), nm.substr(0, pos));
      nm = nm.substr(pos + 2);
    }
    int idx = add_symbol(n, nm, kind, scope(), true, container);
    access_[idx] = kind == "class" ? "private" : "public";
    TSNode bases = ts::child_of_type(n, "base_class_clause");
    uint32_t c = bases.id ? ts_node_named_child_count(bases) : 0;
    for (uint32_t i = 0; i < c; ++i) {
      TSNode b = ts_node_named_child(bases, i);
      if (is(b, "type_identifier") || is(b, "qualified_identifier")) {
        out_.inherits.push_back({idx, std::string(text(b)), "extends", ts::start_pos(b)});
      } else if (is(b, "template_type")) {
        out_.inherits.push_back({idx, std::string(text(field(b, "name"))), "extends", ts::start_pos(b)});
      }
    }
    // typedef struct { ... } Name;
    TSNode parent = ts_node_parent(n);
    if (!name.id && is(parent, "type_definition")) {
      std::string alias = declarator_name(field(parent, "declarator"));
      if (!alias.empty()) {
        auto& s = out_.symbols[static_cast<size_t>(idx)];
        s.name = alias;
        s.qualified_name = join(scope_qname(s.parent), alias);
      }
    }
  }

  static std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    return a + "::" + b;
  }

  void on_enum(TSNode n) {
    TSNode name = field(n, "name");
    if (!name.id || !field(n, "body").id) return;
    add_symbol(n, std::string(text(name)), "enum", scope(), false);
  }

  void on_access(TSNode n) {
    int cls = scope();
    if (!class_kind(symbol(cls).kind)) return;
    std::string a(text(n));
    while (!a.empty() && (a.back() == ':' || a.back() == ' ')) a.pop_back();
    access_[cls] = a;
  }

  // Symbol for a function definition or declaration. Returns its index.
  int add_function(TSNode n, TSNode fdecl, bool definition) {
    TSNode d = field(fdecl, "declarator");
    std::string raw(text(d));
    std::string name = raw;
    std::string container;
    int parent = scope();
    bool in_class = class_kind(symbol(parent).kind);
    if (is(d, "qualified_identifier")) {
      // Out-of-line definition: void ns::A::f() {...}
      auto pos = raw.rfind("::");
      std::string qualifier = raw.substr(0, pos);
      name = raw.substr(pos + 2);
      container = join(scope_qname(parent), qualifier);
    } else if (is(d, "template_function")) {
      name = std::string(text(field(d, "name")));
    } else if (is(d, "destructor_name") || is(d, "operator_name") || is(d, "field_identifier") || is(d, "identifier")) {
      name = raw;
    } else {
      return -1;  // function pointers and other unusual declarators
    }
    std::string kind = "function";
    std::string owner_simple;
    if (in_class) owner_simple = symbol(parent).name;
    else if (!container.empty()) owner_simple = container.substr(container.rfind("::") == std::string::npos ? 0 : container.rfind("::") + 2);
    if (in_class) {
      kind = name == owner_simple ? "constructor" : "method";
    } else if (!container.empty()) {
      // `void A::f()` defines a member when A is a class; `void ns::f()` defines a
      // namespace function. The class may live in another file, so a qualifier
      // naming a class in this file, a constructor/destructor, or a capitalized
      // qualifier is treated as a class (documented heuristic).
      bool owner_is_class = name == owner_simple || name[0] == '~' ||
                            (!owner_simple.empty() && std::isupper(static_cast<unsigned char>(owner_simple[0])));
      for (const auto& s : out_.symbols) {
        if (class_kind(s.kind) && s.qualified_name == container) owner_is_class = true;
      }
      if (owner_is_class) kind = name == owner_simple ? "constructor" : "method";
    }
    int idx = add_symbol(n, name, kind, parent, definition, container);
    auto& s = out_.symbols[static_cast<size_t>(idx)];
    s.is_definition = definition;
    s.param_count = param_count(fdecl);
    s.param_types = param_types(fdecl);
    s.signature = ts::clip(text(fdecl), 120);
    if (in_class) s.visibility = access_[parent];
    TSNode ret = field(n, "type");
    if (ret.id && text(ret) != "void" && kind != "constructor") {
      out_.type_hints.push_back({idx, "#return", std::string(text(ret)), "return"});
    }
    std::string head(text(n).substr(0, std::min<size_t>(text(n).size(), 64)));
    if (head.rfind("static ", 0) == 0) s.is_static = true;
    if (definition && kind == "function" && name == "main" && parent == 0 && container.empty()) {
      out_.entries.push_back({"main_function", "main()", idx, ts::start_pos(n).line});
    }
    s.exported = !s.is_static && s.visibility != "private";
    return idx;
  }

  void on_function_definition(TSNode n) {
    TSNode fdecl = function_declarator(field(n, "declarator"));
    if (!fdecl.id) return;
    add_function(n, fdecl, true);
  }

  void on_declaration(TSNode n) {
    int sc = scope();
    const std::string kind = symbol(sc).kind;  // copy: add_function may reallocate
    bool function_scope = kind == "function" || kind == "method" || kind == "constructor";
    std::string type(text(field(n, "type")));
    uint32_t c = ts_node_named_child_count(n);
    for (uint32_t i = 0; i < c; ++i) {
      TSNode d = ts_node_named_child(n, i);
      if (ts::node_id(d) == ts::node_id(field(n, "type"))) continue;
      TSNode fdecl = function_declarator(d);
      if (fdecl.id && !function_scope && !is(d, "init_declarator")) {
        add_function(n, fdecl, false);  // prototype or in-class method declaration
        continue;
      }
      std::string name = declarator_name(d);
      if (name.empty()) continue;
      std::string t = type;
      if (t == "auto" && is(d, "init_declarator")) t = initializer_type(field(d, "value"));
      if (is(inner_pointer(d), "pointer_declarator")) t += "*";
      const char* hint_kind = class_kind(kind) ? "field" : (function_scope ? "local" : "module");
      add_hint(sc, name, t.empty() ? "?" : t, hint_kind);
    }
  }

  TSNode inner_pointer(TSNode d) const {
    if (is(d, "init_declarator")) d = field(d, "declarator");
    return d;
  }

  // auto x = Foo(...) / new Foo / std::make_unique<Foo>(...)
  std::string initializer_type(TSNode v) const {
    if (is(v, "new_expression")) return std::string(text(field(v, "type")));
    if (is(v, "call_expression")) {
      TSNode fn = field(v, "function");
      if (is(fn, "template_function") || (is(fn, "qualified_identifier") && is(field(fn, "name"), "template_function"))) {
        TSNode tf = is(fn, "template_function") ? fn : field(fn, "name");
        std::string fname(text(field(tf, "name")));
        if (fname == "make_unique" || fname == "make_shared") {
          TSNode args = field(tf, "arguments");
          if (args.id && ts_node_named_child_count(args) > 0) return std::string(text(ts_node_named_child(args, 0)));
        }
      }
      if (is(fn, "identifier") || is(fn, "qualified_identifier")) {
        std::string callee(text(fn));
        std::string last = callee.substr(callee.rfind("::") == std::string::npos ? 0 : callee.rfind("::") + 2);
        if (!last.empty() && std::isupper(static_cast<unsigned char>(last[0]))) return callee;
      }
      if (is(fn, "identifier") || is(fn, "qualified_identifier") || is(fn, "field_expression")) {
        return "@call:" + ts::clip(text(fn), 120);
      }
    }
    return "?";
  }

  void on_parameter(TSNode n) {
    TSNode parent = ts_node_parent(n);
    if (!is(parent, "parameter_list")) return;
    TSNode owner = ts_node_parent(parent);
    // Parameters of the function whose body we are inside, and catch parameters.
    if (!is(owner, "function_declarator") && !is(owner, "catch_clause")) return;
    int sc = scope();
    const std::string kind = symbol(sc).kind;
    if (kind != "function" && kind != "method" && kind != "constructor") return;
    std::string name = declarator_name(field(n, "declarator"));
    if (name.empty()) return;
    std::string type(text(field(n, "type")));
    if (!type.empty() && is(field(n, "declarator"), "pointer_declarator")) type += "*";
    add_hint(sc, name, type.empty() ? "?" : type, "param");
  }

  // ---- calls ------------------------------------------------------------------

  int arg_count(TSNode call) const {
    TSNode args = field(call, "arguments");
    if (!args.id) return -1;
    int count = 0;
    uint32_t c = ts_node_named_child_count(args);
    for (uint32_t i = 0; i < c; ++i) {
      if (!is(ts_node_named_child(args, i), "comment")) ++count;
    }
    return count;
  }

  void on_call(TSNode n) {
    TSNode fn = field(n, "function");
    on_route_call(n);
    CallFact c;
    c.caller = scope();
    c.kind = "calls";
    c.pos = ts::start_pos(n);
    c.arg_count = arg_count(n);
    c.arg_types = arg_types(n);
    c.text = ts::short_text(fn, src_);
    if (is(fn, "identifier")) {
      c.name = std::string(text(fn));
      c.receiver_kind = "none";
    } else if (is(fn, "template_function")) {
      c.name = std::string(text(field(fn, "name")));
      c.receiver_kind = "none";
    } else if (is(fn, "qualified_identifier")) {
      std::string raw(text(fn));
      auto lt = raw.find('<');
      std::string base = lt == std::string::npos ? raw : raw.substr(0, lt);
      auto pos = base.rfind("::");
      if (pos == std::string::npos) return;
      c.name = base.substr(pos + 2);
      c.receiver = base.substr(0, pos);
      c.receiver_kind = c.receiver.empty() ? "none" : "qualified";
    } else if (is(fn, "field_expression")) {
      TSNode arg = field(fn, "argument");
      TSNode member = field(fn, "field");
      c.name = std::string(text(member));
      if (is(member, "template_method")) c.name = std::string(text(field(member, "name")));
      if (is(member, "qualified_identifier")) c.name = std::string(text(field(member, "name")));
      c.receiver = ts::short_text(arg, src_);
      if (is(arg, "this")) {
        c.receiver_kind = "this";
      } else if (is(arg, "identifier")) {
        c.receiver_kind = "identifier";
      } else if (is(arg, "field_expression") && is(field(arg, "argument"), "this")) {
        c.receiver_kind = "this_member";
        c.receiver = std::string(text(field(arg, "field")));
      } else if (is(arg, "pointer_expression") && is(field(arg, "argument"), "identifier")) {
        c.receiver_kind = "identifier";  // (*p).f()
        c.receiver = std::string(text(field(arg, "argument")));
      } else if (is(arg, "call_expression")) {
        c.receiver_kind = "call";  // chained: a.f().g()
        awaiting_receiver_[ts::node_id(arg)] = static_cast<int>(out_.calls.size());
      } else {
        c.receiver_kind = "expression";
      }
    } else {
      c.receiver_kind = "expression";
      c.receiver = c.text;
    }
    out_.calls.push_back(std::move(c));
    if (auto it = awaiting_receiver_.find(ts::node_id(n)); it != awaiting_receiver_.end()) {
      out_.calls[static_cast<size_t>(it->second)].receiver_call = static_cast<int>(out_.calls.size()) - 1;
      awaiting_receiver_.erase(it);
    }
  }

  void on_new(TSNode n) {
    TSNode type = field(n, "type");
    std::string raw(text(type));
    if (is(type, "template_type")) raw = std::string(text(field(type, "name")));
    CallFact c;
    c.caller = scope();
    c.kind = "instantiates";
    c.pos = ts::start_pos(n);
    c.text = ts::clip("new " + std::string(text(type)));
    c.arg_count = arg_count(n);
    c.arg_types = arg_types(n);
    auto pos = raw.rfind("::");
    if (pos != std::string::npos) {
      c.receiver = raw.substr(0, pos);
      c.name = raw.substr(pos + 2);
      c.receiver_kind = "qualified";
    } else {
      c.name = raw;
      c.receiver_kind = "none";
    }
    if (!c.name.empty() && std::isupper(static_cast<unsigned char>(c.name[0]))) out_.calls.push_back(std::move(c));
  }

  // ---- routes registered in code ------------------------------------------------

  std::optional<std::string> literal(TSNode n) const {
    if (is(n, "string_literal")) {
      std::string out;
      uint32_t c = ts_node_named_child_count(n);
      for (uint32_t i = 0; i < c; ++i) out += text(ts_node_named_child(n, i));
      return out;
    }
    if (is(n, "raw_string_literal")) {
      TSNode content = ts::child_of_type(n, "raw_string_content");
      return std::string(content.id ? text(content) : "");
    }
    if (is(n, "concatenated_string")) {
      std::string out;
      uint32_t c = ts_node_named_child_count(n);
      for (uint32_t i = 0; i < c; ++i) {
        if (auto s = literal(ts_node_named_child(n, i))) out += *s;
      }
      return out;
    }
    return std::nullopt;
  }

  std::vector<TSNode> call_args(TSNode call) const {
    std::vector<TSNode> out;
    TSNode args = field(call, "arguments");
    uint32_t c = args.id ? ts_node_named_child_count(args) : 0;
    for (uint32_t i = 0; i < c; ++i) {
      TSNode a = ts_node_named_child(args, i);
      if (!is(a, "comment")) out.push_back(a);
    }
    return out;
  }

  // Last name of a callee: svr.Get -> Get, Routes::Get -> Get, app().registerHandler -> registerHandler.
  std::string callee_name(TSNode fn) const {
    if (is(fn, "field_expression")) fn = field(fn, "field");
    if (is(fn, "template_function") || is(fn, "template_method")) fn = field(fn, "name");
    std::string raw(text(fn));
    auto sep = raw.rfind("::");
    return sep == std::string::npos ? raw : raw.substr(sep + 2);
  }

  static bool all_caps(std::string_view s) {
    return !s.empty() && std::none_of(s.begin(), s.end(), [](char c) { return std::islower(static_cast<unsigned char>(c)); });
  }

  // A route handler argument: function name, &function, &Class::method, lambda,
  // std::bind(&Class::method, this), a handler object (new X, make_shared<X>, X()).
  std::optional<RefFact> handler_arg(TSNode a, TSNode* lambda) const {
    while ((is(a, "pointer_expression") || is(a, "parenthesized_expression")) && ts_node_named_child_count(a) > 0) {
      a = is(a, "pointer_expression") ? field(a, "argument") : ts_node_named_child(a, 0);
    }
    RefFact ref;
    ref.pos = ts::start_pos(a);
    ref.text = ts::short_text(a, src_);
    if (is(a, "lambda_expression")) {
      if (lambda) *lambda = a;
      ref.kind = "lambda";
      return ref;
    }
    if (is(a, "identifier")) {
      if (all_caps(text(a))) return std::nullopt;  // a constant or macro, not a function
      ref.kind = "identifier";
      ref.name = std::string(text(a));
      return ref;
    }
    if (is(a, "qualified_identifier")) {
      std::string raw(text(a));
      auto sep = raw.rfind("::");
      ref.kind = "member";
      ref.name = raw.substr(sep + 2);
      ref.receiver = raw.substr(0, sep);
      return ref;
    }
    if (is(a, "new_expression")) {
      TSNode type = field(a, "type");
      ref.kind = "class";
      ref.name = is(type, "template_type") ? std::string(text(field(type, "name"))) : std::string(text(type));
      return ref;
    }
    if (is(a, "call_expression")) {
      TSNode fn = field(a, "function");
      std::string name = callee_name(fn);
      auto inner = call_args(a);
      if ((name == "bind" || name == "mem_fn" || name == "bind_front") && !inner.empty()) return handler_arg(inner[0], lambda);
      if (name == "make_shared" || name == "make_unique") {
        TSNode tf = is(fn, "qualified_identifier") ? field(fn, "name") : fn;
        TSNode targs = field(tf, "arguments");
        if (targs.id && ts_node_named_child_count(targs) > 0) {
          ref.kind = "class";
          ref.name = std::string(text(ts_node_named_child(targs, 0)));
          return ref;
        }
      }
      if (!name.empty() && std::isupper(static_cast<unsigned char>(name[0])) && !all_caps(name)) {
        ref.kind = "class";  // a handler object constructed in place: UserHandler()
        ref.name = std::string(text(fn));
        return ref;
      }
      return std::nullopt;
    }
    return std::nullopt;
  }

  int add_route(const std::string& framework, const std::string& method, const std::string& path, TSNode at,
                const std::string& registrar, const std::string& confidence, const std::string& note) {
    RouteFact r;
    r.framework = framework;
    r.method = method;
    r.path = path;
    r.registrar = registrar;
    r.scope = scope();
    r.pos = ts::start_pos(at);
    r.confidence = confidence;
    r.note = note;
    out_.routes.push_back(std::move(r));
    return static_cast<int>(out_.routes.size()) - 1;
  }

  void attach_handler(int route, const RefFact& ref, TSNode lambda) {
    if (ref.kind == "lambda") {
      pending_lambda_[ts::node_id(lambda)] = route;
      return;
    }
    auto& r = out_.routes[static_cast<size_t>(route)];
    r.has_handler = true;
    r.handler = ref;
  }

  void on_lambda(TSNode n) {
    auto it = pending_lambda_.find(ts::node_id(n));
    if (it == pending_lambda_.end()) return;
    const auto& r = out_.routes[static_cast<size_t>(it->second)];
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

  std::string enclosing_function_name() const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      const auto& k = symbol(it->symbol).kind;
      if (k == "function" || k == "method" || k == "constructor") return symbol(it->symbol).name;
    }
    return "(file scope)";
  }

  std::vector<std::string> methods_in(TSNode n, int depth = 0) const {
    std::vector<std::string> out;
    if (!n.id || depth > 4) return out;
    if (auto s = literal(n)) {
      if (auto m = route_util::http_method_name(*s)) out.push_back(*m);
      return out;
    }
    if (is(n, "identifier") || is(n, "qualified_identifier") || is(n, "field_expression")) {
      if (auto m = route_util::http_method_name(text(n))) out.push_back(*m);
      return out;
    }
    uint32_t c = ts_node_named_child_count(n);
    for (uint32_t i = 0; i < c; ++i) {
      for (auto& m : methods_in(ts_node_named_child(n, i), depth + 1)) {
        if (std::find(out.begin(), out.end(), m) == out.end()) out.push_back(m);
      }
    }
    return out;
  }

  // svr.Get("/x", h); router.add_route("/x", &f); app().registerHandler("/x", h, {Get});
  // Routes::Get(router, "/x", Routes::bind(&Api::f, this)); server.on("/x", HTTP_GET, h);
  // CROW_ROUTE(app, "/x")([](){...})
  void on_route_call(TSNode call) {
    TSNode fn = field(call, "function");
    if (is(fn, "call_expression")) {
      std::string macro = callee_name(field(fn, "function"));
      if (macro == "CROW_ROUTE" || macro == "CROW_BP_ROUTE") {
        auto margs = call_args(fn);
        auto path = margs.size() >= 2 ? literal(margs[1]) : std::nullopt;
        auto hargs = call_args(call);
        if (!path || hargs.empty()) return;
        TSNode lambda{};
        auto h = handler_arg(hargs[0], &lambda);
        int r = add_route("crow", "ANY", *path, call, "CROW_ROUTE", "recognized", "");
        if (h) attach_handler(r, *h, lambda);
      }
      return;
    }
    std::string name = callee_name(fn);
    std::string lname;
    for (char ch : name) lname += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    static const std::unordered_set<std::string> kNames = {
        "get", "post", "put", "delete", "del", "patch", "options", "head", "route", "add_route", "addroute",
        "register_route", "registerroute", "registerhandler", "register_handler", "addhandler", "add_handler",
        "handle", "handlefunc", "on", "map", "bind", "resource", "endpoint", "set_handler", "sethandler",
        "setrequesthandler", "set_request_handler", "mg_set_request_handler", "add", "support"};
    std::string receiver = is(fn, "field_expression") ? std::string(text(field(fn, "argument"))) : "";
    std::string lreceiver;
    for (char ch : receiver) lreceiver += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    bool table_insert = (lname == "emplace" || lname == "insert" || lname == "try_emplace" || lname == "push_back") &&
                        (lreceiver.find("route") != std::string::npos || lreceiver.find("handler") != std::string::npos ||
                         lreceiver.find("dispatch") != std::string::npos || lreceiver.find("endpoint") != std::string::npos);
    if (!kNames.count(lname) && lname.find("route") == std::string::npos && lname.find("handler") == std::string::npos &&
        !table_insert) {
      return;
    }
    auto args = call_args(call);
    std::optional<std::string> path;
    std::optional<RefFact> handler;
    TSNode lambda{};
    std::vector<std::string> methods;
    for (TSNode a : args) {
      if (auto s = literal(a)) {
        if (!path && route_util::is_route_path(*s)) path = s;
        else if (auto m = route_util::http_method_name(*s)) methods.push_back(*m);
        continue;
      }
      if (auto m = route_util::http_method_name(text(a)); m && !is(a, "call_expression")) {
        methods.push_back(*m);
        continue;
      }
      if (is(a, "initializer_list")) {
        for (auto& m : methods_in(a)) methods.push_back(m);
        continue;
      }
      if (path) {
        TSNode l{};
        if (auto h = handler_arg(a, &l)) {
          handler = h;
          lambda = l;
        }
      }
    }
    if (!path || !handler) return;
    if (auto verb = route_util::http_method_name(name); verb && methods.empty()) methods.push_back(*verb);
    if (methods.empty()) methods.push_back("ANY");
    bool known = !http_library_.empty();
    for (const auto& m : methods) {
      int r = add_route(known ? http_library_ : "cpp_http", m, *path, call, receiver.empty() ? name : receiver,
                        known ? "recognized" : "heuristic", known ? "" : "registration call " + name + "()");
      attach_handler(r, *handler, lambda);
    }
  }

  // Route tables: { "/users", handle_users }, { "GET", "/users", &Api::list }
  void on_route_table(TSNode list) {
    std::optional<std::string> path;
    std::optional<RefFact> handler;
    TSNode lambda{};
    std::vector<std::string> methods;
    uint32_t c = ts_node_named_child_count(list);
    if (c < 2 || c > 6) return;
    for (uint32_t i = 0; i < c; ++i) {
      TSNode a = ts_node_named_child(list, i);
      if (is(a, "initializer_list")) return;  // a table, not an entry
      if (auto s = literal(a)) {
        if (!path && route_util::is_route_path(*s)) path = s;
        else if (auto m = route_util::http_method_name(*s)) methods.push_back(*m);
        continue;
      }
      if (auto m = route_util::http_method_name(text(a)); m && (is(a, "identifier") || is(a, "qualified_identifier"))) {
        methods.push_back(*m);
        continue;
      }
      if (!handler) {
        TSNode l{};
        if (auto h = handler_arg(a, &l)) {
          handler = h;
          lambda = l;
        }
      }
    }
    if (!path || !handler) return;
    std::string table = "route table";
    for (TSNode p = ts_node_parent(list); p.id; p = ts_node_parent(p)) {
      if (is(p, "init_declarator")) {
        table = declarator_name(field(p, "declarator"));
        break;
      }
      if (is(p, "compound_statement") || is(p, "translation_unit")) break;
    }
    if (methods.empty()) methods.push_back("ANY");
    for (const auto& m : methods) {
      int r = add_route("cpp_table", m, *path, list, table, "heuristic", "entry of " + table);
      attach_handler(r, *handler, lambda);
    }
  }

  // routes["/users"] = handle_users;
  void on_route_assignment(TSNode n) {
    TSNode left = field(n, "left");
    if (!is(left, "subscript_expression")) return;
    std::optional<std::string> path;
    uint32_t c = ts_node_named_child_count(left);
    for (uint32_t i = 0; i < c && !path; ++i) {
      TSNode part = ts_node_named_child(left, i);
      if (auto s = literal(part)) path = s;
      uint32_t k = ts_node_named_child_count(part);
      for (uint32_t j = 0; j < k && !path; ++j) {
        if (auto s = literal(ts_node_named_child(part, j))) path = s;
      }
    }
    if (!path || !route_util::is_route_path(*path)) return;
    TSNode lambda{};
    auto h = handler_arg(field(n, "right"), &lambda);
    if (!h) return;
    std::string table(text(field(left, "argument")));
    int r = add_route("cpp_table", "ANY", *path, n, table, "heuristic", "entry of " + table);
    attach_handler(r, *h, lambda);
  }

  // if (uri == "/users") handle_users(req); else if (strcmp(path, "/orders") == 0) ...
  void collect_literals(TSNode n, std::vector<std::string>& paths, std::vector<std::string>& methods, int depth) const {
    if (!n.id || depth > 8) return;
    if (auto s = literal(n)) {
      if (route_util::names_resource(*s)) paths.push_back(*s);
      else if (auto m = route_util::http_method_name(*s)) methods.push_back(*m);
      return;
    }
    if (is(n, "lambda_expression") || is(n, "compound_statement")) return;
    uint32_t c = ts_node_named_child_count(n);
    for (uint32_t i = 0; i < c; ++i) collect_literals(ts_node_named_child(n, i), paths, methods, depth + 1);
  }

  // The handler of a dispatch branch: a call that handles the request (or the
  // first call that is not logging/output), or a handler object `new X`.
  std::optional<RefFact> branch_handler(TSNode body) const {
    std::optional<RefFact> first, preferred;
    std::vector<TSNode> stack{body};
    int visited = 0;
    while (!stack.empty() && visited < 200 && !preferred) {
      TSNode n = stack.back();
      stack.pop_back();
      ++visited;
      if (is(n, "if_statement") && ts::node_id(n) != ts::node_id(body)) continue;  // a nested dispatch
      if (is(n, "new_expression")) {
        TSNode t = field(n, "type");
        RefFact ref;
        ref.kind = "class";
        ref.name = is(t, "template_type") ? std::string(text(field(t, "name"))) : std::string(text(t));
        ref.text = ts::short_text(n, src_);
        ref.pos = ts::start_pos(n);
        if (std::isupper(static_cast<unsigned char>(ref.name.empty() ? 'a' : ref.name[0]))) preferred = ref;
        continue;
      }
      if (is(n, "call_expression")) {
        TSNode fn = field(n, "function");
        std::string name = callee_name(fn);
        if (!route_util::is_noise_call(name) && !name.empty()) {
          RefFact ref;
          ref.pos = ts::start_pos(n);
          ref.text = ts::short_text(fn, src_);
          if (is(fn, "identifier")) {
            ref.kind = "identifier";
            ref.name = name;
          } else if (is(fn, "qualified_identifier")) {
            std::string raw(text(fn));
            ref.kind = "member";
            ref.name = name;
            ref.receiver = raw.substr(0, raw.rfind("::"));
          } else if (is(fn, "field_expression")) {
            TSNode obj = field(fn, "argument");
            ref.name = name;
            if (is(obj, "this")) {
              ref.kind = "method_ref";
              ref.receiver = "this";
            } else {
              ref.kind = "member";
              ref.receiver = std::string(text(obj));
            }
          }
          if (!ref.kind.empty()) {
            if (route_util::is_handler_like(name)) preferred = ref;
            else if (!first) first = ref;
          }
        }
      }
      // depth-first, in source order
      uint32_t c = ts_node_named_child_count(n);
      for (uint32_t i = c; i-- > 0;) stack.push_back(ts_node_named_child(n, i));
    }
    return preferred ? preferred : first;
  }

  void on_if_dispatch(TSNode n) {
    TSNode cond = field(n, "condition");
    if (!route_util::mentions_request(text(cond))) return;
    std::vector<std::string> paths, methods;
    collect_literals(cond, paths, methods, 0);
    if (paths.empty() || paths.size() > 4) return;
    auto handler = branch_handler(field(n, "consequence"));
    if (!handler) return;
    if (methods.empty()) methods.push_back("ANY");
    std::string fn = enclosing_function_name();
    for (const auto& p : paths) {
      for (const auto& m : methods) {
        int r = add_route("dispatch", m, p, n, fn, "heuristic", "branch on the request path in " + fn + "()");
        attach_handler(r, *handler, TSNode{});
      }
    }
  }

  // ---- exceptions ---------------------------------------------------------------

  static std::string clean_exception_type(std::string t) {
    for (const char* w : {"const ", "volatile "}) {
      size_t pos;
      while ((pos = t.find(w)) != std::string::npos) t.erase(pos, std::string(w).size());
    }
    std::string out;
    for (char ch : t) {
      if (ch != '&' && ch != '*' && ch != ' ') out += ch;
    }
    return out;
  }

  void on_try(TSNode n) {
    TSNode body = field(n, "body");
    if (!body.id) return;
    TryCtx ctx{ts::node_id(body), scope(), {}};
    uint32_t c = ts_node_named_child_count(n);
    for (uint32_t i = 0; i < c; ++i) {
      TSNode clause = ts_node_named_child(n, i);
      if (!is(clause, "catch_clause")) continue;
      TSNode params = field(clause, "parameters");
      ExceptionFact f;
      f.kind = "catch";
      f.text = ts::short_text(params, src_);
      f.scope = scope();
      f.pos = ts::start_pos(clause);
      if (text(params).find("...") != std::string_view::npos) {
        f.type = "*";
      } else {
        TSNode p = ts::child_of_type(params, "parameter_declaration");
        f.type = p.id ? clean_exception_type(std::string(text(field(p, "type")))) : "*";
      }
      out_.exceptions.push_back(std::move(f));
      ctx.catches.push_back(static_cast<int>(out_.exceptions.size()) - 1);
    }
    if (!ctx.catches.empty()) pending_try_[ctx.body] = ctx;
  }

  void on_throw(TSNode n) {
    TSNode e = ts_node_named_child(n, 0);
    ExceptionFact f;
    f.kind = e.id ? "throw" : "rethrow";
    f.scope = scope();
    f.pos = ts::start_pos(n);
    f.text = e.id ? ts::short_text(e, src_) : "throw";
    if (is(e, "call_expression")) {
      TSNode fn = field(e, "function");
      if (is(fn, "identifier") || is(fn, "qualified_identifier")) f.type = std::string(text(fn));
    } else if (is(e, "new_expression")) {
      f.type = std::string(text(field(e, "type")));
    } else if (is(e, "compound_literal_expression") || is(e, "initializer_list")) {
      f.type = std::string(text(field(e, "type")));
    }
    for (auto it = tries_.rbegin(); it != tries_.rend() && f.handler < 0; ++it) {
      if (it->scope != f.scope) break;
      for (int c : it->catches) {
        const auto& t = out_.exceptions[static_cast<size_t>(c)].type;
        auto simple = [](const std::string& s) { auto p = s.rfind("::"); return p == std::string::npos ? s : s.substr(p + 2); };
        if (!f.type.empty() && (t == f.type || simple(t) == simple(f.type))) {
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

void extract_cpp(const ExtractInput& in, FileFacts& out) {
  CppExtractor x(in, out);
  x.run();
}

}  // namespace proj
