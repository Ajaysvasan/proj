#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Compact, language-neutral facts extracted from one source file. Extractors
// fill a FileFacts from a syntax tree; the tree is released immediately after
// extraction and only these facts are persisted.
//
// Indexes such as `parent`, `scope` or `caller` refer to positions in
// FileFacts::symbols. symbols[0] is always the file's module symbol, which
// owns top-level code.
namespace proj {

struct Pos {
  uint32_t line = 0;  // 1-based
  uint32_t col = 0;   // 1-based byte column
};

struct Span {
  Pos start;
  Pos end;
};

struct SymbolFact {
  std::string name;
  std::string qualified_name;
  std::string kind;  // module|function|method|constructor|class|interface|struct|enum|type_alias|namespace|variable|object
  Span span;
  int parent = -1;
  bool exported = false;
  std::string visibility;
  bool is_static = false;
  bool is_definition = true;
  int param_count = -1;   // -1 when unknown or variadic
  std::string container;  // logical owner when it is not the lexical parent (C++ out-of-line methods)
  std::string signature;
};

struct BindingFact {
  std::string local;     // name visible in this file
  std::string imported;  // exported name in the target, "default", or "*"
  std::string kind;      // named|default|namespace|module
};

struct ImportFact {
  std::string kind;       // import|type_import|require|dynamic_import|reexport|include|from_import
  std::string specifier;  // module text as written, without quotes
  Pos pos;
  int scope = 0;          // symbol owning the import (0 = module level)
  bool system = false;    // C/C++ <...> include
  std::vector<BindingFact> bindings;
};

struct ExportFact {
  std::string exported;   // exported name ("default" for default exports)
  std::string local;      // local identifier, when exported by name
  int symbol = -1;        // symbol index when known directly
  std::string kind;       // symbol|instance|reexport|reexport_all
  int import_index = -1;  // re-exports: index into FileFacts::imports
  uint32_t line = 0;
};

struct CallFact {
  int caller = 0;
  std::string kind;           // calls|instantiates
  std::string name;           // simple callee name
  std::string receiver;       // receiver expression text, bounded
  // none|this|super|identifier|this_member|qualified|expression|module_require
  // call:  the receiver is the result of another call (receiver_call)
  // typed: the receiver's static type is written in the code (new X().m(), casts); `receiver` holds the type
  std::string receiver_kind;
  std::string text;           // callee expression text, bounded
  int arg_count = -1;
  Pos pos;
  int receiver_call = -1;  // index into FileFacts::calls when receiver_kind == "call"
};

struct TypeHintFact {
  int scope = 0;      // function/method (local, param), class (field) or module
  std::string name;   // variable, parameter or field name
  std::string type;   // declared or constructed type name
  std::string kind;   // local|param|field|module|return ("#return" hint on a function: declared return type)
};

struct InheritFact {
  int symbol = 0;
  std::string base;
  std::string kind;  // extends|implements
  Pos pos;
};

// A reference to code used as a route handler, middleware or mount target.
struct RefFact {
  std::string kind;       // identifier|member|inline|require|other
  std::string name;
  std::string receiver;
  std::string text;
  int symbol = -1;        // inline function symbol
  std::string wrapper;    // e.g. "asyncHandler" when unwrapped from wrapper(fn)
  std::string specifier;  // require('./x') target
  Pos pos;
};

struct RouteFact {
  std::string framework;   // express|nestjs|flask|fastapi|spring
  std::string method;      // GET|POST|...|ALL
  std::string path;        // as declared
  std::string prefix;      // statically known prefix from the same file (class-level mapping)
  std::string registrar;   // receiver ("app", "router", "bp") or class name
  int scope = 0;           // symbol owning the registration statement
  Pos pos;
  bool has_handler = false;
  RefFact handler;
  std::vector<RefFact> middleware;
  std::string confidence;  // recognized|heuristic
  std::string note;
};

struct RouterFact {
  std::string name;       // variable name
  std::string kind;       // app|router|blueprint
  std::string framework;
  std::string prefix;     // constructor-declared prefix (Blueprint url_prefix, APIRouter prefix)
  int scope = 0;
  uint32_t line = 0;
};

// app.use([path,] ...refs), register_blueprint(bp, url_prefix=...),
// include_router(r, prefix=...). Each reference is either a router mount or
// a middleware registration; the resolver decides which.
struct MountFact {
  std::string framework;
  std::string registrar;
  std::string prefix;
  bool has_prefix = false;
  int scope = 0;
  Pos pos;
  RefFact target;
};

struct ExceptionFact {
  std::string kind;   // throw|rethrow|catch|declares
  std::string type;   // type name when identifiable, "*" for catch-all handlers
  std::string text;   // bounded expression text
  int scope = 0;
  Pos pos;
  int handler = -1;           // index of an enclosing local catch fact that may handle a throw
  std::string handler_match;  // exact|catch_all|supertype_name
};

struct EntryFact {
  std::string kind;    // main_function|main_guard|server_start|framework_app|module_main
  std::string reason;
  int scope = 0;
  uint32_t line = 0;
};

struct FileFacts {
  std::string module_name;  // Java package, Python module path
  std::vector<SymbolFact> symbols;
  std::vector<ImportFact> imports;
  std::vector<ExportFact> exports;
  std::vector<CallFact> calls;
  std::vector<TypeHintFact> type_hints;
  std::vector<InheritFact> inherits;
  std::vector<RouteFact> routes;
  std::vector<RouterFact> routers;
  std::vector<MountFact> mounts;
  std::vector<ExceptionFact> exceptions;
  std::vector<EntryFact> entries;
  std::vector<std::string> notes;  // unsupported constructs worth reporting
  bool depth_limited = false;
};

}  // namespace proj
