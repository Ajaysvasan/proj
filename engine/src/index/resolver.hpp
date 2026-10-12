#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/json.hpp"
#include "db/sqlite.hpp"
#include "index/registry.hpp"

namespace proj {

struct ResolveStats {
  int64_t deps_total = 0, deps_resolved = 0, deps_external = 0, deps_unresolved = 0, deps_other = 0;
  int64_t calls_total = 0, calls_resolved = 0, calls_ambiguous = 0, calls_heuristic = 0, calls_unresolved = 0,
          calls_external = 0;
  int64_t inherits_resolved = 0, inherits_unresolved = 0;
  int64_t override_edges = 0, module_load_edges = 0;
  int64_t routes = 0, route_handlers_resolved = 0, route_handlers_unresolved = 0;
  int64_t mounts_router = 0, mounts_middleware = 0, mounts_unresolved = 0;
  int64_t manifest_entry_points = 0;
  json to_json() const;
};

// Resolves cross-file relationships stored as "pending" rows. All lookups go
// through indexed SQL queries and small per-file caches, so memory stays
// bounded by the size of one file's facts rather than the whole graph.
class Resolver {
 public:
  Resolver(db::Database& db, std::filesystem::path root, DiagnosticSink& diagnostics,
           std::vector<std::string> manifests);
  // Full resolution: run_dependencies() then run_relationships(). Every phase
  // processes only rows in the "pending" state (and regenerates derived rows
  // that the caller deleted), so a refresh can reset a subset and re-run.
  ResolveStats run();
  void run_dependencies();
  ResolveStats run_relationships();

  // Lookup results are public so they can be unit-tested.
  struct SymbolInfo {
    int64_t id = -1, file_id = -1, parent_id = -1;
    std::string name, kind, qualified_name, container, language;
    int param_count = -1;
    bool is_definition = true;
    std::string param_types;  // normalized, comma-separated; "" when unknown
  };

  struct Value {
    enum class Kind { None, Symbol, Module, Instance, External, Unresolved };
    Kind kind = Kind::None;
    int64_t id = -1;           // symbol id (Symbol) or file id (Module)
    std::string type;          // Instance: constructed type name
    int64_t context_file = -1; // Instance: file in which `type` is resolved
    int64_t context_scope = -1; // Instance: scope in which `type` is resolved (-1: module)
    bool commonjs = false;     // Module bound through require()
    std::string note;
  };

  struct Outcome {
    std::string status;  // resolved|ambiguous|heuristic|unresolved|external
    std::vector<int64_t> targets;
    std::string note;
    std::string kind;    // optional edge kind override (instantiates)
  };

 private:
  db::Database& db_;
  std::filesystem::path root_;
  DiagnosticSink& diagnostics_;
  std::vector<std::string> manifests_;
  ResolveStats stats_;

  // current file context
  int64_t file_id_ = -1;
  std::string file_path_, file_lang_, file_module_;
  std::unordered_set<int64_t> visible_files_;  // C/C++ include closure

  std::unordered_map<int64_t, SymbolInfo> sym_cache_;
  // Prepared statements for hot-path queries, keyed by their SQL literal.
  std::unordered_map<const char*, db::Statement> stmt_cache_;
  int type_depth_ = 0;  // recursion guard for @call: type evaluation
  // Argument types of the call being resolved; cleared while evaluating the
  // types of other expressions so they never leak into unrelated lookups.
  struct CallContext {
    int64_t scope = -1;
    int arg_count = -1;
    std::vector<std::string> arg_types;
  };
  CallContext call_ctx_;
  // Narrows same-arity overloads by the static argument types of the current call.
  std::vector<SymbolInfo> narrow_by_types(const std::vector<SymbolInfo>& cands, std::string* note);
  std::string arg_static_type(const std::string& token, bool cpp);
  int type_match(const std::string& arg, const std::string& param, bool cpp);
  std::vector<std::string> ancestor_names(const std::string& type, bool cpp);
  // Memoized class lookups for overload scoring (bounded; facts do not change during a pass).
  int64_t type_class(const std::string& type, bool cpp);
  std::unordered_map<std::string, int64_t> type_class_cache_;
  std::unordered_map<int64_t, std::vector<std::string>> ancestors_cache_;
  // Memoized @call: evaluations (bounded; facts do not change during a pass).
  std::unordered_map<std::string, std::pair<int64_t, std::string>> call_type_cache_;
  int64_t call_result_type(int64_t file, int64_t scope, const std::string& spec, std::string* note);
  int64_t evaluate_call_type(int64_t file, int64_t scope, const std::string& spec, std::string* note);
  db::Statement& cached(const char* sql);
  std::unordered_map<int64_t, int64_t> module_symbol_cache_;
  std::unordered_map<int64_t, std::string> file_lang_cache_;

  db::Statement q_file_by_path_, q_files_by_name_, q_symbol_, q_children_, q_module_symbol_, q_bindings_,
      q_exports_, q_hint_, q_bases_, q_by_name_kind_, q_container_, q_qualified_, q_same_package_type_,
      q_file_lang_, q_dep_by_spec_, q_router_, q_file_symbols_named_, q_file_has_module_;

  // phases
  void resolve_dependencies();
  void resolve_python_submodule_bindings();
  void add_module_load_edges();
  void resolve_inheritance();
  void resolve_calls();
  void add_override_edges();
  void resolve_mounts();
  void resolve_route_handlers();
  void resolve_route_middleware();
  void compute_route_prefixes();
  std::vector<std::pair<std::string, int64_t>> django_prefixes(int64_t file, int depth, std::set<int64_t>& seen);
  void add_manifest_entry_points();

  // dependency helpers
  struct DepResult {
    std::string status;
    int64_t dst = -1;
    std::string note;
  };
  DepResult resolve_dep(const std::string& src_path, const std::string& lang, const std::string& kind,
                        const std::string& spec);
  DepResult resolve_js_dep(const std::string& src_path, const std::string& spec);
  DepResult resolve_python_dep(const std::string& src_path, const std::string& spec);
  bool python_dir_exists(const std::string& dir);
  std::optional<std::string> python_package_dir(const std::string& src_path, const std::string& spec);
  DepResult resolve_java_dep(const std::string& spec);
  DepResult resolve_c_dep(const std::string& src_path, const std::string& spec, bool system);
  std::optional<int64_t> file_by_path(const std::string& path);
  std::optional<int64_t> first_existing(const std::vector<std::string>& candidates);

 public:
  // symbol/value helpers (exposed for tests)
  std::optional<SymbolInfo> symbol(int64_t id);
  std::vector<SymbolInfo> children_named(int64_t parent, const std::string& name);
  int64_t module_symbol(int64_t file_id);
  std::string language_of_file(int64_t file_id);
  void set_file_context(int64_t file_id);

  Value resolve_name(int64_t file, int64_t scope, const std::string& name, int depth = 0);
  Value export_value(int64_t file, const std::string& name, int depth = 0);
  Outcome member_of(const Value& v, const std::string& name, int arg_count, int depth = 0);
  int64_t resolve_type(int64_t file, int64_t scope, const std::string& type, std::string* note = nullptr);
  Outcome method_lookup(int64_t cls, const std::string& name, int arg_count, int depth = 0);
  int64_t enclosing_class(int64_t scope);
  std::optional<std::string> hint_in_scope_chain(int64_t scope, const std::string& name);
  Outcome resolve_call(int64_t scope, const std::string& kind, const std::string& name, const std::string& receiver,
                       const std::string& receiver_kind, int arg_count);

  // References by name, from configuration files (web.xml, Struts, Spring XML,
  // OpenAPI, serverless.yml, route files) and class-based registrations.
  Outcome config_class(const std::string& ref);  // com.x.Y, pkg.mod.Y, ns::Y, Y
  Outcome config_member(const std::string& qualified, const std::string& member);  // com.x.Y.m, pkg.mod.f, ns::Y::m
  Outcome config_function_name(const std::string& name);  // a bare function or method name
  Outcome config_file(const std::string& path, const std::string& member);  // a source file, optionally a function in it
  Outcome config_bean(const std::string& id);  // a Spring XML bean
  // A class named in code (import-aware for Java, scope-aware for Python), else by name.
  Outcome resolve_class_ref(int64_t file, int64_t scope, const std::string& name);

  // The methods of a handler class that serve a route, using the framework's
  // dispatch convention (servlet doGet/doPost/service, Struts execute, Spring
  // handleRequest, Python get/post, Falcon on_get, generic handle/process...).
  struct DispatchTarget {
    std::string method;  // HTTP method served
    int64_t symbol = -1;
    std::string note;
  };
  std::vector<DispatchTarget> dispatch_targets(int64_t cls, const std::string& framework, const std::string& route_method,
                                               const std::string& explicit_method, std::string* note);

 private:
  Outcome resolve_typed_receiver(int64_t scope, const std::string& type, const std::string& name, int arg_count);
  std::string external_base(int64_t cls, int depth);
  Outcome java_static_import(const std::string& name, int args, bool inherited_external);

 public:
  // A call whose receiver is the result of `receiver` (another call).
  Outcome resolve_chained(const std::string& receiver_text, const Outcome& receiver, const std::string& name,
                          int arg_count);

 private:
  Outcome resolve_call_dynamic(int64_t scope, const std::string& kind, const std::string& name,
                               const std::string& receiver, const std::string& receiver_kind, int arg_count);
  Outcome resolve_call_java(int64_t scope, const std::string& kind, const std::string& name,
                            const std::string& receiver, const std::string& receiver_kind, int arg_count);
  Outcome resolve_call_cpp(int64_t scope, const std::string& kind, const std::string& name,
                           const std::string& receiver, const std::string& receiver_kind, int arg_count);
  Outcome resolve_call_config(const std::string& name, const std::string& receiver, const std::string& receiver_kind);
  // A route handler or middleware reference; `cls` receives the handler class when it is one.
  Outcome resolve_handler_ref(int64_t file, int64_t scope, const std::string& kind, const std::string& name,
                              const std::string& receiver, int64_t* cls);
  Outcome value_call(const Value& v, int arg_count, bool instantiate);
  Outcome instantiate(int64_t cls, int arg_count);
  Outcome heuristic(const std::string& name, const std::string& receiver_kind);
  Outcome pick(std::vector<SymbolInfo> cands, int arg_count, const std::string& resolved_note);
  int64_t java_type(int64_t file, const std::string& type, std::string* note);
  int64_t cpp_type(int64_t scope, const std::string& type);
  void compute_visible_files();
  std::string family(const std::string& lang) const;
};

}  // namespace proj
