// JavaScript / TypeScript / TSX extractor.
//
// Extracts declarations, ES module and CommonJS imports/exports, calls,
// constructor-based type hints, throw/try sites, Express-style routes and
// mounts, NestJS controller routes, and server start calls.
#include <array>
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

constexpr std::array<std::string_view, 8> kHttpMethods = {"get", "post", "put", "patch", "delete", "options", "head", "all"};

// Calls to these globals are language/runtime built-ins, not project
// relationships; they are counted but not stored as edges.
const std::unordered_set<std::string_view> kBuiltinReceivers = {
    "console", "Math", "JSON", "Object", "Array", "Promise", "Number", "String", "Date", "Reflect",
    "Symbol", "Intl", "Atomics", "BigInt", "Boolean", "RegExp", "Error", "Map", "Set", "WeakMap", "WeakSet"};
const std::unordered_set<std::string_view> kBuiltinFunctions = {
    "parseInt", "parseFloat", "isNaN", "isFinite", "setTimeout", "setInterval", "clearTimeout", "clearInterval",
    "setImmediate", "clearImmediate", "encodeURIComponent", "decodeURIComponent", "encodeURI", "decodeURI",
    "queueMicrotask", "structuredClone", "Symbol", "BigInt", "Number", "String", "Boolean", "Array", "Object"};

std::optional<std::string> http_method(std::string_view name) {
  for (auto m : kHttpMethods) {
    if (name == m) {
      std::string up(m);
      for (auto& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      return up;
    }
  }
  return std::nullopt;
}

struct Scope {
  const void* node;
  int symbol;
};

struct TryCtx {
  const void* body;
  int scope;
  int catch_fact;
};

struct PendingName {
  std::string name;
  std::string kind;
  int parent = 0;
  bool exported = false;
  bool is_static = false;
  std::string visibility;
};

class JsExtractor {
 public:
  JsExtractor(const ExtractInput& in, FileFacts& out)
      : in_(in), out_(out), src_(in.source), typescript_(in.language != Language::JavaScript) {}

  void run() {
    add_module_symbol(in_, out_);
    scopes_.push_back({ts::node_id(in_.root), 0});
    out_.depth_limited = ts::walk(
        in_.root, [this](TSNode n) { return enter(n); }, [this](TSNode n) { leave(n); });
  }

 private:
  const ExtractInput& in_;
  FileFacts& out_;
  std::string_view src_;
  bool typescript_;
  std::vector<Scope> scopes_;
  std::unordered_map<const void*, PendingName> pending_;  // nodes named by their syntactic context
  std::unordered_map<const void*, TryCtx> pending_try_;   // try body node -> catch info
  std::vector<TryCtx> tries_;
  std::vector<std::pair<const void*, std::string>> catch_params_;
  std::unordered_set<const void*> consumed_;              // require() calls already recorded
  std::unordered_map<std::string, std::string> routers_;  // variable -> app|router
  std::unordered_set<std::string> express_names_ = {"express"};
  std::unordered_set<std::string> router_ctor_names_ = {"Router"};
  std::unordered_map<int, std::string> nest_prefix_;      // class symbol -> @Controller prefix
  std::unordered_map<const void*, int> awaiting_receiver_; // inner call node -> outer call index

  std::string_view text(TSNode n) const { return ts::text(n, src_); }
  std::string short_text(TSNode n) const { return ts::short_text(n, src_); }
  int scope() const { return scopes_.back().symbol; }
  const SymbolFact& symbol(int i) const { return out_.symbols[static_cast<size_t>(i)]; }

  int enclosing_class() const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      const auto& k = symbol(it->symbol).kind;
      if (k == "class" || k == "interface") return it->symbol;
    }
    return -1;
  }

  int add_symbol(TSNode n, std::string name, std::string kind, int parent, bool push) {
    SymbolFact s;
    s.qualified_name = parent > 0 ? symbol(parent).qualified_name + "." + name : name;
    s.name = std::move(name);
    s.kind = std::move(kind);
    s.span = ts::span(n);
    s.parent = parent;
    out_.symbols.push_back(std::move(s));
    int idx = static_cast<int>(out_.symbols.size()) - 1;
    if (push) scopes_.push_back({ts::node_id(n), idx});
    return idx;
  }

  // ---- small syntax helpers -------------------------------------------------

  std::optional<std::string> string_value(TSNode n) const {
    if (is(n, "string")) {
      auto t = text(n);
      if (t.size() >= 2) return std::string(t.substr(1, t.size() - 2));
    } else if (is(n, "template_string")) {
      if (!ts::child_of_type(n, "template_substitution").id) {
        auto t = text(n);
        if (t.size() >= 2) return std::string(t.substr(1, t.size() - 2));
      }
    }
    return std::nullopt;
  }

  static bool is_function_node(TSNode n) {
    auto t = ts::type(n);
    return t == "arrow_function" || t == "function_expression" || t == "function" || t == "generator_function";
  }

  std::vector<TSNode> arguments(TSNode call) const {
    std::vector<TSNode> args;
    TSNode list = field(call, "arguments");
    if (!is(list, "arguments")) return args;
    uint32_t n = ts_node_named_child_count(list);
    for (uint32_t i = 0; i < n; ++i) {
      TSNode a = ts_node_named_child(list, i);
      if (!is(a, "comment")) args.push_back(a);
    }
    return args;
  }

  int param_count(TSNode fn) const {
    TSNode params = field(fn, "parameters");
    if (ts_node_is_null(params)) return field(fn, "parameter").id ? 1 : 0;
    int count = 0;
    uint32_t n = ts_node_named_child_count(params);
    for (uint32_t i = 0; i < n; ++i) {
      TSNode p = ts_node_named_child(params, i);
      if (is(p, "comment")) continue;
      if (is(p, "rest_pattern") || ts::child_of_type(p, "rest_pattern").id) return -1;
      ++count;
    }
    return count;
  }

  // Name of a TypeScript type annotation when it is a plain or generic type reference.
  std::string type_name(TSNode annotation) const {
    if (ts_node_is_null(annotation)) return "";
    TSNode t = is(annotation, "type_annotation") ? ts_node_named_child(annotation, 0) : annotation;
    if (is(t, "type_identifier") || is(t, "nested_type_identifier") || is(t, "identifier")) return std::string(text(t));
    if (is(t, "array_type") || is(t, "tuple_type")) return "Array";
    if (is(t, "predefined_type") || is(t, "literal_type")) return std::string(text(t));
    if (is(t, "generic_type")) {
      TSNode name = field(t, "name");
      if (ts_node_is_null(name)) name = ts_node_named_child(t, 0);
      return std::string(text(name));
    }
    return "";
  }

  // Constructor name of `new X()` / `new a.X()`.
  std::string new_type(TSNode n) const {
    TSNode ctor = field(n, "constructor");
    if (is(ctor, "identifier")) return std::string(text(ctor));
    if (is(ctor, "member_expression")) return std::string(text(field(ctor, "property")));
    return "";
  }

  // `export ...` wrapping a declaration, possibly through lexical_declaration.
  bool exported_decl(TSNode n, bool* is_default) const {
    TSNode p = ts_node_parent(n);
    if (is(p, "lexical_declaration") || is(p, "variable_declaration")) p = ts_node_parent(p);
    if (!is(p, "export_statement")) return false;
    if (is_default) {
      *is_default = false;
      uint32_t c = ts_node_child_count(p);
      for (uint32_t i = 0; i < c; ++i) {
        if (is(ts_node_child(p, i), "default")) *is_default = true;
      }
    }
    return true;
  }

  bool is_require_call(TSNode n, std::string* spec) const {
    if (!is(n, "call_expression")) return false;
    TSNode fn = field(n, "function");
    if (!is(fn, "identifier") || text(fn) != "require") return false;
    auto args = arguments(n);
    if (args.size() != 1) return false;
    auto v = string_value(args[0]);
    if (!v) return false;
    if (spec) *spec = *v;
    return true;
  }

  int add_import(const std::string& kind, const std::string& spec, TSNode at) {
    ImportFact imp;
    imp.kind = kind;
    imp.specifier = spec;
    imp.pos = ts::start_pos(at);
    imp.scope = scope();
    out_.imports.push_back(std::move(imp));
    return static_cast<int>(out_.imports.size()) - 1;
  }

  void add_export(std::string exported, std::string local, int sym, std::string kind, uint32_t line, int imp = -1) {
    ExportFact e;
    e.exported = std::move(exported);
    e.local = std::move(local);
    e.symbol = sym;
    e.kind = std::move(kind);
    e.import_index = imp;
    e.line = line;
    out_.exports.push_back(std::move(e));
  }

  void add_hint(int sc, std::string name, std::string type, std::string kind) {
    if (name.empty() || type.empty()) return;
    out_.type_hints.push_back({sc, std::move(name), std::move(type), std::move(kind)});
  }

  // ---- traversal -------------------------------------------------------------

  bool enter(TSNode n) {
    if (auto it = pending_try_.find(ts::node_id(n)); it != pending_try_.end()) tries_.push_back(it->second);
    auto t = ts::type(n);
    if (t == "comment" || t == "regex" || t == "string" || t == "hash_bang_line") return false;
    if (t == "function_declaration" || t == "generator_function_declaration") on_function_decl(n);
    else if (t == "function_signature") on_function_signature(n);
    else if (is_function_node(n)) on_function_expr(n);
    else if (t == "class_declaration" || t == "abstract_class_declaration" || t == "class") on_class(n);
    else if (t == "interface_declaration") on_named_type(n, "interface", true);
    else if (t == "type_alias_declaration") on_named_type(n, "type_alias", false);
    else if (t == "enum_declaration") on_named_type(n, "enum", false);
    else if (t == "internal_module" || t == "module") on_namespace(n);
    else if (t == "method_definition" || t == "method_signature" || t == "abstract_method_signature") on_method(n);
    else if (t == "public_field_definition" || t == "field_definition") on_field(n);
    else if (t == "object") on_object(n);
    else if (t == "pair") on_pair(n);
    else if (t == "variable_declarator") on_var(n);
    else if (t == "assignment_expression") on_assign(n);
    else if (t == "export_statement") on_export(n);
    else if (t == "import_statement") { on_import(n); return false; }
    else if (t == "call_expression") on_call(n);
    else if (t == "new_expression") on_new(n);
    else if (t == "throw_statement") on_throw(n);
    else if (t == "try_statement") on_try(n);
    else if (t == "catch_clause") on_catch(n);
    else if (t == "required_parameter" || t == "optional_parameter") on_param(n);
    return true;
  }

  void leave(TSNode n) {
    const void* id = ts::node_id(n);
    if (scopes_.size() > 1 && scopes_.back().node == id) scopes_.pop_back();
    if (!tries_.empty() && tries_.back().body == id) tries_.pop_back();
    if (!catch_params_.empty() && catch_params_.back().first == id) catch_params_.pop_back();
  }

  // ---- declarations -------------------------------------------------------------

  // Declared return type; generics keep their full text (Promise<User> is unwrapped on await).
  void record_return_type(TSNode fn, int idx) {
    TSNode ann = field(fn, "return_type");
    if (!is(ann, "type_annotation")) return;
    TSNode t = ts_node_named_child(ann, 0);
    std::string type = is(t, "generic_type") ? std::string(text(t)) : type_name(ann);
    if (!type.empty() && type != "void") add_hint(idx, "#return", type, "return");
  }

  void on_function_decl(TSNode n) {
    std::string name(text(field(n, "name")));
    if (name.empty()) name = "default";
    int idx = add_symbol(n, name, "function", scope(), true);
    record_params(n);
    record_return_type(n, idx);
    out_.symbols[static_cast<size_t>(idx)].param_count = param_count(n);
    bool is_default = false;
    if (exported_decl(n, &is_default)) {
      out_.symbols[static_cast<size_t>(idx)].exported = true;
      add_export(is_default ? "default" : name, name, idx, "symbol", ts::start_pos(n).line);
    }
  }

  void on_function_signature(TSNode n) {
    std::string name(text(field(n, "name")));
    if (name.empty()) return;
    int idx = add_symbol(n, name, "function", scope(), false);
    out_.symbols[static_cast<size_t>(idx)].is_definition = false;
    out_.symbols[static_cast<size_t>(idx)].param_count = param_count(n);
  }

  // Parameters shadow outer names; recording them (with "?" when the type is
  // unknown) keeps the resolver from binding a parameter call such as next()
  // to an unrelated module-level function.
  void record_params(TSNode fn) {
    int sc = scope();
    TSNode single = field(fn, "parameter");
    if (is(single, "identifier")) add_hint(sc, std::string(text(single)), "?", "param");
    TSNode params = field(fn, "parameters");
    if (ts_node_is_null(params)) return;
    uint32_t n = ts_node_named_child_count(params);
    for (uint32_t i = 0; i < n; ++i) {
      TSNode p = ts_node_named_child(params, i);
      if (is(p, "identifier")) add_hint(sc, std::string(text(p)), "?", "param");
      else if (is(p, "assignment_pattern") && is(field(p, "left"), "identifier")) {
        add_hint(sc, std::string(text(field(p, "left"))), "?", "param");
      }
      // TypeScript required/optional parameters are handled by on_param.
    }
  }

  void on_function_expr(TSNode n) {
    auto it = pending_.find(ts::node_id(n));
    if (it == pending_.end()) {
      std::string own(text(field(n, "name")));
      if (own.empty()) {
        record_params(n);  // anonymous callback: its names live in the enclosing symbol
        return;
      }
      // Named function expression (also how error recovery often presents a
      // declaration that follows a syntax error).
      pending_[ts::node_id(n)] = {own, "function", scope(), false, false, ""};
      it = pending_.find(ts::node_id(n));
    }
    PendingName p = std::move(it->second);
    pending_.erase(it);
    int idx = add_symbol(n, p.name, p.kind, p.parent, true);
    record_params(n);
    record_return_type(n, idx);
    auto& s = out_.symbols[static_cast<size_t>(idx)];
    s.param_count = param_count(n);
    s.exported = p.exported;
    s.is_static = p.is_static;
    s.visibility = p.visibility;
    if (p.exported) add_export(p.name, p.name, idx, "symbol", s.span.start.line);
  }

  void on_class(TSNode n) {
    std::string name(text(field(n, "name")));
    PendingName p;
    if (auto it = pending_.find(ts::node_id(n)); it != pending_.end()) {
      p = it->second;
      pending_.erase(it);
      if (name.empty()) name = p.name;
    }
    if (name.empty()) name = "<anonymous class>";
    int idx = add_symbol(n, name, "class", scope(), true);
    bool is_default = false;
    if (exported_decl(n, &is_default) || p.exported) {
      out_.symbols[static_cast<size_t>(idx)].exported = true;
      add_export(is_default ? "default" : name, name, idx, "symbol", ts::start_pos(n).line);
    }
    TSNode heritage = ts::child_of_type(n, "class_heritage");
    if (heritage.id) on_heritage(heritage, idx);
    on_class_decorators(n, idx);
  }

  void on_heritage(TSNode heritage, int cls) {
    uint32_t count = ts_node_named_child_count(heritage);
    for (uint32_t i = 0; i < count; ++i) {
      TSNode c = ts_node_named_child(heritage, i);
      if (is(c, "extends_clause")) {
        TSNode v = field(c, "value");
        if (is(v, "identifier") || is(v, "member_expression")) {
          out_.inherits.push_back({cls, std::string(text(v)), "extends", ts::start_pos(v)});
        }
      } else if (is(c, "implements_clause")) {
        uint32_t k = ts_node_named_child_count(c);
        for (uint32_t j = 0; j < k; ++j) {
          TSNode ty = ts_node_named_child(c, j);
          std::string tn = type_name(ty);
          if (!tn.empty()) out_.inherits.push_back({cls, tn, "implements", ts::start_pos(ty)});
        }
      } else if (is(c, "identifier") || is(c, "member_expression")) {
        out_.inherits.push_back({cls, std::string(text(c)), "extends", ts::start_pos(c)});
      }
    }
  }

  void on_named_type(TSNode n, const char* kind, bool push) {
    std::string name(text(field(n, "name")));
    if (name.empty()) return;
    int idx = add_symbol(n, name, kind, scope(), push);
    bool is_default = false;
    if (exported_decl(n, &is_default)) {
      out_.symbols[static_cast<size_t>(idx)].exported = true;
      add_export(is_default ? "default" : name, name, idx, "symbol", ts::start_pos(n).line);
    }
    if (std::string_view(kind) == "interface") {
      TSNode ext = ts::child_of_type(n, "extends_type_clause");
      uint32_t k = ext.id ? ts_node_named_child_count(ext) : 0;
      for (uint32_t j = 0; j < k; ++j) {
        std::string tn = type_name(ts_node_named_child(ext, j));
        if (!tn.empty()) out_.inherits.push_back({idx, tn, "extends", ts::start_pos(ts_node_named_child(ext, j))});
      }
    }
  }

  void on_namespace(TSNode n) {
    std::string name(text(field(n, "name")));
    if (name.empty()) return;
    add_symbol(n, name, "namespace", scope(), true);
  }

  void on_method(TSNode n) {
    int parent = scope();
    const std::string pk = symbol(parent).kind;  // copy: add_symbol may reallocate
    if (pk != "class" && pk != "interface" && pk != "object") return;  // method of an anonymous object literal
    std::string name(text(field(n, "name")));
    if (name.empty()) return;
    bool definition = is(n, "method_definition") && field(n, "body").id;
    int idx = add_symbol(n, name, name == "constructor" ? "constructor" : "method", parent, definition);
    if (definition) record_params(n);
    record_return_type(n, idx);
    auto& s = out_.symbols[static_cast<size_t>(idx)];
    s.is_definition = definition;
    s.param_count = param_count(n);
    uint32_t c = ts_node_child_count(n);
    for (uint32_t i = 0; i < c; ++i) {
      TSNode ch = ts_node_child(n, i);
      if (is(ch, "static")) s.is_static = true;
      if (is(ch, "accessibility_modifier")) s.visibility = std::string(text(ch));
    }
    if (!name.empty() && name[0] == '#') s.visibility = "private";
    if (pk == "class") on_method_decorators(n, idx, parent);
  }

  void on_field(TSNode n) {
    int cls = scope();
    if (symbol(cls).kind != "class") return;
    TSNode name_node = field(n, "name");
    if (ts_node_is_null(name_node)) name_node = field(n, "property");
    std::string name(text(name_node));
    if (name.empty()) return;
    TSNode value = field(n, "value");
    bool is_static = false;
    std::string visibility;
    uint32_t c = ts_node_child_count(n);
    for (uint32_t i = 0; i < c; ++i) {
      TSNode ch = ts_node_child(n, i);
      if (is(ch, "static")) is_static = true;
      if (is(ch, "accessibility_modifier")) visibility = std::string(text(ch));
    }
    if (is_function_node(value)) {
      pending_[ts::node_id(value)] = {name, "method", cls, false, is_static, visibility};
    } else if (is(value, "new_expression")) {
      add_hint(cls, name, new_type(value), "field");
    }
    std::string declared = type_name(field(n, "type"));
    if (!declared.empty()) add_hint(cls, name, declared, "field");
  }

  void on_object(TSNode n) {
    auto it = pending_.find(ts::node_id(n));
    if (it == pending_.end()) return;
    PendingName p = std::move(it->second);
    pending_.erase(it);
    int idx = add_symbol(n, p.name, "object", p.parent, true);
    out_.symbols[static_cast<size_t>(idx)].exported = p.exported;
  }

  void on_pair(TSNode n) {
    // Function-valued properties of a named object literal become methods.
    int obj = scope();
    if (symbol(obj).kind != "object") return;
    if (ts::node_id(ts_node_parent(n)) != scopes_.back().node) return;
    TSNode key = field(n, "key");
    TSNode value = field(n, "value");
    std::string name(text(key));
    if (is(key, "string")) {
      if (auto v = string_value(key)) name = *v;
    }
    if (name.empty()) return;
    bool exported = symbol(obj).name == "module.exports";
    if (is_function_node(value)) {
      pending_[ts::node_id(value)] = {name, "method", obj, false, false, ""};
      if (exported) export_by_pending_ .push_back({ts::node_id(value), name});
    } else if (exported && is(value, "identifier")) {
      add_export(name, std::string(text(value)), -1, "symbol", ts::start_pos(n).line);
    }
  }

  // Function values inside `module.exports = { ... }` are exported under their key.
  std::vector<std::pair<const void*, std::string>> export_by_pending_;

  void on_var(TSNode n) {
    TSNode name_node = field(n, "name");
    TSNode value = field(n, "value");
    int sc = scope();
    bool module_level = sc == 0;
    bool is_default = false;
    bool exported = module_level && exported_decl(n, &is_default);

    if (is(name_node, "object_pattern")) {
      on_destructuring_require(name_node, value);
      return;
    }
    if (!is(name_node, "identifier")) return;
    std::string name(text(name_node));

    std::string declared = typescript_ ? type_name(field(n, "type")) : "";
    const char* hint_kind = module_level ? "module" : "local";
    if (!declared.empty()) add_hint(sc, name, declared, hint_kind);

    if (ts_node_is_null(value)) {
      if (declared.empty()) add_hint(sc, name, "?", hint_kind);
      return;
    }
    TSNode v = value;
    bool awaited = false;
    if (is(v, "await_expression")) {
      v = ts_node_named_child(v, 0);
      awaited = true;
    }

    if (is_function_node(v)) {
      pending_[ts::node_id(v)] = {name, "function", sc, exported, false, ""};
      return;
    }
    if (declared.empty() && !is(v, "new_expression") && !router_kind(v) && !is_require_call(v, nullptr) &&
        !(is(v, "member_expression") && is_require_call(field(v, "object"), nullptr)) && !is(v, "class") &&
        !(is(v, "object") && module_level)) {
      std::string hint = "?";
      TSNode fn = field(v, "function");
      if (is(v, "call_expression") && (is(fn, "identifier") || is(fn, "member_expression"))) {
        // Typed later from the callee: external value or its declared return type.
        hint = std::string("@call:") + (awaited ? "await " : "") + ts::short_text(fn, src_, 120);
      }
      add_hint(sc, name, hint, hint_kind);
    }
    if (is(v, "class")) {
      pending_[ts::node_id(v)] = {name, "class", sc, exported, false, ""};
      return;
    }
    std::string spec;
    if (is_require_call(v, &spec)) {
      int imp = add_import("require", spec, v);
      out_.imports[static_cast<size_t>(imp)].bindings.push_back({name, "*", "module"});
      consumed_.insert(ts::node_id(v));
      if (spec == "express") express_names_.insert(name);
      return;
    }
    if (is(v, "member_expression") && is_require_call(field(v, "object"), &spec)) {
      // const x = require('./m').y
      int imp = add_import("require", spec, v);
      std::string prop(text(field(v, "property")));
      out_.imports[static_cast<size_t>(imp)].bindings.push_back({name, prop, "named"});
      consumed_.insert(ts::node_id(field(v, "object")));
      if (spec == "express" && prop == "Router") router_ctor_names_.insert(name);
      return;
    }
    if (auto kind = router_kind(v)) {
      TSNode callee = is(v, "new_expression") ? field(v, "constructor") : field(v, "function");
      add_hint(sc, name, std::string(text(callee)), hint_kind);  // e.g. express.Router (external)
      routers_[name] = kind->first;
      out_.routers.push_back({name, kind->first, kind->second, "", sc, ts::start_pos(n).line});
      if (module_level) add_variable(n, name, exported, is_default);
      return;
    }
    if (is(v, "new_expression")) {
      std::string type = new_type(v);
      add_hint(sc, name, type.empty() ? "?" : type, hint_kind);
      if (module_level) add_variable(n, name, exported, is_default);
      return;
    }
    if (is(v, "object") && module_level) {
      pending_[ts::node_id(v)] = {name, "object", sc, exported, false, ""};
      if (exported) add_export(is_default ? "default" : name, name, -1, "symbol", ts::start_pos(n).line);
    }
  }

  void add_variable(TSNode n, const std::string& name, bool exported, bool is_default) {
    int idx = add_symbol(n, name, "variable", 0, false);
    out_.symbols[static_cast<size_t>(idx)].exported = exported;
    if (exported) add_export(is_default ? "default" : name, name, idx, "symbol", ts::start_pos(n).line);
  }

  void on_destructuring_require(TSNode pattern, TSNode value) {
    std::string spec;
    if (!is_require_call(value, &spec)) return;
    int imp = add_import("require", spec, value);
    consumed_.insert(ts::node_id(value));
    uint32_t count = ts_node_named_child_count(pattern);
    for (uint32_t i = 0; i < count; ++i) {
      TSNode c = ts_node_named_child(pattern, i);
      std::string imported, local;
      if (is(c, "shorthand_property_identifier_pattern")) {
        imported = local = std::string(text(c));
      } else if (is(c, "pair_pattern")) {
        imported = std::string(text(field(c, "key")));
        TSNode v = field(c, "value");
        if (is(v, "assignment_pattern")) v = field(v, "left");
        local = std::string(text(v));
      } else if (is(c, "object_assignment_pattern")) {
        imported = local = std::string(text(field(c, "left")));
      }
      if (imported.empty() || local.empty()) continue;
      out_.imports[static_cast<size_t>(imp)].bindings.push_back({local, imported, "named"});
      if (spec == "express" && imported == "Router") router_ctor_names_.insert(local);
    }
  }

  // Recognizes express(), express.Router(), Router(), new Router(), fastify().
  std::optional<std::pair<std::string, std::string>> router_kind(TSNode v) const {
    bool is_new = is(v, "new_expression");
    if (!is_new && !is(v, "call_expression")) return std::nullopt;
    TSNode fn = is_new ? field(v, "constructor") : field(v, "function");
    if (is(fn, "identifier")) {
      auto name = text(fn);
      if (!is_new && express_names_.count(std::string(name))) return std::make_pair("app", "express");
      if (router_ctor_names_.count(std::string(name))) return std::make_pair("router", "express");
      if (!is_new && (name == "fastify" || name == "Fastify")) return std::make_pair("app", "fastify");
    } else if (is(fn, "member_expression")) {
      TSNode obj = field(fn, "object");
      auto prop = text(field(fn, "property"));
      if (prop == "Router" && is(obj, "identifier") && express_names_.count(std::string(text(obj)))) {
        return std::make_pair("router", "express");
      }
      std::string spec;
      if (prop == "Router" && is_require_call(obj, &spec) && spec == "express") return std::make_pair("router", "express");
    } else if (!is_new && is_require_call(fn, nullptr)) {
      std::string spec;
      is_require_call(fn, &spec);
      if (spec == "express") return std::make_pair("app", "express");
    }
    return std::nullopt;
  }

  void on_assign(TSNode n) {
    TSNode left = field(n, "left");
    TSNode right = field(n, "right");
    if (!is(left, "member_expression")) {
      if (is(left, "identifier")) {
        if (auto kind = router_kind(right)) {
          routers_[std::string(text(left))] = kind->first;
          out_.routers.push_back({std::string(text(left)), kind->first, kind->second, "", scope(), ts::start_pos(n).line});
        }
      }
      return;
    }
    std::string lhs(text(left));
    uint32_t line = ts::start_pos(n).line;
    if (lhs == "module.exports") {
      export_value("default", right, line);
      return;
    }
    TSNode obj = field(left, "object");
    std::string prop(text(field(left, "property")));
    std::string obj_text(text(obj));
    if (obj_text == "module.exports" || obj_text == "exports") {
      export_value(prop, right, line);
      return;
    }
    if (is(obj, "this")) {
      int cls = enclosing_class();
      if (cls >= 0 && is(right, "new_expression")) add_hint(cls, prop, new_type(right), "field");
    }
  }

  // module.exports = X / exports.name = X / export default X
  void export_value(const std::string& exported, TSNode value, uint32_t line) {
    if (is(value, "identifier")) {
      add_export(exported, std::string(text(value)), -1, "symbol", line);
    } else if (is(value, "new_expression")) {
      add_export(exported, new_type(value), -1, "instance", line);
    } else if (is_function_node(value)) {
      pending_[ts::node_id(value)] = {exported == "default" ? "module.exports" : exported, "function", 0, true, false, ""};
      if (exported == "default") {
        // The symbol is named after its export site; record the default export by symbol name.
        add_export("default", "module.exports", -1, "symbol", line);
        pending_[ts::node_id(value)].exported = false;
      }
    } else if (is(value, "class")) {
      std::string name(text(field(value, "name")));
      if (name.empty()) name = exported == "default" ? "module.exports" : exported;
      pending_[ts::node_id(value)] = {name, "class", 0, false, false, ""};
      add_export(exported, name, -1, "symbol", line);
    } else if (is(value, "object")) {
      if (exported == "default") {
        pending_[ts::node_id(value)] = {"module.exports", "object", 0, true, false, ""};
        add_export("default", "module.exports", -1, "symbol", line);
        // Shorthand properties: module.exports = { a, b }
        uint32_t c = ts_node_named_child_count(value);
        for (uint32_t i = 0; i < c; ++i) {
          TSNode p = ts_node_named_child(value, i);
          if (is(p, "shorthand_property_identifier")) add_export(std::string(text(p)), std::string(text(p)), -1, "symbol", line);
        }
      }
    } else {
      std::string spec;
      if (is_require_call(value, &spec)) {
        int imp = add_import("require", spec, value);
        consumed_.insert(ts::node_id(value));
        add_export(exported, exported == "default" ? "*" : exported, -1,
                   exported == "default" ? "reexport_all" : "reexport", line, imp);
      } else if (is(value, "member_expression") || is(value, "call_expression")) {
        // Not statically traceable; record nothing rather than guessing.
      }
    }
  }

  void on_export(TSNode n) {
    uint32_t line = ts::start_pos(n).line;
    TSNode source = field(n, "source");
    TSNode value = field(n, "value");
    TSNode clause = ts::child_of_type(n, "export_clause");
    if (source.id) {
      auto spec = string_value(source);
      if (!spec) return;
      int imp = add_import("reexport", *spec, n);
      if (clause.id) {
        uint32_t c = ts_node_named_child_count(clause);
        for (uint32_t i = 0; i < c; ++i) {
          TSNode s = ts_node_named_child(clause, i);
          if (!is(s, "export_specifier")) continue;
          std::string name(text(field(s, "name")));
          TSNode alias = field(s, "alias");
          add_export(alias.id ? std::string(text(alias)) : name, name, -1, "reexport", line, imp);
        }
      } else if (TSNode ns = ts::child_of_type(n, "namespace_export"); ns.id) {
        std::string name(text(ts_node_named_child(ns, ts_node_named_child_count(ns) - 1)));
        add_export(name, "*", -1, "reexport", line, imp);
        out_.imports[static_cast<size_t>(imp)].bindings.push_back({name, "*", "namespace"});
      } else {
        add_export("*", "*", -1, "reexport_all", line, imp);
      }
      return;
    }
    if (clause.id) {
      uint32_t c = ts_node_named_child_count(clause);
      for (uint32_t i = 0; i < c; ++i) {
        TSNode s = ts_node_named_child(clause, i);
        if (!is(s, "export_specifier")) continue;
        std::string name(text(field(s, "name")));
        TSNode alias = field(s, "alias");
        add_export(alias.id ? std::string(text(alias)) : name, name, -1, "symbol", line);
      }
      return;
    }
    if (value.id) export_value("default", value, line);
  }

  void on_import(TSNode n) {
    auto spec = string_value(field(n, "source"));
    TSNode req = ts::child_of_type(n, "import_require_clause");
    if (req.id) {
      auto rs = string_value(field(req, "source"));
      TSNode id = ts::child_of_type(req, "identifier");
      if (!rs) return;
      int imp = add_import("require", *rs, n);
      if (id.id) out_.imports[static_cast<size_t>(imp)].bindings.push_back({std::string(text(id)), "*", "module"});
      return;
    }
    if (!spec) return;
    bool type_only = false;
    uint32_t c = ts_node_child_count(n);
    for (uint32_t i = 0; i < c && i < 3; ++i) {
      if (is(ts_node_child(n, i), "type")) type_only = true;
    }
    int imp = add_import(type_only ? "type_import" : "import", *spec, n);
    TSNode clause = ts::child_of_type(n, "import_clause");
    if (!clause.id) return;
    auto& bindings = out_.imports[static_cast<size_t>(imp)].bindings;
    uint32_t k = ts_node_named_child_count(clause);
    for (uint32_t i = 0; i < k; ++i) {
      TSNode ch = ts_node_named_child(clause, i);
      if (is(ch, "identifier")) {
        bindings.push_back({std::string(text(ch)), "default", "default"});
        if (*spec == "express") express_names_.insert(std::string(text(ch)));
      } else if (is(ch, "namespace_import")) {
        TSNode id = ts::child_of_type(ch, "identifier");
        if (id.id) bindings.push_back({std::string(text(id)), "*", "namespace"});
      } else if (is(ch, "named_imports")) {
        uint32_t m = ts_node_named_child_count(ch);
        for (uint32_t j = 0; j < m; ++j) {
          TSNode s = ts_node_named_child(ch, j);
          if (!is(s, "import_specifier")) continue;
          std::string name(text(field(s, "name")));
          TSNode alias = field(s, "alias");
          std::string local = alias.id ? std::string(text(alias)) : name;
          bindings.push_back({local, name, name == "default" ? "default" : "named"});
          if (*spec == "express" && name == "Router") router_ctor_names_.insert(local);
        }
      }
    }
  }

  // ---- calls ----------------------------------------------------------------

  void on_call(TSNode n) {
    TSNode fn = field(n, "function");
    std::string spec;
    if (is_require_call(n, &spec)) {
      if (!consumed_.count(ts::node_id(n))) add_import("require", spec, n);
      return;
    }
    if (is(fn, "import")) {
      auto args = arguments(n);
      if (!args.empty()) {
        if (auto v = string_value(args[0])) add_import("dynamic_import", *v, n);
      }
      return;
    }
    if (is(fn, "call_expression") && is_require_call(fn, &spec)) {
      // require('./routes')(app): calls the default export of a local module.
      add_import("require", spec, fn);
      consumed_.insert(ts::node_id(fn));
      CallFact c;
      c.caller = scope();
      c.kind = "calls";
      c.name = "default";
      c.receiver = spec;
      c.receiver_kind = "module_require";
      c.text = short_text(fn);
      c.arg_count = static_cast<int>(arguments(n).size());
      c.pos = ts::start_pos(n);
      out_.calls.push_back(std::move(c));
      return;
    }

    if (is(fn, "member_expression")) {
      TSNode obj = field(fn, "object");
      std::string prop(text(field(fn, "property")));
      if (auto method = http_method(prop)) {
        if (try_route(n, obj, *method)) return add_call(n, fn);
      }
      if (prop == "use") try_use(n, obj);
      if (prop == "listen") try_listen(n, obj);
    }
    add_call(n, fn);
  }

  void add_call(TSNode n, TSNode fn) {
    CallFact c;
    c.caller = scope();
    c.kind = "calls";
    c.pos = ts::start_pos(n);
    c.arg_count = static_cast<int>(arguments(n).size());
    c.text = short_text(fn);
    if (is(fn, "identifier")) {
      c.name = std::string(text(fn));
      c.receiver_kind = "none";
      if (kBuiltinFunctions.count(c.name)) return;
    } else if (is(fn, "member_expression")) {
      TSNode obj = field(fn, "object");
      c.name = std::string(text(field(fn, "property")));
      describe_receiver(obj, c);
      if (c.receiver_kind == "identifier" && kBuiltinReceivers.count(c.receiver)) return;
      if (c.receiver_kind == "call") {
        TSNode inner = obj;
        while (is(inner, "parenthesized_expression") || is(inner, "await_expression")) inner = ts_node_named_child(inner, 0);
        out_.calls.push_back(std::move(c));
        awaiting_receiver_[ts::node_id(inner)] = static_cast<int>(out_.calls.size()) - 1;
        link_receiver(n);
        return;
      }
    } else if (is(fn, "super")) {
      c.name = "constructor";
      c.receiver = "super";
      c.receiver_kind = "super";
    } else {
      c.receiver_kind = "expression";
      c.receiver = c.text;
    }
    out_.calls.push_back(std::move(c));
    link_receiver(n);
  }

  // If an outer call is waiting for this call's result as its receiver, link them.
  void link_receiver(TSNode n) {
    if (auto it = awaiting_receiver_.find(ts::node_id(n)); it != awaiting_receiver_.end()) {
      out_.calls[static_cast<size_t>(it->second)].receiver_call = static_cast<int>(out_.calls.size()) - 1;
      awaiting_receiver_.erase(it);
    }
  }

  void describe_receiver(TSNode obj, CallFact& c) const {
    c.receiver = short_text(obj);
    if (is(obj, "this")) {
      c.receiver_kind = "this";
    } else if (is(obj, "super")) {
      c.receiver_kind = "super";
    } else if (is(obj, "identifier")) {
      c.receiver_kind = "identifier";
    } else if (is(obj, "member_expression") && is(field(obj, "object"), "this") &&
               (is(field(obj, "property"), "property_identifier") || is(field(obj, "property"), "private_property_identifier"))) {
      c.receiver_kind = "this_member";
      c.receiver = std::string(text(field(obj, "property")));
    } else if (is(obj, "member_expression") && is_identifier_chain(obj)) {
      c.receiver_kind = "qualified";
    } else {
      TSNode inner = obj;
      while (is(inner, "parenthesized_expression") || is(inner, "non_null_expression")) inner = ts_node_named_child(inner, 0);
      if (is(inner, "new_expression") && !new_type(inner).empty()) {
        c.receiver_kind = "typed";
        c.receiver = new_type(inner);
      } else if ((is(inner, "as_expression") || is(inner, "satisfies_expression")) && !type_name(ts_node_named_child(inner, 1)).empty()) {
        c.receiver_kind = "typed";
        c.receiver = type_name(ts_node_named_child(inner, 1));
      } else if (is(inner, "call_expression") || (is(inner, "await_expression") && is(ts_node_named_child(inner, 0), "call_expression"))) {
        c.receiver_kind = "call";  // chained: a.f().g() / (await a.f()).g()
      } else {
        c.receiver_kind = "expression";
      }
    }
  }

  bool is_identifier_chain(TSNode n) const {
    while (is(n, "member_expression")) n = field(n, "object");
    return is(n, "identifier");
  }

  void on_new(TSNode n) {
    TSNode ctor = field(n, "constructor");
    CallFact c;
    c.caller = scope();
    c.kind = "instantiates";
    c.pos = ts::start_pos(n);
    c.text = short_text(ctor);
    c.arg_count = static_cast<int>(arguments(n).size());
    if (is(ctor, "identifier")) {
      c.name = std::string(text(ctor));
      c.receiver_kind = "none";
      if (kBuiltinReceivers.count(c.name) || c.name == "Error" || c.name == "Date") return;
    } else if (is(ctor, "member_expression")) {
      c.name = std::string(text(field(ctor, "property")));
      describe_receiver(field(ctor, "object"), c);
    } else {
      return;
    }
    out_.calls.push_back(std::move(c));
  }

  // ---- routes (Express-style) ------------------------------------------------

  // Resolves `X.route('/p').get(...)` chains to ('/p', "X").
  bool route_chain(TSNode obj, std::string& path, std::string& registrar) const {
    while (is(obj, "call_expression")) {
      TSNode fn = field(obj, "function");
      if (!is(fn, "member_expression")) return false;
      std::string prop(text(field(fn, "property")));
      if (prop == "route") {
        auto args = arguments(obj);
        if (args.empty()) return false;
        auto v = string_value(args[0]);
        if (!v) return false;
        path = *v;
        registrar = short_text(field(fn, "object"));
        return true;
      }
      if (!http_method(prop)) return false;
      obj = field(fn, "object");
    }
    return false;
  }

  // HTTP client calls such as axios.get('/users', config) share the route
  // call shape; skip them when the receiver is not a recognized router.
  bool looks_like_http_client(const std::string& receiver, TSNode last_arg) const {
    static const std::unordered_set<std::string> kClients = {
        "axios", "http", "https", "request", "superagent", "supertest", "fetch", "client", "api", "$http",
        "ky", "got", "instance", "agent", "httpClient", "apiClient"};
    if (kClients.count(receiver)) return true;
    auto t = ts::type(last_arg);
    return t == "object" || t == "array" || t == "string" || t == "number" || t == "template_string" ||
           t == "true" || t == "false" || t == "null";
  }

  bool try_route(TSNode call, TSNode obj, const std::string& method) {
    auto args = arguments(call);
    std::string path, registrar;
    size_t first_ref = 1;
    if (route_chain(obj, path, registrar)) {
      first_ref = 0;
    } else {
      if (!is(obj, "identifier") && !is(obj, "member_expression")) return false;
      if (args.size() < 2) return false;
      registrar = short_text(obj);
      if (!routers_.count(registrar) && looks_like_http_client(registrar, args.back())) return false;
      auto v = string_value(args[0]);
      if (!v) {
        // A route on a known app/router whose path is computed at runtime.
        if (!routers_.count(registrar)) return false;
        path = "<dynamic: " + short_text(args[0]) + ">";
        out_.notes.push_back("line " + std::to_string(ts::start_pos(call).line) +
                             ": route path is not a string literal; recorded as unresolved pattern");
      } else {
        path = *v;
        if (path.empty() || (path[0] != '/' && path[0] != '*')) return false;
      }
    }
    if (args.size() <= first_ref) return false;
    RouteFact r;
    r.framework = "express";
    r.method = method;
    r.path = path;
    r.registrar = registrar;
    r.scope = scope();
    r.pos = ts::start_pos(call);
    r.confidence = routers_.count(registrar) ? "recognized" : "heuristic";
    if (r.confidence == "heuristic") r.note = "receiver '" + registrar + "' is not a recognized Express app or router";
    std::string label = method + " " + path;
    for (size_t i = first_ref; i < args.size(); ++i) {
      bool last = i + 1 == args.size();
      std::string name = last ? label : "middleware#" + std::to_string(i - first_ref + 1) + " " + label;
      RefFact ref = make_ref(args[i], name, last ? "route_handler" : "middleware");
      if (last) {
        r.handler = std::move(ref);
        r.has_handler = true;
      } else {
        r.middleware.push_back(std::move(ref));
      }
    }
    out_.routes.push_back(std::move(r));
    return true;
  }

  // Describes a handler/middleware argument. Inline functions get their own
  // symbol so calls made inside them are attributed to the handler.
  RefFact make_ref(TSNode arg, const std::string& inline_name, const char* inline_kind) {
    RefFact ref;
    ref.pos = ts::start_pos(arg);
    ref.text = short_text(arg);
    TSNode a = arg;
    if (is(a, "call_expression") && !is_require_call(a, nullptr)) {
      // asyncHandler(fn) / catchAsync(async (req, res) => {...})
      auto inner = arguments(a);
      TSNode wfn = field(a, "function");
      if (inner.size() == 1 && (is_function_node(inner[0]) || is(inner[0], "identifier") || is(inner[0], "member_expression"))) {
        ref.wrapper = short_text(wfn);
        a = inner[0];
      }
    }
    std::string spec;
    if (is_function_node(a)) {
      // The symbol is created when the walker reaches the function node;
      // finish() links it back by name and position.
      ref.kind = "inline";
      ref.name = inline_name;
      ref.pos = ts::start_pos(a);
      pending_[ts::node_id(a)] = {inline_name, inline_kind, scope(), false, false, ""};
    } else if (is(a, "identifier")) {
      ref.kind = "identifier";
      ref.name = std::string(text(a));
    } else if (is(a, "member_expression")) {
      ref.kind = "member";
      ref.name = std::string(text(field(a, "property")));
      TSNode obj = field(a, "object");
      if (is(obj, "member_expression") && is(field(obj, "object"), "this")) {
        ref.receiver = "this." + std::string(text(field(obj, "property")));
      } else {
        ref.receiver = short_text(obj);
      }
    } else if (is_require_call(a, &spec)) {
      ref.kind = "require";
      ref.specifier = spec;
      add_import("require", spec, a);
      consumed_.insert(ts::node_id(a));
    } else {
      ref.kind = "other";
    }
    return ref;
  }

  void try_use(TSNode call, TSNode obj) {
    if (!is(obj, "identifier") && !is(obj, "member_expression")) return;
    std::string registrar = short_text(obj);
    auto args = arguments(call);
    if (args.empty()) return;
    std::optional<std::string> prefix = string_value(args[0]);
    bool known = routers_.count(registrar) > 0;
    if (!known && !(prefix && !prefix->empty() && (*prefix)[0] == '/')) return;
    size_t first = prefix ? 1 : 0;
    for (size_t i = first; i < args.size(); ++i) {
      MountFact m;
      m.framework = "express";
      m.registrar = registrar;
      m.has_prefix = prefix.has_value();
      m.prefix = prefix.value_or("");
      m.scope = scope();
      m.pos = ts::start_pos(args[i]);
      std::string label = "use " + (prefix ? *prefix + " " : std::string()) + "middleware#" + std::to_string(i - first + 1);
      m.target = make_ref(args[i], label, "middleware");
      out_.mounts.push_back(std::move(m));
    }
  }

  void try_listen(TSNode call, TSNode obj) {
    std::string receiver = short_text(obj);
    bool known = routers_.count(receiver) && routers_[receiver] == "app";
    static const std::unordered_set<std::string> kNames = {"app", "server", "httpServer", "httpsServer", "fastify"};
    bool create_server = false;
    if (is(obj, "call_expression")) {
      auto inner = text(field(obj, "function"));
      create_server = inner.find("createServer") != std::string_view::npos;
    }
    if (!known && !kNames.count(receiver) && !create_server) return;
    out_.entries.push_back({"server_start",
                            (known ? "listen() on Express/Fastify app '" : "listen() on '") + clip_name(receiver) + "'",
                            scope(), ts::start_pos(call).line});
  }

  static std::string clip_name(const std::string& s) { return ts::clip(s, 60); }

  // ---- NestJS decorators ---------------------------------------------------

  std::vector<TSNode> decorators_of(TSNode n) const {
    std::vector<TSNode> out;
    uint32_t c = ts_node_named_child_count(n);
    for (uint32_t i = 0; i < c; ++i) {
      TSNode ch = ts_node_named_child(n, i);
      if (is(ch, "decorator")) out.push_back(ch);
    }
    // Some grammar versions attach member decorators as preceding siblings.
    TSNode prev = ts_node_prev_named_sibling(n);
    while (is(prev, "decorator")) {
      out.push_back(prev);
      prev = ts_node_prev_named_sibling(prev);
    }
    return out;
  }

  // @Name(args) -> ("Name", first string argument or "")
  std::optional<std::pair<std::string, std::string>> decorator_call(TSNode d) const {
    TSNode e = ts_node_named_child(d, 0);
    if (is(e, "call_expression")) {
      TSNode fn = field(e, "function");
      if (!is(fn, "identifier")) return std::nullopt;
      std::string arg;
      auto args = arguments(e);
      if (!args.empty()) {
        if (auto v = string_value(args[0])) arg = *v;
        else if (is(args[0], "object")) arg = object_string_prop(args[0], "path");
      }
      return std::make_pair(std::string(text(fn)), arg);
    }
    if (is(e, "identifier")) return std::make_pair(std::string(text(e)), std::string());
    return std::nullopt;
  }

  std::string object_string_prop(TSNode obj, std::string_view key) const {
    uint32_t c = ts_node_named_child_count(obj);
    for (uint32_t i = 0; i < c; ++i) {
      TSNode p = ts_node_named_child(obj, i);
      if (is(p, "pair") && text(field(p, "key")) == key) {
        if (auto v = string_value(field(p, "value"))) return *v;
      }
    }
    return "";
  }

  void on_class_decorators(TSNode n, int cls) {
    std::vector<TSNode> decorators = decorators_of(n);
    // On `export class X` the decorators belong to the export statement.
    TSNode parent = ts_node_parent(n);
    if (is(parent, "export_statement")) {
      for (TSNode d : decorators_of(parent)) decorators.push_back(d);
    }
    for (TSNode d : decorators) {
      auto call = decorator_call(d);
      if (call && call->first == "Controller") nest_prefix_[cls] = call->second;
    }
  }

  void on_method_decorators(TSNode n, int method, int cls) {
    auto prefix = nest_prefix_.find(cls);
    if (prefix == nest_prefix_.end()) return;
    for (TSNode d : decorators_of(n)) {
      auto call = decorator_call(d);
      if (!call) continue;
      std::string lower = call->first;
      for (auto& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      auto m = http_method(lower);
      if (!m || call->first[0] != std::toupper(static_cast<unsigned char>(lower[0]))) continue;
      RouteFact r;
      r.framework = "nestjs";
      r.method = *m;
      r.path = "/" + call->second;
      r.prefix = "/" + prefix->second;
      r.registrar = symbol(cls).name;
      r.scope = cls;
      r.pos = ts::start_pos(d);
      r.confidence = "recognized";
      r.has_handler = true;
      r.handler.kind = "inline";
      r.handler.name = symbol(method).name;
      r.handler.symbol = method;
      r.handler.text = symbol(method).qualified_name;
      r.handler.pos = ts::start_pos(n);
      out_.routes.push_back(std::move(r));
    }
  }

  // ---- exceptions -------------------------------------------------------------

  void on_try(TSNode n) {
    TSNode handler = field(n, "handler");
    TSNode body = field(n, "body");
    if (!handler.id || !body.id) return;
    ExceptionFact c;
    c.kind = "catch";
    c.type = "*";
    c.text = short_text(field(handler, "parameter"));
    c.scope = scope();
    c.pos = ts::start_pos(handler);
    out_.exceptions.push_back(std::move(c));
    pending_try_[ts::node_id(body)] = {ts::node_id(body), scope(), static_cast<int>(out_.exceptions.size()) - 1};
  }

  void on_catch(TSNode n) {
    TSNode p = field(n, "parameter");
    if (is(p, "identifier")) catch_params_.push_back({ts::node_id(n), std::string(text(p))});
  }

  void on_throw(TSNode n) {
    TSNode e = ts_node_named_child(n, 0);
    while (is(e, "parenthesized_expression")) e = ts_node_named_child(e, 0);
    ExceptionFact f;
    f.kind = "throw";
    f.scope = scope();
    f.pos = ts::start_pos(n);
    f.text = short_text(e);
    if (is(e, "new_expression")) {
      f.type = new_type(e);
    } else if (is(e, "call_expression")) {
      TSNode fn = field(e, "function");
      if (is(fn, "identifier") && !text(fn).empty() && std::isupper(static_cast<unsigned char>(text(fn)[0]))) {
        f.type = std::string(text(fn));  // Error("x") without new
      }
    } else if (is(e, "identifier") && !catch_params_.empty() && catch_params_.back().second == text(e)) {
      f.kind = "rethrow";
    }
    for (auto it = tries_.rbegin(); it != tries_.rend(); ++it) {
      if (it->scope != f.scope) break;  // a try in an enclosing function does not catch at definition time
      f.handler = it->catch_fact;
      f.handler_match = "catch_all";
      break;
    }
    out_.exceptions.push_back(std::move(f));
  }

  // ---- TypeScript parameters ------------------------------------------------

  void on_param(TSNode n) {
    TSNode pattern = field(n, "pattern");
    if (ts_node_is_null(pattern)) pattern = field(n, "name");
    if (!is(pattern, "identifier")) return;
    std::string name(text(pattern));
    std::string type = type_name(field(n, "type"));
    int fn = scope();
    add_hint(fn, name, type.empty() ? "?" : type, "param");
    if (type.empty()) return;
    // Constructor parameter properties (`constructor(private repo: Repo)`) are fields.
    if (ts::child_of_type(n, "accessibility_modifier").id || has_child(n, "readonly")) {
      int cls = symbol(fn).parent;
      if (cls >= 0 && symbol(cls).kind == "class") add_hint(cls, name, type, "field");
    }
  }

  static bool has_child(TSNode n, const char* t) {
    uint32_t c = ts_node_child_count(n);
    for (uint32_t i = 0; i < c; ++i) {
      if (is(ts_node_child(n, i), t)) return true;
    }
    return false;
  }

 public:
  // Fix up inline route/middleware references once symbols exist, and export
  // function-valued properties of `module.exports = {...}`.
  void finish() {
    std::unordered_map<std::string, int> by_name;
    for (size_t i = 0; i < out_.symbols.size(); ++i) {
      const auto& s = out_.symbols[i];
      if (s.kind == "route_handler" || s.kind == "middleware") by_name.emplace(s.name + "@" + std::to_string(s.span.start.line) + ":" + std::to_string(s.span.start.col), static_cast<int>(i));
    }
    auto fix = [&](RefFact& ref) {
      if (ref.kind != "inline" || ref.symbol >= 0) return;
      auto it = by_name.find(ref.name + "@" + std::to_string(ref.pos.line) + ":" + std::to_string(ref.pos.col));
      if (it != by_name.end()) ref.symbol = it->second;
    };
    for (auto& r : out_.routes) {
      fix(r.handler);
      for (auto& m : r.middleware) fix(m);
    }
    for (auto& m : out_.mounts) fix(m.target);
    for (const auto& [node, key] : export_by_pending_) {
      for (size_t i = 0; i < out_.symbols.size(); ++i) {
        const auto& s = out_.symbols[i];
        if (s.name == key && s.parent >= 0 && symbol(s.parent).name == "module.exports") {
          add_export(key, key, static_cast<int>(i), "symbol", s.span.start.line);
          break;
        }
      }
    }
  }
};

}  // namespace

void extract_javascript(const ExtractInput& in, FileFacts& out) {
  JsExtractor x(in, out);
  x.run();
  x.finish();
}

}  // namespace proj
