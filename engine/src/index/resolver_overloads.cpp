// Overload selection by argument types (Java, C/C++).
//
// Extractors record each method's declared parameter types and, at call
// sites, the argument types that are visible without a type checker: literals,
// `new X()`, casts, and variables or fields with a declared type. When several
// overloads have the right number of parameters, each candidate is scored per
// argument (exact, widening/boxing/subtype, unknown, incompatible). Candidates
// with an incompatible argument are dropped; the best total wins when it is
// unique. Otherwise the call stays ambiguous among the remaining candidates.
#include <algorithm>
#include <map>
#include <set>

#include "index/resolver.hpp"

namespace proj {
namespace {

std::vector<std::string> split_types(const std::string& s) {
  std::vector<std::string> out;
  if (s.empty()) return out;
  std::string cur;
  int depth = 0;
  for (char c : s) {
    if (c == '<' || c == '(') ++depth;
    if (c == '>' || c == ')') --depth;
    if (c == ',' && depth == 0) {
      out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  out.push_back(cur);
  return out;
}

bool ends_with(std::string_view s, std::string_view p) {
  return s.size() >= p.size() && s.substr(s.size() - p.size()) == p;
}

// ---- Java -------------------------------------------------------------------------

const std::set<std::string> kJavaPrimitives = {"byte", "short", "char", "int", "long", "float", "double", "boolean"};

const std::map<std::string, std::string> kBoxed = {{"int", "Integer"},  {"long", "Long"},     {"double", "Double"},
                                                   {"float", "Float"},  {"boolean", "Boolean"}, {"char", "Character"},
                                                   {"byte", "Byte"},    {"short", "Short"}};

const std::map<std::string, std::vector<std::string>> kWidening = {
    {"byte", {"short", "int", "long", "float", "double"}}, {"short", {"int", "long", "float", "double"}},
    {"char", {"int", "long", "float", "double"}},          {"int", {"long", "float", "double"}},
    {"long", {"float", "double"}},                         {"float", {"double"}}};

// Well-known JDK subtype relations (simple names).
const std::map<std::string, std::vector<std::string>> kJdkSupertypes = {
    {"String", {"CharSequence", "Comparable", "Serializable"}},
    {"StringBuilder", {"CharSequence"}},
    {"StringBuffer", {"CharSequence"}},
    {"Integer", {"Number", "Comparable", "Serializable"}},
    {"Long", {"Number", "Comparable", "Serializable"}},
    {"Double", {"Number", "Comparable", "Serializable"}},
    {"Float", {"Number", "Comparable", "Serializable"}},
    {"Short", {"Number", "Comparable", "Serializable"}},
    {"Byte", {"Number", "Comparable", "Serializable"}},
    {"BigDecimal", {"Number", "Comparable", "Serializable"}},
    {"BigInteger", {"Number", "Comparable", "Serializable"}},
    {"Boolean", {"Comparable", "Serializable"}},
    {"Character", {"Comparable", "Serializable"}},
    {"ArrayList", {"List", "Collection", "Iterable", "AbstractList", "RandomAccess", "Serializable"}},
    {"LinkedList", {"List", "Deque", "Queue", "Collection", "Iterable", "Serializable"}},
    {"List", {"Collection", "Iterable"}},
    {"Set", {"Collection", "Iterable"}},
    {"Queue", {"Collection", "Iterable"}},
    {"Deque", {"Queue", "Collection", "Iterable"}},
    {"Collection", {"Iterable"}},
    {"HashSet", {"Set", "Collection", "Iterable", "Serializable"}},
    {"LinkedHashSet", {"HashSet", "Set", "Collection", "Iterable", "Serializable"}},
    {"TreeSet", {"Set", "SortedSet", "NavigableSet", "Collection", "Iterable", "Serializable"}},
    {"HashMap", {"Map", "AbstractMap", "Serializable"}},
    {"LinkedHashMap", {"HashMap", "Map", "AbstractMap", "Serializable"}},
    {"TreeMap", {"Map", "SortedMap", "NavigableMap", "AbstractMap", "Serializable"}},
    {"ConcurrentHashMap", {"Map", "ConcurrentMap", "AbstractMap", "Serializable"}},
    {"Date", {"Comparable", "Serializable"}},
    {"RuntimeException", {"Exception", "Throwable", "Serializable"}},
    {"Exception", {"Throwable", "Serializable"}}};

bool java_known(const std::string& t) {
  if (kJavaPrimitives.count(t) || t == "Object" || t == "null") return true;
  for (const auto& [p, b] : kBoxed) {
    if (b == t) return true;
  }
  if (kJdkSupertypes.count(t)) return true;
  for (const auto& [k, supers] : kJdkSupertypes) {
    if (std::find(supers.begin(), supers.end(), t) != supers.end()) return true;
  }
  return false;
}

bool is_type_variable(const std::string& t) {
  // T, E, K, V, T2: single upper-case letter, optionally followed by digits
  return !t.empty() && t.size() <= 3 && std::isupper(static_cast<unsigned char>(t[0])) &&
         std::all_of(t.begin() + 1, t.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
}

// ---- C++ --------------------------------------------------------------------------

const std::set<std::string> kCppArithmetic = {
    "bool", "char", "signedchar", "unsignedchar", "wchar_t", "char8_t", "char16_t", "char32_t", "short",
    "unsignedshort", "int", "unsigned", "unsignedint", "long", "unsignedlong", "longlong", "unsignedlonglong",
    "float", "double", "longdouble", "size_t", "ssize_t", "ptrdiff_t", "int8_t", "int16_t", "int32_t", "int64_t",
    "uint8_t", "uint16_t", "uint32_t", "uint64_t", "intptr_t", "uintptr_t"};

// "const std::string&" -> "string", "const char *" -> "char*", "std::vector<int>" -> "vector"
std::string cpp_norm(const std::string& raw) {
  std::string flat;
  int depth = 0;
  for (char c : raw) {
    if (c == '<') ++depth;
    else if (c == '>') --depth;
    else if (depth == 0) flat += c;
  }
  std::string out;
  size_t i = 0;
  int stars = 0;
  while (i < flat.size()) {
    char c = flat[i];
    if (c == '*') {
      ++stars;
      ++i;
      continue;
    }
    if (c == '&' || std::isspace(static_cast<unsigned char>(c))) {
      ++i;
      continue;
    }
    size_t start = i;
    while (i < flat.size() && flat[i] != '*' && flat[i] != '&' && !std::isspace(static_cast<unsigned char>(flat[i]))) ++i;
    std::string tok = flat.substr(start, i - start);
    if (tok == "const" || tok == "volatile" || tok == "struct" || tok == "class" || tok == "typename" ||
        tok == "enum" || tok == "mutable" || tok == "register") {
      continue;
    }
    out += tok;  // "unsigned long" -> "unsignedlong"
  }
  auto colon = out.rfind("::");
  if (colon != std::string::npos) out = out.substr(colon + 2);
  return out + std::string(static_cast<size_t>(stars), '*');
}

int arith_rank(const std::string& t) {
  if (t == "bool" || t == "char" || t == "signedchar" || t == "unsignedchar" || t == "short" || t == "unsignedshort" ||
      t == "int8_t" || t == "int16_t" || t == "uint8_t" || t == "uint16_t") {
    return 0;  // promoted to int
  }
  if (t == "float") return 2;
  if (t == "double" || t == "longdouble") return 3;
  return 1;
}

}  // namespace

int64_t Resolver::type_class(const std::string& type, bool cpp) {
  // Java resolves types through the file's imports; C++ through the scope.
  std::string key = (cpp ? std::to_string(call_ctx_.scope) : std::to_string(file_id_)) + "|" + type;
  if (auto it = type_class_cache_.find(key); it != type_class_cache_.end()) return it->second;
  int64_t cls = cpp ? cpp_type(call_ctx_.scope, type) : java_type(file_id_, type, nullptr);
  if (type_class_cache_.size() > 20000) type_class_cache_.clear();
  type_class_cache_.emplace(key, cls);
  return cls;
}

std::vector<std::string> Resolver::ancestor_names(const std::string& type, bool cpp) {
  std::vector<std::string> out;
  int64_t cls = type_class(type, cpp);
  if (cls < 0) return out;
  if (auto it = ancestors_cache_.find(cls); it != ancestors_cache_.end()) return it->second;
  std::vector<int64_t> frontier{cls};
  std::set<int64_t> seen{cls};
  for (int depth = 0; depth < 6 && !frontier.empty(); ++depth) {
    std::vector<int64_t> next;
    for (int64_t c : frontier) {
      auto& st = cached(
          "SELECT IFNULL(dst_symbol_id, -1), IFNULL(target_name, '') FROM edges WHERE src_symbol_id = ? "
          "AND kind IN ('extends','implements') ORDER BY id LIMIT 16");
      st.bind(1, c);
      std::vector<std::pair<int64_t, std::string>> bases;
      while (st.step()) bases.emplace_back(st.int64(0), st.text(1));
      st.reset();
      for (const auto& [dst, name] : bases) {
        if (!name.empty()) out.push_back(name);
        if (dst >= 0 && seen.insert(dst).second) next.push_back(dst);
      }
    }
    frontier = std::move(next);
  }
  if (ancestors_cache_.size() > 20000) ancestors_cache_.clear();
  ancestors_cache_.emplace(cls, out);
  return out;
}

std::string Resolver::arg_static_type(const std::string& token, bool cpp) {
  if (token.empty() || token == "?") return "";
  if (token[0] != '$') return cpp ? cpp_norm(token) : token;
  std::string name = token.substr(1);
  std::optional<std::string> type;
  if (name == "this") {
    int64_t cls = enclosing_class(call_ctx_.scope);
    auto s = symbol(cls);
    if (!s) return "";
    return cpp ? s->name + "*" : s->name;
  }
  if (name.rfind("this.", 0) == 0) {
    int64_t cls = enclosing_class(call_ctx_.scope);
    if (cls < 0) return "";
    q_hint_.bind(1, cls).bind(2, name.substr(5));
    if (q_hint_.step()) type = q_hint_.text(0);
    q_hint_.reset();
  } else {
    type = hint_in_scope_chain(call_ctx_.scope, name);
  }
  if (!type || type->empty() || *type == "?") return "";
  if (type->rfind("@call:", 0) == 0) {
    int64_t cls = call_result_type(file_id_, call_ctx_.scope, type->substr(6), nullptr);
    auto s = symbol(cls);
    return s ? s->name : "";
  }
  if (cpp) return cpp_norm(*type);
  // Java: drop type arguments and the package, keep array dimensions.
  std::string t;
  int depth = 0;
  for (char c : *type) {
    if (c == '<') ++depth;
    else if (c == '>') --depth;
    else if (depth == 0 && !std::isspace(static_cast<unsigned char>(c))) t += c;
  }
  if (t == "var") return "";
  std::string dims;
  while (ends_with(t, "[]")) {
    dims += "[]";
    t.resize(t.size() - 2);
  }
  auto dot = t.rfind('.');
  if (dot != std::string::npos) t = t.substr(dot + 1);
  return t + dims;
}

// 3 exact, 2 widening/boxing/subtype, 1 possible (unknown relation), 0 incompatible.
int Resolver::type_match(const std::string& arg, const std::string& param, bool cpp) {
  if (arg.empty() || param.empty() || param == "?") return 1;
  if (arg == param) return 3;
  if (!cpp) {
    if (arg == "null") return kJavaPrimitives.count(param) ? 0 : 2;
    if (auto w = kWidening.find(arg); w != kWidening.end()) {
      if (std::find(w->second.begin(), w->second.end(), param) != w->second.end()) return 2;
    }
    if (auto b = kBoxed.find(arg); b != kBoxed.end() && b->second == param) return 2;
    for (const auto& [prim, box] : kBoxed) {
      if (box == arg && prim == param) return 2;  // unboxing
    }
    if (param == "Object") return 1;
    if (is_type_variable(param)) return 1;
    if (auto s = kJdkSupertypes.find(arg); s != kJdkSupertypes.end()) {
      if (std::find(s->second.begin(), s->second.end(), param) != s->second.end()) return 2;
    }
    if (kBoxed.count(arg)) {
      // a primitive passed where a boxed supertype is expected: int -> Number
      auto s = kJdkSupertypes.find(kBoxed.at(arg));
      if (s != kJdkSupertypes.end() && std::find(s->second.begin(), s->second.end(), param) != s->second.end()) return 2;
    }
    if (ends_with(arg, "[]") != ends_with(param, "[]")) return 0;
    auto ancestors = ancestor_names(arg, false);
    if (std::find(ancestors.begin(), ancestors.end(), param) != ancestors.end()) return 2;
    bool arg_project = type_class(arg, false) >= 0;
    bool param_project = type_class(param, false) >= 0;
    if (java_known(arg) && java_known(param)) return 0;
    if (java_known(arg) && param_project) return 0;
    if (arg_project && (param_project || java_known(param))) return 0;
    return 1;
  }
  // C++
  bool arg_ptr = ends_with(arg, "*"), param_ptr = ends_with(param, "*");
  if (arg == "nullptr") return param_ptr ? 2 : 0;
  if (kCppArithmetic.count(arg) && kCppArithmetic.count(param)) {
    // promotion beats conversion
    int ra = arith_rank(arg), rp = arith_rank(param);
    if ((ra == 0 && param == "int") || (arg == "float" && param == "double")) return 2;
    (void)rp;
    return 1;
  }
  if (arg == "char*") {
    if (param == "string" || param == "string_view" || param == "String" || param == "QString") return 2;
    return param_ptr ? 0 : (kCppArithmetic.count(param) ? 0 : 1);
  }
  if (kCppArithmetic.count(arg) != kCppArithmetic.count(param) && (kCppArithmetic.count(arg) || kCppArithmetic.count(param))) {
    return arg_ptr || param_ptr ? 0 : (kCppArithmetic.count(arg) ? 0 : 1);
  }
  if (arg_ptr != param_ptr) return 0;
  std::string base_arg = arg_ptr ? arg.substr(0, arg.size() - 1) : arg;
  std::string base_param = param_ptr ? param.substr(0, param.size() - 1) : param;
  if (base_arg == base_param) return 3;
  auto ancestors = ancestor_names(base_arg, true);
  for (const auto& a : ancestors) {
    if (cpp_norm(a) == base_param) return 2;
  }
  return 1;
}

std::vector<Resolver::SymbolInfo> Resolver::narrow_by_types(const std::vector<SymbolInfo>& cands, std::string* note) {
  bool cpp = family(file_lang_) == "c";
  std::vector<std::string> args;
  bool any = false;
  for (const auto& tok : call_ctx_.arg_types) {
    args.push_back(arg_static_type(tok, cpp));
    if (!args.back().empty()) any = true;
  }
  if (!any) return cands;
  struct Scored {
    const SymbolInfo* sym;
    int total;
  };
  std::vector<Scored> viable;
  for (const auto& c : cands) {
    auto params = split_types(c.param_types);
    if (params.empty()) {
      viable.push_back({&c, static_cast<int>(args.size())});  // unknown parameter types: neutral
      continue;
    }
    bool variadic = ends_with(params.back(), "...");
    int total = 0;
    bool ok = true;
    for (size_t i = 0; i < args.size() && ok; ++i) {
      std::string p;
      if (i < params.size() - (variadic ? 1 : 0)) {
        p = params[i];
      } else if (variadic) {
        p = params.back().substr(0, params.back().size() - 3);
      } else {
        ok = false;
        break;
      }
      if (cpp) p = cpp_norm(p);
      int s = type_match(args[i], p, cpp);
      if (s == 0) ok = false;
      total += s;
    }
    if (ok) viable.push_back({&c, total - (variadic ? 1 : 0)});
  }
  std::string shown;
  for (size_t i = 0; i < args.size(); ++i) shown += (i ? ", " : "") + (args[i].empty() ? std::string("?") : args[i]);
  if (viable.empty()) {
    if (note) *note = "no overload accepts (" + shown + ")";
    return cands;
  }
  int best = std::max_element(viable.begin(), viable.end(), [](const Scored& a, const Scored& b) { return a.total < b.total; })->total;
  std::vector<SymbolInfo> out;
  for (const auto& v : viable) {
    if (v.total == best) out.push_back(*v.sym);
  }
  if (note) *note = "overload selected by argument types (" + shown + ")";
  return out;
}

}  // namespace proj
