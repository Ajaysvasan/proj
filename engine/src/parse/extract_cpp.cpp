// C and C++ extractor (the C grammar produces a subset of the same nodes).
//
// Extracts #include directives, namespaces, classes/structs/unions/enums,
// function and method definitions and declarations (including out-of-line
// `A::f()` definitions), declared types of parameters, locals and fields,
// calls, `new` expressions, throw/try/catch, and `main` entry points.
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

const std::unordered_set<std::string_view> kCatchAll = {"*", "std::exception", "exception"};

class CppExtractor {
 public:
  CppExtractor(const ExtractInput& in, FileFacts& out)
      : in_(in), out_(out), src_(in.source), cpp_(in.language == Language::Cpp) {}

  void run() {
    add_module_symbol(in_, out_);
    scopes_.push_back({ts::node_id(in_.root), 0});
    out_.depth_limited = ts::walk(in_.root, [this](TSNode n) { return enter(n); }, [this](TSNode n) { leave(n); });
  }

 private:
  const ExtractInput& in_;
  FileFacts& out_;
  std::string_view src_;
  bool cpp_;
  std::vector<Scope> scopes_;
  std::unordered_map<const void*, TryCtx> pending_try_;
  std::vector<TryCtx> tries_;
  std::unordered_map<int, std::string> access_;  // class symbol -> current access label
  std::unordered_map<const void*, int> awaiting_receiver_;  // inner call node -> outer call index

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
    else if (t == "lambda_expression") return true;
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
    CallFact c;
    c.caller = scope();
    c.kind = "calls";
    c.pos = ts::start_pos(n);
    c.arg_count = arg_count(n);
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
