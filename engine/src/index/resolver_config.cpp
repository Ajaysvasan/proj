// Resolution of references by name: handler classes and functions named in
// configuration files (web.xml, Struts, Spring XML, OpenAPI, serverless.yml,
// custom route files) and in class-based route registrations, plus the
// framework-specific dispatch from a handler class to the methods serving each
// HTTP method (doGet/doPost, execute, handleRequest, get/post, handle, ...).
#include <algorithm>

#include "index/resolver.hpp"

namespace proj {
namespace {

constexpr size_t kMaxCandidates = 8;

bool ends_with(std::string_view s, std::string_view p) {
  return s.size() >= p.size() && s.substr(s.size() - p.size()) == p;
}

bool is_class_kind(const std::string& k) {
  return k == "class" || k == "interface" || k == "struct" || k == "enum" || k == "union";
}

struct Dispatch {
  std::vector<std::pair<std::string, std::string>> verbs;  // (method name, HTTP method)
  std::vector<std::string> catch_all;                      // serves every HTTP method
};

const std::vector<std::string> kHttpVerbs = {"GET", "POST", "PUT", "DELETE", "PATCH", "HEAD", "OPTIONS", "TRACE"};

std::string capitalized(std::string s) {
  for (size_t i = 0; i < s.size(); ++i) {
    s[i] = static_cast<char>(i == 0 ? std::toupper(static_cast<unsigned char>(s[i])) : std::tolower(static_cast<unsigned char>(s[i])));
  }
  return s;
}

std::string lowered(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

Dispatch verbs_with(const std::vector<std::string>& spellings, std::vector<std::string> catch_all) {
  // spellings use "{Verb}", "{verb}" or "{VERB}" placeholders
  Dispatch d;
  for (const auto& verb : kHttpVerbs) {
    for (const auto& s : spellings) {
      std::string name = s;
      auto put = [&](const char* ph, const std::string& v) {
        auto p = name.find(ph);
        if (p != std::string::npos) name.replace(p, std::string(ph).size(), v);
      };
      put("{Verb}", capitalized(verb));
      put("{verb}", lowered(verb));
      put("{VERB}", verb);
      d.verbs.emplace_back(name, verb);
    }
  }
  d.catch_all = std::move(catch_all);
  return d;
}

const Dispatch& dispatch_for(const std::string& framework) {
  static const Dispatch servlet = verbs_with({"do{Verb}"}, {"service"});
  static const Dispatch httpserver = verbs_with({}, {"handle"});
  static const Dispatch action = verbs_with({}, {"execute"});
  static const Dispatch spring = verbs_with({}, {"handleRequest", "handleRequestInternal"});
  static const Dispatch python = verbs_with({"{verb}"}, {"dispatch", "__call__"});
  static const Dispatch falcon = verbs_with({"on_{verb}"}, {});
  static const Dispatch webpy = verbs_with({"{VERB}"}, {});
  static const Dispatch http_server = verbs_with({"do_{VERB}"}, {});
  static const Dispatch generic = verbs_with(
      {"do{Verb}", "{verb}", "on{Verb}", "handle{Verb}", "on_{verb}", "handle_{verb}", "do_{VERB}", "{VERB}"},
      {"handle", "handleRequest", "handle_request", "service", "execute", "process", "processRequest", "onRequest",
       "on_request", "invoke", "dispatch", "operator()", "__call__", "call", "run", "apply"});
  if (framework == "servlet") return servlet;
  if (framework == "httpserver") return httpserver;
  if (framework == "struts" || framework == "struts2") return action;
  if (framework == "spring_xml") return spring;
  if (framework == "tornado" || framework == "webapp2" || framework == "django" || framework == "pyramid" ||
      framework == "flask" || framework == "bottle") {
    return python;
  }
  if (framework == "falcon") return falcon;
  if (framework == "webpy") return webpy;
  if (framework == "http_server") return http_server;
  return generic;
}

std::string describe(const Dispatch& d) {
  std::vector<std::string> names;
  for (const auto& [name, verb] : d.verbs) {
    if (verb == "GET" || verb == "POST") names.push_back(name);
  }
  for (const auto& c : d.catch_all) names.push_back(c);
  std::string out;
  for (size_t i = 0; i < names.size() && i < 6; ++i) out += (i ? ", " : "") + names[i];
  if (names.size() > 6) out += ", ...";
  return out;
}

}  // namespace

Resolver::Outcome Resolver::config_class(const std::string& raw) {
  Outcome o;
  o.status = "unresolved";
  std::string ref = raw;
  if (ref.rfind("::", 0) == 0) ref = ref.substr(2);
  bool cpp = ref.find("::") != std::string::npos;
  std::replace(ref.begin(), ref.end(), '$', '.');
  if (!cpp) std::replace(ref.begin(), ref.end(), ':', '.');
  const std::string sep = cpp ? "::" : ".";
  auto pos = ref.rfind(sep);
  std::string simple = pos == std::string::npos ? ref : ref.substr(pos + sep.size());
  if (simple.empty()) return o;
  std::vector<int64_t> exact, suffix, by_name;
  auto& st = cached(
      "SELECT s.id, s.qualified_name FROM symbols s JOIN files f ON f.id = s.file_id WHERE s.name = ? "
      "AND s.kind IN ('class','interface','struct','enum','union') ORDER BY f.path, s.start_line, s.start_col LIMIT 64");
  st.bind(1, simple);
  while (st.step()) {
    int64_t id = st.int64(0);
    std::string qn = st.text(1);
    if (qn == ref) exact.push_back(id);
    else if (pos != std::string::npos && ends_with(qn, sep + ref)) suffix.push_back(id);  // src.app.X for app.X
    else by_name.push_back(id);
  }
  st.reset();
  auto take = [&](const std::vector<int64_t>& ids, const std::string& note) {
    if (ids.empty()) return false;
    if (ids.size() == 1) {
      o.status = "resolved";
      o.targets = ids;
      o.note = note;
    } else {
      o.status = "ambiguous";
      for (size_t i = 0; i < ids.size() && i < kMaxCandidates; ++i) o.targets.push_back(ids[i]);
      o.note = std::to_string(ids.size()) + " classes match " + raw;
    }
    return true;
  };
  if (take(exact, "")) return o;
  if (take(suffix, "matched by the end of its qualified name")) return o;
  if (pos == std::string::npos) {
    if (take(by_name, "matched by class name")) return o;
    o.note = "no class named " + raw + " in the project";
    return o;
  }
  std::string owner = ref.substr(0, pos);
  if (!cpp) {
    // A package or module that is not part of the project: an external class.
    auto& pkg = cached("SELECT 1 FROM files WHERE module_name = ?1 OR module_name LIKE '%.' || ?1 LIMIT 1");
    pkg.bind(1, owner);
    bool project = pkg.step();
    pkg.reset();
    if (!project) {
      o.status = "external";
      o.note = raw + " is not part of the project";
      return o;
    }
  }
  o.note = "class " + raw + " not found in the project";
  if (!by_name.empty()) o.note += " (" + std::to_string(by_name.size()) + " class(es) named " + simple + " elsewhere)";
  return o;
}

Resolver::Outcome Resolver::config_member(const std::string& qualified, const std::string& member) {
  Outcome o;
  o.status = "unresolved";
  bool cpp = qualified.find("::") != std::string::npos;
  const std::string sep = cpp ? "::" : ".";
  std::vector<SymbolInfo> cands;
  {
    std::vector<int64_t> ids;
    auto& st = cached(
        "SELECT s.id, s.qualified_name FROM symbols s JOIN files f ON f.id = s.file_id WHERE s.name = ? "
        "AND s.kind IN ('function','method','constructor') ORDER BY f.path, s.start_line, s.start_col LIMIT 64");
    st.bind(1, member);
    while (st.step()) {
      std::string qn = st.text(1);
      if (qn == qualified || ends_with(qn, sep + qualified)) ids.push_back(st.int64(0));
    }
    st.reset();
    for (auto id : ids) {
      if (auto s = symbol(id)) cands.push_back(*s);
    }
  }
  if (!cands.empty()) return pick(cands, -1, "");
  auto pos = qualified.rfind(sep);
  if (pos == std::string::npos) {
    o.note = "no function named " + qualified + " in the project";
    return o;
  }
  std::string owner = qualified.substr(0, pos);
  Outcome c = config_class(owner);
  if (c.status == "resolved") {
    Outcome m = method_lookup(c.targets[0], member, -1);
    if (m.status == "unresolved" && m.note.empty()) m.note = "no method " + member + " in " + owner;
    return m;
  }
  if (c.status == "external" || c.status == "ambiguous") {
    c.targets.clear();
    if (c.status == "ambiguous") c.status = "unresolved";
    return c;
  }
  o.note = "no function or method " + qualified + " in the project";
  return o;
}

Resolver::Outcome Resolver::config_function_name(const std::string& name) {
  Outcome o;
  o.status = "unresolved";
  std::vector<SymbolInfo> defs, decls;
  {
    std::vector<int64_t> ids;
    auto& st = cached(
        "SELECT s.id FROM symbols s JOIN files f ON f.id = s.file_id WHERE s.name = ? AND s.kind IN ('function','method') "
        "ORDER BY f.path, s.start_line, s.start_col LIMIT 32");
    st.bind(1, name);
    while (st.step()) ids.push_back(st.int64(0));
    st.reset();
    for (auto id : ids) {
      auto s = symbol(id);
      if (!s) continue;
      auto parent = symbol(s->parent_id);
      bool interface_method = parent && parent->kind == "interface";
      (s->is_definition && !interface_method ? defs : decls).push_back(*s);
    }
  }
  const auto& use = defs.empty() ? decls : defs;
  if (use.empty()) {
    o.note = "no function or method named " + name + " in the project";
    return o;
  }
  if (use.size() == 1) {
    o.status = "heuristic";
    o.targets = {use[0].id};
    o.note = "matched by function name only";
    return o;
  }
  o.status = "ambiguous";
  std::string names;
  for (size_t i = 0; i < use.size() && i < 3; ++i) names += (i ? ", " : "") + use[i].qualified_name;
  o.note = std::to_string(use.size()) + " definitions named " + name + ": " + names + (use.size() > 3 ? ", ..." : "");
  return o;
}

Resolver::Outcome Resolver::config_file(const std::string& path, const std::string& member) {
  Outcome o;
  o.status = "unresolved";
  std::string rel = path;
  while (rel.rfind("./", 0) == 0) rel = rel.substr(2);
  while (!rel.empty() && rel[0] == '/') rel = rel.substr(1);
  int64_t file = -1;
  std::string lang, found;
  auto& st = cached(
      "SELECT id, IFNULL(language, ''), path FROM files WHERE path = ?1 OR (length(path) > length(?1) AND "
      "substr(path, length(path) - length(?1)) = '/' || ?1) ORDER BY length(path), path LIMIT 1");
  for (const char* ext : {"", ".py", ".js", ".ts", ".java", ".cpp", ".cc", ".c", ".h", ".hpp", ".mjs", ".cjs"}) {
    st.bind(1, rel + ext);
    if (st.step()) {
      file = st.int64(0);
      lang = st.text(1);
      found = st.text(2);
    }
    st.reset();
    if (file >= 0) break;
  }
  if (file < 0) {
    o.note = "no file " + rel + " in the project";
    return o;
  }
  if (lang.empty()) {
    o.note = found + " is not analyzed (no parser for this file type)";
    return o;
  }
  int64_t module = module_symbol(file);
  std::string stem = found.substr(found.rfind('/') == std::string::npos ? 0 : found.rfind('/') + 1);
  stem = stem.substr(0, stem.find('.'));
  int64_t cls = -1;
  for (const auto& c : children_named(module, stem)) {
    if (is_class_kind(c.kind)) cls = c.id;
  }
  if (cls < 0) {
    // Java: the class lives inside the package, not directly under the module symbol.
    auto& q = cached("SELECT id FROM symbols WHERE file_id = ? AND name = ? AND kind IN ('class','interface','struct') ORDER BY start_line LIMIT 1");
    q.bind(1, file).bind(2, stem);
    if (q.step()) cls = q.int64(0);
    q.reset();
  }
  if (!member.empty()) {
    std::vector<SymbolInfo> cands;
    for (const auto& c : children_named(module, member)) {
      if (c.kind == "function" || c.kind == "method") cands.push_back(c);
    }
    if (!cands.empty()) return pick(cands, -1, "");
    if (cls >= 0) return method_lookup(cls, member, -1);
    o.note = "no function " + member + " in " + found;
    return o;
  }
  o.status = "resolved";
  o.targets = {cls >= 0 ? cls : module};
  o.note = cls >= 0 ? "" : "module " + found;
  return o;
}

Resolver::Outcome Resolver::config_bean(const std::string& id) {
  Outcome o;
  o.status = "unresolved";
  auto& st = cached(
      "SELECT s.id FROM symbols s JOIN files f ON f.id = s.file_id WHERE s.kind = 'bean' AND s.name = ? "
      "ORDER BY f.path, s.start_line LIMIT 8");
  st.bind(1, id);
  while (st.step()) o.targets.push_back(st.int64(0));
  st.reset();
  if (o.targets.empty()) {
    o.note = "bean '" + id + "' is not declared in the project's XML";
  } else {
    o.status = o.targets.size() == 1 ? "resolved" : "ambiguous";
    if (o.targets.size() > 1) o.note = std::to_string(o.targets.size()) + " beans named " + id;
  }
  return o;
}

Resolver::Outcome Resolver::resolve_call_config(const std::string& name, const std::string& receiver,
                                                const std::string& receiver_kind) {
  if (receiver_kind == "config_bean") return config_bean(receiver);
  Outcome o;
  o.status = "unresolved";
  if (receiver_kind != "config_class") {
    o.note = "unsupported configuration reference";
    return o;
  }
  Outcome c = config_class(receiver);
  if (c.status != "resolved") return c;
  auto info = symbol(c.targets[0]);
  if (info && info->name == name) return c;  // the class itself
  Outcome m = method_lookup(c.targets[0], name, -1);
  if (m.status == "unresolved" && m.note.empty()) m.note = "no method " + name + " in " + receiver;
  return m;
}

Resolver::Outcome Resolver::resolve_class_ref(int64_t file, int64_t scope, const std::string& name) {
  std::string fam = family(language_of_file(file));
  if (fam == "java") {
    std::string note;
    int64_t id = java_type(file, name, &note);
    if (id >= 0) {
      Outcome o;
      o.status = "resolved";
      o.targets = {id};
      return o;
    }
    if (!note.empty()) {
      Outcome o;
      o.status = "external";
      o.note = note;
      return o;
    }
  } else if (fam == "python" || fam == "js") {
    Value v = resolve_name(file, scope, name);
    if (v.kind == Value::Kind::Symbol) {
      if (auto s = symbol(v.id); s && is_class_kind(s->kind)) {
        Outcome o;
        o.status = "resolved";
        o.targets = {v.id};
        return o;
      }
    } else if (v.kind == Value::Kind::External) {
      Outcome o;
      o.status = "external";
      o.note = v.note;
      return o;
    }
  } else if (fam == "c") {
    int64_t id = cpp_type(scope, name);
    if (id >= 0) {
      Outcome o;
      o.status = "resolved";
      o.targets = {id};
      return o;
    }
  }
  return config_class(name);
}

std::vector<Resolver::DispatchTarget> Resolver::dispatch_targets(int64_t cls, const std::string& framework,
                                                                 const std::string& route_method,
                                                                 const std::string& explicit_method, std::string* note) {
  std::vector<DispatchTarget> out;
  auto info = symbol(cls);
  std::string cname = info ? info->name : "the class";
  auto find = [&](const std::string& name) -> int64_t {
    Outcome o = method_lookup(cls, name, -1);
    if ((o.status == "resolved" || o.status == "ambiguous") && !o.targets.empty()) return o.targets[0];
    return -1;
  };
  if (!explicit_method.empty()) {
    if (int64_t m = find(explicit_method); m >= 0) {
      out.push_back({route_method, m, explicit_method + " of " + cname});
    } else if (note) {
      *note = "method " + explicit_method + " not found in " + cname + " or its project base classes";
    }
    return out;
  }
  const Dispatch& d = dispatch_for(framework);
  bool any = route_method == "ANY" || route_method == "ALL";
  for (const auto& [name, verb] : d.verbs) {
    if (!any && verb != route_method) continue;
    if (std::any_of(out.begin(), out.end(), [&](const DispatchTarget& t) { return t.method == verb; })) continue;
    if (int64_t m = find(name); m >= 0) out.push_back({verb, m, name + " of " + cname + " handles " + verb});
  }
  if (!out.empty()) return out;
  for (const auto& name : d.catch_all) {
    if (int64_t m = find(name); m >= 0) {
      bool any_method = route_method == "ANY" || route_method == "ALL";
      out.push_back({route_method, m, name + " of " + cname + (any_method ? " handles every HTTP method" : " handles " + route_method)});
      return out;
    }
  }
  if (note) *note = "no request-handling method (" + describe(d) + ") in " + cname + " or its project base classes";
  return out;
}

}  // namespace proj
