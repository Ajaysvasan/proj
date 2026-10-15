#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/json.hpp"
#include "db/sqlite.hpp"

namespace proj {

// A method seen at runtime (runtime store row).
struct RtMethod {
  int64_t id = -1;
  std::string cls;   // binary name: com.acme.Foo$Bar
  std::string name;  // <init>, <clinit>, lambda$doGet$0, ...
  std::string desc;  // JVM descriptor
  std::string source;
  int64_t first_line = 0;
  int64_t last_line = 0;
  std::string display() const;  // Foo$Bar.name(String, int)
};

// Where a runtime method sits in the static index.
//   exact      the symbol of that name (overloads told apart by line, then parameters)
//   lambda     a lambda body: its enclosing method, which the static model credits with its calls
//   implicit   a constructor the source does not declare: its class
//   enclosing  no symbol of that name: the method containing its first line
//   ambiguous  several symbols remain; the first is used
//   unresolved the source file or method is not in the index (another module, generated code)
struct RtMapping {
  int64_t symbol = -1;
  std::string status = "unresolved";
};

// How the static graph accounts for an observed call caller -> callee:
//   STATIC_CONFIRMED  a resolved static edge
//   STATIC_INFERRED   a heuristic edge, a candidate of an ambiguous edge (via_edge), or dynamic
//                     dispatch through an overridden method the code calls (via_symbol)
//   RUNTIME_OBSERVED  no static edge: only the runtime shows this call
struct Evidence {
  std::string kind;
  int64_t via_edge = -1;
  int64_t via_symbol = -1;
  std::string note;
};

// Maps runtime identities to static symbols, with per-query caches. Never invents a location: a
// method that cannot be placed stays unresolved with its runtime identity.
class RuntimeMap {
 public:
  RuntimeMap(db::Database& index, db::Database& store) : index_(index), store_(store) {}

  const RtMethod* method(int64_t rt_id);
  RtMapping map(int64_t rt_id);
  // A stack frame (no descriptor): by source file and line.
  RtMapping map_frame(const std::string& cls, const std::string& method, const std::string& file, int64_t line);
  // Runtime methods that map to this symbol (including lambdas inside it).
  std::vector<int64_t> methods_of_symbol(int64_t symbol_id);
  Evidence evidence(int64_t caller, int64_t callee);
  // Calls observed out of (forward) or into a symbol: mapped neighbors and unmapped runtime methods.
  void observed(int64_t symbol, bool forward, std::map<int64_t, int64_t>& mapped,
                std::map<std::string, int64_t>& unmapped);

  static std::vector<std::string> descriptor_params(const std::string& desc);

 private:
  struct Sym {
    int64_t id;
    std::string name;
    std::string kind;
    int64_t start;
    int64_t end;
    int64_t params;
    std::string param_types;
  };
  db::Database& index_;
  db::Database& store_;
  std::unordered_map<int64_t, RtMethod> methods_;
  std::unordered_map<int64_t, RtMapping> mappings_;
  std::map<std::string, int64_t> files_;  // "pkg/dir|File.java" -> file id (-1: not indexed)
  std::unordered_map<int64_t, std::vector<Sym>> symbols_;

  int64_t file_for(const std::string& cls, const std::string& source);
  const std::vector<Sym>& file_symbols(int64_t file_id);
  RtMapping place(int64_t file, const std::string& cls, const std::string& name, const std::string& desc, int64_t line);
};

}  // namespace proj
