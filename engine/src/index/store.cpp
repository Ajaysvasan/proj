#include "index/store.hpp"

#include <vector>

#include "core/paths.hpp"

namespace proj {

FactWriter::FactWriter(db::Database& db)
    : db_(db),
      file_(db.prepare("INSERT INTO files(path, dir, name, language, size_bytes, mtime_s, fingerprint, status, "
                       "error_count, first_error_line, line_count, module_name, extractor_version, note) "
                       "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)")),
      link_receiver_(db.prepare("UPDATE edges SET receiver_edge_id = ? WHERE id = ?")),
      file_update_(db.prepare("UPDATE files SET language = ?, size_bytes = ?, mtime_s = ?, fingerprint = ?, status = ?, "
                              "error_count = ?, first_error_line = ?, line_count = ?, module_name = ?, "
                              "extractor_version = ?, note = ? WHERE id = ?")),
      symbol_(db.prepare("INSERT INTO symbols(file_id, parent_id, name, qualified_name, kind, language, start_line, "
                         "start_col, end_line, end_col, exported, visibility, is_static, is_definition, param_count, "
                         "container, signature, param_types) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)")),
      symbol_with_id_(db.prepare("INSERT INTO symbols(file_id, parent_id, name, qualified_name, kind, language, "
                                 "start_line, start_col, end_line, end_col, exported, visibility, is_static, "
                                 "is_definition, param_count, container, signature, param_types, id) "
                                 "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)")),
      edge_(db.prepare("INSERT INTO edges(kind, raw_kind, src_symbol_id, file_id, line, col, status, target_name, "
                       "receiver, receiver_kind, target_text, arg_count, arg_types) "
                       "VALUES (?1,?1,?2,?3,?4,?5,'pending',?6,?7,?8,?9,?10,?11)")),
      dep_(db.prepare("INSERT INTO file_deps(src_file_id, scope_symbol_id, kind, specifier, line, col, status, note, raw_note) "
                      "VALUES (?1,?2,?3,?4,?5,?6,'pending',?7,?7)")),
      binding_(db.prepare("INSERT INTO bindings(file_id, dep_id, local_name, imported_name, kind, line, raw_imported, "
                          "raw_kind, raw_dep_id) VALUES (?1,?2,?3,?4,?5,?6,?4,?5,?2)")),
      export_(db.prepare("INSERT INTO exports(file_id, exported_name, local_name, symbol_id, kind, dep_id, line) "
                         "VALUES (?,?,?,?,?,?,?)")),
      hint_(db.prepare("INSERT INTO type_hints(file_id, scope_symbol_id, name, type_name, kind) VALUES (?,?,?,?,?)")),
      router_(db.prepare("INSERT INTO routers(file_id, scope_symbol_id, name, kind, framework, prefix, line) "
                         "VALUES (?,?,?,?,?,?,?)")),
      mount_(db.prepare("INSERT INTO mounts(file_id, scope_symbol_id, framework, registrar, prefix, target_kind, "
                        "target_name, target_receiver, target_text, line, col, target_symbol_id, status, note, raw_note) "
                        "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,'pending',?13,?13)")),
      route_(db.prepare("INSERT INTO routes(framework, method, path, full_path, declared_prefix, mount_status, file_id, "
                        "line, col, registrar, scope_symbol_id, handler_kind, handler_name, handler_receiver, "
                        "handler_text, handler_symbol_id, handler_status, handler_note, confidence, raw_handler_note, "
                        "raw_method) VALUES (?1,?2,?3,?4,?5,'pending',?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?17,?2)")),
      middleware_(db.prepare("INSERT INTO route_middleware(route_id, position, scope, ref_kind, name, receiver, text, "
                             "symbol_id, status, file_id, line) VALUES (?,?,'route',?,?,?,?,?,?,?,?)")),
      exception_(db.prepare("INSERT INTO exceptions(kind, type_name, expression, symbol_id, file_id, line, col, "
                            "handler_id, handler_match) VALUES (?,?,?,?,?,?,?,?,?)")),
      entry_(db.prepare("INSERT INTO entry_points(kind, symbol_id, file_id, line, reason) VALUES (?,?,?,?,?)")) {}

int64_t FactWriter::insert_file(const FileRecord& f) {
  file_.bind(1, f.rel_path).bind(2, paths::parent_dir(f.rel_path)).bind(3, paths::base_name(f.rel_path));
  file_.bind_text_or_null(4, f.language).bind(5, static_cast<int64_t>(f.size)).bind(6, f.mtime_s);
  file_.bind_text_or_null(7, f.fingerprint).bind(8, f.status).bind(9, f.error_count);
  if (f.first_error_line) file_.bind(10, f.first_error_line); else file_.bind(10, nullptr);
  if (f.line_count) file_.bind(11, f.line_count); else file_.bind(11, nullptr);
  file_.bind_text_or_null(12, f.module_name);
  if (f.extractor_version) file_.bind(13, f.extractor_version); else file_.bind(13, nullptr);
  file_.bind_text_or_null(14, f.note);
  file_.run();
  return db_.last_insert_rowid();
}

void FactWriter::update_file(int64_t id, const FileRecord& f) {
  file_update_.bind_text_or_null(1, f.language).bind(2, static_cast<int64_t>(f.size)).bind(3, f.mtime_s);
  file_update_.bind_text_or_null(4, f.fingerprint).bind(5, f.status).bind(6, f.error_count);
  if (f.first_error_line) file_update_.bind(7, f.first_error_line); else file_update_.bind(7, nullptr);
  if (f.line_count) file_update_.bind(8, f.line_count); else file_update_.bind(8, nullptr);
  file_update_.bind_text_or_null(9, f.module_name);
  if (f.extractor_version) file_update_.bind(10, f.extractor_version); else file_update_.bind(10, nullptr);
  file_update_.bind_text_or_null(11, f.note).bind(12, id).run();
}

std::string FactWriter::symbol_key(const std::string& kind, const std::string& qualified_name, int param_count,
                                   int ordinal) {
  return kind + "|" + qualified_name + "|" + std::to_string(param_count < 0 ? -1 : param_count) + "|" +
         std::to_string(ordinal);
}

WriteCounts FactWriter::write_facts(int64_t file_id, const std::string& language, const FileFacts& facts,
                                    const std::unordered_map<std::string, int64_t>* reuse_ids,
                                    std::vector<int64_t>* reused) {
  WriteCounts counts;
  std::vector<int64_t> sym_ids(facts.symbols.size(), -1);
  std::unordered_map<std::string, int> occurrences;
  auto sym = [&](int idx) -> int64_t {
    return idx >= 0 && static_cast<size_t>(idx) < sym_ids.size() ? sym_ids[static_cast<size_t>(idx)] : sym_ids[0];
  };

  for (size_t i = 0; i < facts.symbols.size(); ++i) {
    const auto& s = facts.symbols[i];
    int64_t reuse = -1;
    if (reuse_ids) {
      std::string base = s.kind + "|" + s.qualified_name + "|" + std::to_string(s.param_count < 0 ? -1 : s.param_count);
      int ordinal = occurrences[base]++;
      auto it = reuse_ids->find(symbol_key(s.kind, s.qualified_name, s.param_count, ordinal));
      if (it != reuse_ids->end()) reuse = it->second;
    }
    db::Statement& st = reuse >= 0 ? symbol_with_id_ : symbol_;
    st.bind(1, file_id);
    // Parents always precede children, so the parent id is already known.
    if (s.parent >= 0 && static_cast<size_t>(s.parent) < i) st.bind(2, sym_ids[static_cast<size_t>(s.parent)]);
    else st.bind(2, nullptr);
    st.bind(3, s.name).bind(4, s.qualified_name).bind(5, s.kind).bind(6, language);
    st.bind(7, s.span.start.line).bind(8, s.span.start.col).bind(9, s.span.end.line).bind(10, s.span.end.col);
    st.bind(11, s.exported).bind_text_or_null(12, s.visibility).bind(13, s.is_static);
    st.bind(14, s.is_definition);
    if (s.param_count >= 0) st.bind(15, s.param_count); else st.bind(15, nullptr);
    st.bind_text_or_null(16, s.container).bind_text_or_null(17, s.signature).bind_text_or_null(18, s.param_types);
    if (reuse >= 0) st.bind(19, reuse);
    st.run();
    sym_ids[i] = reuse >= 0 ? reuse : db_.last_insert_rowid();
    if (reuse >= 0 && reused) reused->push_back(reuse);
  }
  counts.symbols = static_cast<int64_t>(facts.symbols.size());
  if (sym_ids.empty()) return counts;

  std::vector<int64_t> call_ids;
  call_ids.reserve(facts.calls.size());
  for (const auto& c : facts.calls) {
    edge_.bind(1, c.kind).bind(2, sym(c.caller)).bind(3, file_id).bind(4, c.pos.line).bind(5, c.pos.col);
    edge_.bind_text_or_null(6, c.name).bind_text_or_null(7, c.receiver).bind_text_or_null(8, c.receiver_kind);
    edge_.bind_text_or_null(9, c.text);
    if (c.arg_count >= 0) edge_.bind(10, c.arg_count); else edge_.bind(10, nullptr);
    edge_.bind_text_or_null(11, c.arg_types);
    edge_.run();
    call_ids.push_back(db_.last_insert_rowid());
  }
  // Chained calls: link each call to the call that produces its receiver.
  for (size_t i = 0; i < facts.calls.size(); ++i) {
    int r = facts.calls[i].receiver_call;
    if (r < 0 || static_cast<size_t>(r) >= call_ids.size()) continue;
    link_receiver_.bind(1, call_ids[static_cast<size_t>(r)]).bind(2, call_ids[i]).run();
  }
  counts.calls = static_cast<int64_t>(facts.calls.size());

  for (const auto& h : facts.inherits) {
    // Inheritance is resolved like a call: the base name is looked up later.
    auto dot = h.base.find_last_of(".:");
    std::string simple = dot == std::string::npos ? h.base : h.base.substr(dot + 1);
    std::string qualifier = dot == std::string::npos ? "" : h.base.substr(0, h.base[dot] == ':' ? dot - 1 : dot);
    edge_.bind(1, h.kind).bind(2, sym(h.symbol)).bind(3, file_id).bind(4, h.pos.line).bind(5, h.pos.col);
    edge_.bind(6, simple).bind_text_or_null(7, qualifier).bind(8, qualifier.empty() ? "none" : "qualified");
    edge_.bind(9, h.base).bind(10, nullptr).bind(11, nullptr);
    edge_.run();
  }

  std::vector<int64_t> dep_ids;
  dep_ids.reserve(facts.imports.size());
  for (const auto& imp : facts.imports) {
    dep_.bind(1, file_id).bind(2, sym(imp.scope)).bind(3, imp.kind).bind(4, imp.specifier);
    dep_.bind(5, imp.pos.line).bind(6, imp.pos.col);
    if (imp.system) dep_.bind(7, "system include"); else dep_.bind(7, nullptr);
    dep_.run();
    int64_t dep_id = db_.last_insert_rowid();
    dep_ids.push_back(dep_id);
    for (const auto& b : imp.bindings) {
      binding_.bind(1, file_id).bind(2, dep_id).bind(3, b.local).bind(4, b.imported).bind(5, b.kind);
      binding_.bind(6, imp.pos.line).run();
    }
  }
  counts.imports = static_cast<int64_t>(facts.imports.size());

  for (const auto& e : facts.exports) {
    export_.bind(1, file_id).bind(2, e.exported).bind_text_or_null(3, e.local);
    if (e.symbol >= 0) export_.bind(4, sym(e.symbol)); else export_.bind(4, nullptr);
    export_.bind(5, e.kind);
    if (e.import_index >= 0 && static_cast<size_t>(e.import_index) < dep_ids.size()) {
      export_.bind(6, dep_ids[static_cast<size_t>(e.import_index)]);
    } else {
      export_.bind(6, nullptr);
    }
    export_.bind(7, e.line).run();
  }

  for (const auto& h : facts.type_hints) {
    hint_.bind(1, file_id).bind(2, sym(h.scope)).bind(3, h.name).bind(4, h.type).bind(5, h.kind).run();
  }

  for (const auto& r : facts.routers) {
    router_.bind(1, file_id).bind(2, sym(r.scope)).bind(3, r.name).bind(4, r.kind).bind(5, r.framework);
    router_.bind_text_or_null(6, r.prefix).bind(7, r.line).run();
  }

  for (const auto& m : facts.mounts) {
    mount_.bind(1, file_id).bind(2, sym(m.scope)).bind(3, m.framework).bind(4, m.registrar);
    if (m.has_prefix) mount_.bind(5, m.prefix); else mount_.bind(5, nullptr);
    mount_.bind(6, m.target.kind).bind_text_or_null(7, m.target.kind == "require" ? m.target.specifier : m.target.name);
    mount_.bind_text_or_null(8, m.target.receiver).bind_text_or_null(9, m.target.text);
    mount_.bind(10, m.pos.line).bind(11, m.pos.col);
    if (m.target.kind == "inline" && m.target.symbol >= 0) mount_.bind(12, sym(m.target.symbol)); else mount_.bind(12, nullptr);
    mount_.bind_text_or_null(13, m.target.wrapper.empty() ? "" : "wrapped by " + m.target.wrapper).run();
  }

  for (const auto& r : facts.routes) {
    route_.bind(1, r.framework).bind(2, r.method).bind(3, r.path).bind(4, r.path).bind_text_or_null(5, r.prefix);
    route_.bind(6, file_id).bind(7, r.pos.line).bind(8, r.pos.col).bind_text_or_null(9, r.registrar);
    route_.bind(10, sym(r.scope));
    const auto& h = r.handler;
    if (r.has_handler) {
      route_.bind(11, h.kind).bind_text_or_null(12, h.kind == "require" ? h.specifier : h.name);
      route_.bind_text_or_null(13, h.receiver).bind_text_or_null(14, h.text);
      if (h.kind == "inline" && h.symbol >= 0) {
        route_.bind(15, sym(h.symbol)).bind(16, "resolved").bind(17, "inline handler");
      } else {
        std::string note = h.wrapper.empty() ? "" : "wrapped by " + h.wrapper;
        if (!r.note.empty()) note = note.empty() ? r.note : note + "; " + r.note;
        route_.bind(15, nullptr).bind(16, "pending").bind_text_or_null(17, note);
      }
    } else {
      route_.bind(11, nullptr).bind(12, nullptr).bind(13, nullptr).bind(14, nullptr).bind(15, nullptr);
      route_.bind(16, "unresolved").bind(17, r.note.empty() ? "no handler argument recognized" : r.note);
    }
    route_.bind(18, r.confidence);
    route_.run();
    int64_t route_id = db_.last_insert_rowid();
    int position = 0;
    for (const auto& m : r.middleware) {
      middleware_.bind(1, route_id).bind(2, position++).bind(3, m.kind);
      middleware_.bind_text_or_null(4, m.kind == "require" ? m.specifier : m.name);
      middleware_.bind_text_or_null(5, m.receiver).bind(6, m.text.empty() ? m.name : m.text);
      if (m.kind == "inline" && m.symbol >= 0) {
        middleware_.bind(7, sym(m.symbol)).bind(8, "resolved");
      } else {
        middleware_.bind(7, nullptr).bind(8, "pending");
      }
      middleware_.bind(9, file_id).bind(10, m.pos.line ? m.pos.line : r.pos.line).run();
    }
  }
  counts.routes = static_cast<int64_t>(facts.routes.size());

  std::vector<int64_t> exc_ids(facts.exceptions.size(), -1);
  for (size_t i = 0; i < facts.exceptions.size(); ++i) {
    const auto& e = facts.exceptions[i];
    exception_.bind(1, e.kind).bind_text_or_null(2, e.type).bind_text_or_null(3, e.text).bind(4, sym(e.scope));
    exception_.bind(5, file_id).bind(6, e.pos.line).bind(7, e.pos.col);
    // Catch facts are emitted before the throws they enclose.
    if (e.handler >= 0 && static_cast<size_t>(e.handler) < i) {
      exception_.bind(8, exc_ids[static_cast<size_t>(e.handler)]).bind_text_or_null(9, e.handler_match);
    } else {
      exception_.bind(8, nullptr).bind(9, nullptr);
    }
    exception_.run();
    exc_ids[i] = db_.last_insert_rowid();
  }
  counts.exceptions = static_cast<int64_t>(facts.exceptions.size());

  for (const auto& e : facts.entries) {
    entry_.bind(1, e.kind).bind(2, sym(e.scope)).bind(3, file_id).bind(4, e.line).bind(5, e.reason).run();
  }
  counts.entries = static_cast<int64_t>(facts.entries.size());
  return counts;
}

}  // namespace proj
