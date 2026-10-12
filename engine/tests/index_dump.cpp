#include "index_dump.hpp"

#include <algorithm>

namespace proj::test {
namespace {

// SQL fragment: stable key for the symbol with id <col>.
std::string sym_key(const std::string& col) {
  // Distinct aliases (ks/kf) so the subquery never binds to the outer query's tables.
  return "(SELECT kf.path || ':' || ks.qualified_name || ':' || ks.kind || ':' || ks.start_line || ':' || ks.start_col "
         "FROM symbols ks JOIN files kf ON kf.id = ks.file_id WHERE ks.id = " + col + ")";
}

std::string file_key(const std::string& col) { return "(SELECT kf2.path FROM files kf2 WHERE kf2.id = " + col + ")"; }

void collect(db::Database& db, const std::string& label, const std::string& sql, std::vector<std::string>& out) {
  auto st = db.prepare(sql);
  while (st.step()) out.push_back(label + "|" + st.text(0));
}

}  // namespace

std::vector<std::string> canonical_dump(db::Database& db) {
  std::vector<std::string> out;
  auto c = [](const std::string& expr) { return "IFNULL(" + expr + ", '-')"; };
  collect(db, "file",
          "SELECT path || '|' || " + c("language") + " || '|' || status || '|' || " + c("fingerprint") + " || '|' || " +
              "error_count || '|' || " + c("module_name") + " || '|' || " + c("note") + " FROM files",
          out);
  collect(db, "symbol",
          "SELECT " + sym_key("s.id") + " || '|' || s.end_line || ':' || s.end_col || '|' || " + c(sym_key("s.parent_id")) +
              " || '|' || s.exported || s.is_static || s.is_definition || '|' || " + c("s.visibility") + " || '|' || " +
              c("s.param_count") + " || '|' || " + c("s.container") + " || '|' || " + c("s.signature") + " FROM symbols s",
          out);
  collect(db, "edge",
          "SELECT e.kind || '|' || e.raw_kind || '|' || e.generated || '|' || e.status || '|' || " + file_key("e.file_id") +
              " || ':' || e.line || ':' || e.col || '|' || " + c(sym_key("e.src_symbol_id")) + " || '|' || " +
              c(sym_key("e.dst_symbol_id")) + " || '|' || " + c("e.target_name") + " || '|' || " + c("e.receiver") +
              " || '|' || " + c("e.receiver_kind") + " || '|' || " + c("e.target_text") + " || '|' || " +
              c("e.arg_count") + " || '|' || " + c("e.note") + " || '|' || " +
              c("(SELECT r.target_text || '@' || r.line || ':' || r.col FROM edges r WHERE r.id = e.receiver_edge_id)") +
              " || '|' || " +
              c("(SELECT group_concat(k, ',') FROM (SELECT " + sym_key("ec.symbol_id") +
                " AS k FROM edge_candidates ec WHERE ec.edge_id = e.id ORDER BY k))") +
              " FROM edges e",
          out);
  collect(db, "dep",
          "SELECT " + file_key("d.src_file_id") + " || '|' || d.kind || '|' || d.specifier || '|' || d.line || ':' || d.col || "
              "'|' || d.status || '|' || " + c(file_key("d.dst_file_id")) + " || '|' || " + c("d.note") + " || '|' || " +
              "d.generated || '|' || " + c(sym_key("d.scope_symbol_id")) + " FROM file_deps d",
          out);
  collect(db, "binding",
          "SELECT " + file_key("b.file_id") + " || '|' || b.local_name || '|' || b.imported_name || '|' || b.kind || '|' || "
              "(SELECT d.specifier || '@' || d.line || ':' || d.generated FROM file_deps d WHERE d.id = b.dep_id) || '|' || "
              "b.raw_imported || '|' || b.raw_kind FROM bindings b",
          out);
  collect(db, "export",
          "SELECT " + file_key("x.file_id") + " || '|' || x.exported_name || '|' || " + c("x.local_name") + " || '|' || " +
              c(sym_key("x.symbol_id")) + " || '|' || x.kind || '|' || x.line FROM exports x",
          out);
  collect(db, "hint",
          "SELECT " + sym_key("h.scope_symbol_id") + " || '|' || h.name || '|' || h.type_name || '|' || h.kind FROM type_hints h",
          out);
  collect(db, "router",
          "SELECT " + file_key("r.file_id") + " || '|' || r.name || '|' || r.kind || '|' || r.framework || '|' || " +
              c("r.prefix") + " || '|' || r.line FROM routers r",
          out);
  collect(db, "mount",
          "SELECT " + file_key("m.file_id") + " || ':' || m.line || ':' || m.col || '|' || m.registrar || '|' || " +
              c("m.prefix") + " || '|' || m.target_kind || '|' || " + c("m.target_name") + " || '|' || m.status || '|' || " +
              c("(SELECT " + file_key("r.file_id") + " || ':' || r.name FROM routers r WHERE r.id = m.target_router_id)") +
              " || '|' || " + c(sym_key("m.target_symbol_id")) + " || '|' || " + c("m.note") + " FROM mounts m",
          out);
  collect(db, "route",
          "SELECT r.framework || '|' || r.method || '|' || r.path || '|' || r.full_path || '|' || " + c("r.mount_prefix") +
              " || '|' || r.mount_status || '|' || " + file_key("r.file_id") + " || ':' || r.line || ':' || r.col || '|' || " +
              c("r.registrar") + " || '|' || " + c(sym_key("r.handler_symbol_id")) + " || '|' || r.handler_status || '|' || " +
              c("r.handler_note") + " || '|' || r.confidence || '|' || (r.origin_route_id IS NOT NULL) || '|' || " +
              c("(SELECT " + file_key("m.file_id") + " || ':' || m.line FROM mounts m WHERE m.id = r.mount_id)") +
              " || '|' || " + c(sym_key("r.scope_symbol_id")) + " FROM routes r",
          out);
  collect(db, "middleware",
          "SELECT " + file_key("r.file_id") + " || ':' || r.line || ':' || r.method || ':' || r.full_path || '|' || "
              "w.position || '|' || w.scope || '|' || w.ref_kind || '|' || " + c("w.name") + " || '|' || w.text || '|' || " +
              c(sym_key("w.symbol_id")) + " || '|' || w.status || '|' || " + file_key("w.file_id") + " || ':' || w.line "
              "FROM route_middleware w JOIN routes r ON r.id = w.route_id",
          out);
  collect(db, "exception",
          "SELECT x.kind || '|' || " + c("x.type_name") + " || '|' || " + c("x.expression") + " || '|' || " +
              c(sym_key("x.symbol_id")) + " || '|' || " + file_key("x.file_id") + " || ':' || x.line || ':' || x.col || '|' || " +
              c("(SELECT h.line || ':' || h.col FROM exceptions h WHERE h.id = x.handler_id)") + " || '|' || " +
              c("x.handler_match") + " FROM exceptions x",
          out);
  collect(db, "entry",
          "SELECT e.kind || '|' || " + c(sym_key("e.symbol_id")) + " || '|' || " + file_key("e.file_id") +
              " || ':' || e.line || '|' || e.reason FROM entry_points e",
          out);
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace proj::test
