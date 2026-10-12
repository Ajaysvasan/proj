#include "db/schema.hpp"

#include <array>
#include <string>

#include "core/version.hpp"

namespace proj::db {
namespace {

// ---------------------------------------------------------------------------
// Index schema (one SQLite file per project). See docs/INDEX_SCHEMA.md.
// Every index file is built from scratch by one analysis run and renamed into
// place on success, so the index schema is versioned for compatibility checks
// rather than migrated in place.
// ---------------------------------------------------------------------------
constexpr const char* kIndexSchema = R"SQL(
CREATE TABLE index_meta (
  key   TEXT PRIMARY KEY,
  value TEXT NOT NULL
);

CREATE TABLE files (
  id                INTEGER PRIMARY KEY,
  path              TEXT NOT NULL UNIQUE,
  dir               TEXT NOT NULL,
  name              TEXT NOT NULL,
  language          TEXT,
  size_bytes        INTEGER NOT NULL,
  mtime_s           INTEGER,
  fingerprint       TEXT,
  status            TEXT NOT NULL,
  error_count       INTEGER NOT NULL DEFAULT 0,
  first_error_line  INTEGER,
  line_count        INTEGER,
  module_name       TEXT,
  extractor_version INTEGER,
  note              TEXT
);
CREATE INDEX files_name ON files(name);
CREATE INDEX files_dir ON files(dir);
CREATE INDEX files_path_nocase ON files(path COLLATE NOCASE);
CREATE INDEX files_module ON files(module_name) WHERE module_name IS NOT NULL;

CREATE TABLE symbols (
  id            INTEGER PRIMARY KEY,
  file_id       INTEGER NOT NULL REFERENCES files(id),
  parent_id     INTEGER REFERENCES symbols(id),
  name          TEXT NOT NULL,
  qualified_name TEXT NOT NULL,
  kind          TEXT NOT NULL,
  language      TEXT NOT NULL,
  start_line    INTEGER NOT NULL,
  start_col     INTEGER NOT NULL,
  end_line      INTEGER NOT NULL,
  end_col       INTEGER NOT NULL,
  exported      INTEGER NOT NULL DEFAULT 0,
  visibility    TEXT,
  is_static     INTEGER NOT NULL DEFAULT 0,
  is_definition INTEGER NOT NULL DEFAULT 1,
  param_count   INTEGER,
  container     TEXT,
  signature     TEXT,
  param_types   TEXT
);
CREATE INDEX symbols_name ON symbols(name, kind);
CREATE INDEX symbols_name_nocase ON symbols(name COLLATE NOCASE);
CREATE INDEX symbols_qname ON symbols(qualified_name);
CREATE INDEX symbols_qname_nocase ON symbols(qualified_name COLLATE NOCASE);
CREATE INDEX symbols_file ON symbols(file_id, name);
CREATE INDEX symbols_parent ON symbols(parent_id, name);
CREATE INDEX symbols_location ON symbols(file_id, start_line);
CREATE INDEX symbols_container ON symbols(container, name) WHERE container IS NOT NULL;

-- Trigram index for case-insensitive substring search over symbol names.
CREATE VIRTUAL TABLE symbols_fts USING fts5(
  name, qualified_name, content='symbols', content_rowid='id', tokenize='trigram');

CREATE TABLE file_deps (
  id          INTEGER PRIMARY KEY,
  src_file_id INTEGER NOT NULL REFERENCES files(id),
  dst_file_id INTEGER REFERENCES files(id),
  scope_symbol_id INTEGER REFERENCES symbols(id),
  kind        TEXT NOT NULL,
  specifier   TEXT NOT NULL,
  line        INTEGER NOT NULL,
  col         INTEGER NOT NULL,
  status      TEXT NOT NULL,
  note        TEXT,
  raw_note    TEXT,
  generated   INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX file_deps_src ON file_deps(src_file_id, line);
CREATE INDEX file_deps_dst ON file_deps(dst_file_id);

CREATE TABLE bindings (
  id            INTEGER PRIMARY KEY,
  file_id       INTEGER NOT NULL REFERENCES files(id),
  dep_id        INTEGER NOT NULL REFERENCES file_deps(id),
  local_name    TEXT NOT NULL,
  imported_name TEXT NOT NULL,
  kind          TEXT NOT NULL,
  line          INTEGER NOT NULL,
  raw_imported  TEXT NOT NULL,
  raw_kind      TEXT NOT NULL,
  raw_dep_id    INTEGER NOT NULL
);
CREATE INDEX bindings_file ON bindings(file_id, local_name);

CREATE TABLE exports (
  id            INTEGER PRIMARY KEY,
  file_id       INTEGER NOT NULL REFERENCES files(id),
  exported_name TEXT NOT NULL,
  local_name    TEXT,
  symbol_id     INTEGER REFERENCES symbols(id),
  kind          TEXT NOT NULL,
  dep_id        INTEGER REFERENCES file_deps(id),
  line          INTEGER NOT NULL
);
CREATE INDEX exports_file ON exports(file_id, exported_name);

CREATE TABLE type_hints (
  id              INTEGER PRIMARY KEY,
  file_id         INTEGER NOT NULL REFERENCES files(id),
  scope_symbol_id INTEGER NOT NULL REFERENCES symbols(id),
  name            TEXT NOT NULL,
  type_name       TEXT NOT NULL,
  kind            TEXT NOT NULL
);
CREATE INDEX type_hints_scope ON type_hints(scope_symbol_id, name);

CREATE TABLE edges (
  id            INTEGER PRIMARY KEY,
  kind          TEXT NOT NULL,
  src_symbol_id INTEGER NOT NULL REFERENCES symbols(id),
  dst_symbol_id INTEGER REFERENCES symbols(id),
  file_id       INTEGER NOT NULL REFERENCES files(id),
  line          INTEGER NOT NULL,
  col           INTEGER NOT NULL,
  status        TEXT NOT NULL,
  target_name   TEXT,
  receiver      TEXT,
  receiver_kind TEXT,
  target_text   TEXT,
  arg_count     INTEGER,
  note          TEXT,
  raw_kind      TEXT NOT NULL,
  generated     INTEGER NOT NULL DEFAULT 0,
  receiver_edge_id INTEGER REFERENCES edges(id),
  arg_types     TEXT
);
CREATE INDEX edges_src ON edges(src_symbol_id, kind);
CREATE INDEX edges_dst ON edges(dst_symbol_id, kind);
CREATE INDEX edges_file ON edges(file_id, status);

CREATE TABLE edge_candidates (
  edge_id   INTEGER NOT NULL REFERENCES edges(id),
  symbol_id INTEGER NOT NULL REFERENCES symbols(id),
  PRIMARY KEY (edge_id, symbol_id)
) WITHOUT ROWID;
CREATE INDEX edge_candidates_symbol ON edge_candidates(symbol_id);

CREATE TABLE routers (
  id              INTEGER PRIMARY KEY,
  file_id         INTEGER NOT NULL REFERENCES files(id),
  scope_symbol_id INTEGER REFERENCES symbols(id),
  name            TEXT NOT NULL,
  kind            TEXT NOT NULL,
  framework       TEXT NOT NULL,
  prefix          TEXT,
  line            INTEGER NOT NULL
);
CREATE INDEX routers_file ON routers(file_id, name);

CREATE TABLE mounts (
  id               INTEGER PRIMARY KEY,
  file_id          INTEGER NOT NULL REFERENCES files(id),
  scope_symbol_id  INTEGER REFERENCES symbols(id),
  framework        TEXT NOT NULL,
  registrar        TEXT NOT NULL,
  prefix           TEXT,
  target_kind      TEXT NOT NULL,
  target_name      TEXT,
  target_receiver  TEXT,
  target_text      TEXT,
  line             INTEGER NOT NULL,
  col              INTEGER NOT NULL,
  target_router_id INTEGER REFERENCES routers(id),
  target_symbol_id INTEGER REFERENCES symbols(id),
  status           TEXT NOT NULL,
  note             TEXT,
  raw_note         TEXT
);
CREATE INDEX mounts_target ON mounts(target_router_id);
CREATE INDEX mounts_file ON mounts(file_id, registrar);

CREATE TABLE routes (
  id                INTEGER PRIMARY KEY,
  framework         TEXT NOT NULL,
  method            TEXT NOT NULL,
  path              TEXT NOT NULL,
  full_path         TEXT NOT NULL,
  mount_prefix      TEXT,
  mount_status      TEXT NOT NULL,
  mount_id          INTEGER REFERENCES mounts(id),
  file_id           INTEGER NOT NULL REFERENCES files(id),
  line              INTEGER NOT NULL,
  col               INTEGER NOT NULL,
  registrar         TEXT,
  router_id         INTEGER REFERENCES routers(id),
  scope_symbol_id   INTEGER REFERENCES symbols(id),
  handler_kind      TEXT,
  handler_name      TEXT,
  handler_receiver  TEXT,
  handler_text      TEXT,
  handler_symbol_id INTEGER REFERENCES symbols(id),
  handler_status    TEXT NOT NULL,
  handler_note      TEXT,
  confidence        TEXT NOT NULL,
  declared_prefix   TEXT,
  raw_handler_note  TEXT,
  origin_route_id   INTEGER REFERENCES routes(id),
  raw_method        TEXT NOT NULL
);
CREATE INDEX routes_full_path ON routes(full_path COLLATE NOCASE);
CREATE INDEX routes_method ON routes(method, full_path);
CREATE INDEX routes_handler ON routes(handler_symbol_id);
CREATE INDEX routes_file ON routes(file_id, line);

CREATE TABLE route_middleware (
  id        INTEGER PRIMARY KEY,
  route_id  INTEGER NOT NULL REFERENCES routes(id),
  position  INTEGER NOT NULL,
  scope     TEXT NOT NULL,
  ref_kind  TEXT NOT NULL,
  name      TEXT,
  receiver  TEXT,
  text      TEXT NOT NULL,
  symbol_id INTEGER REFERENCES symbols(id),
  status    TEXT NOT NULL,
  file_id   INTEGER NOT NULL REFERENCES files(id),
  line      INTEGER NOT NULL
);
CREATE INDEX route_middleware_route ON route_middleware(route_id, position);

CREATE TABLE exceptions (
  id            INTEGER PRIMARY KEY,
  kind          TEXT NOT NULL,
  type_name     TEXT,
  expression    TEXT,
  symbol_id     INTEGER REFERENCES symbols(id),
  file_id       INTEGER NOT NULL REFERENCES files(id),
  line          INTEGER NOT NULL,
  col           INTEGER NOT NULL,
  handler_id    INTEGER REFERENCES exceptions(id),
  handler_match TEXT
);
CREATE INDEX exceptions_symbol ON exceptions(symbol_id, line);
CREATE INDEX exceptions_file ON exceptions(file_id, line);
CREATE INDEX exceptions_type ON exceptions(type_name);

CREATE TABLE entry_points (
  id        INTEGER PRIMARY KEY,
  kind      TEXT NOT NULL,
  symbol_id INTEGER REFERENCES symbols(id),
  file_id   INTEGER NOT NULL REFERENCES files(id),
  line      INTEGER NOT NULL,
  reason    TEXT NOT NULL
);
CREATE INDEX entry_points_symbol ON entry_points(symbol_id);
)SQL";

// ---------------------------------------------------------------------------
// Registry schema migrations. Each step upgrades PRAGMA user_version by one.
// Append new steps; never edit a released step.
// ---------------------------------------------------------------------------
constexpr std::array<const char*, 1> kRegistryMigrations = {
    // v1: initial registry
    R"SQL(
CREATE TABLE projects (
  id                   INTEGER PRIMARY KEY,
  name                 TEXT NOT NULL UNIQUE,
  root_path            TEXT NOT NULL UNIQUE,
  index_path           TEXT NOT NULL,
  created_at           TEXT NOT NULL,
  last_analysis_at     TEXT,
  last_success_at      TEXT,
  status               TEXT NOT NULL,
  index_schema_version INTEGER,
  tool_version         TEXT,
  last_run_id          INTEGER,
  options_json         TEXT NOT NULL DEFAULT '{}'
);

CREATE TABLE analysis_runs (
  id                   INTEGER PRIMARY KEY,
  project_id           INTEGER NOT NULL REFERENCES projects(id),
  project_name         TEXT NOT NULL,
  started_at           TEXT NOT NULL,
  finished_at          TEXT,
  duration_ms          REAL,
  status               TEXT NOT NULL,
  pid                  INTEGER,
  tool_version         TEXT NOT NULL,
  index_schema_version INTEGER NOT NULL,
  files_discovered     INTEGER NOT NULL DEFAULT 0,
  files_parsed         INTEGER NOT NULL DEFAULT 0,
  files_skipped        INTEGER NOT NULL DEFAULT 0,
  symbols              INTEGER NOT NULL DEFAULT 0,
  relationships        INTEGER NOT NULL DEFAULT 0,
  unresolved_refs      INTEGER NOT NULL DEFAULT 0,
  parse_errors         INTEGER NOT NULL DEFAULT 0,
  warnings             INTEGER NOT NULL DEFAULT 0,
  fatal_errors         INTEGER NOT NULL DEFAULT 0,
  stats_json           TEXT NOT NULL DEFAULT '{}',
  error_message        TEXT,
  diagnostics_total    INTEGER NOT NULL DEFAULT 0,
  diagnostics_dropped  INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX analysis_runs_project ON analysis_runs(project_id, id);
CREATE INDEX analysis_runs_started ON analysis_runs(id DESC);

CREATE TABLE run_diagnostics (
  id       INTEGER PRIMARY KEY,
  run_id   INTEGER NOT NULL REFERENCES analysis_runs(id),
  level    TEXT NOT NULL,
  category TEXT NOT NULL,
  code     TEXT NOT NULL,
  path     TEXT,
  line     INTEGER,
  message  TEXT NOT NULL
);
CREATE INDEX run_diagnostics_run ON run_diagnostics(run_id, id);
)SQL",
};

}  // namespace

void configure_index_build(Database& db) {
  // The build file is private and discarded on failure, so durability during
  // the build is unnecessary; the finished file is synced before the rename.
  db.exec("PRAGMA journal_mode=OFF");
  db.exec("PRAGMA synchronous=OFF");
  db.exec("PRAGMA temp_store=FILE");
  db.exec("PRAGMA cache_size=-16384");  // 16 MiB page cache upper bound
  db.exec("PRAGMA foreign_keys=OFF");
}

void configure_index_read(Database& db) {
  db.exec("PRAGMA query_only=ON");
  db.exec("PRAGMA cache_size=-8192");
  db.exec("PRAGMA temp_store=MEMORY");
}

void create_index_schema(Database& db) {
  db.exec(kIndexSchema);
  auto st = db.prepare("INSERT INTO index_meta(key, value) VALUES (?, ?)");
  st.bind(1, "schema_version").bind(2, std::to_string(kIndexSchemaVersion)).run();
  st.bind(1, "tool_version").bind(2, kToolVersion).run();
  db.exec("PRAGMA user_version=" + std::to_string(kIndexSchemaVersion));
}

int check_index_schema(Database& db) {
  int version = 0;
  try {
    auto st = db.prepare("SELECT value FROM index_meta WHERE key='schema_version'");
    if (!st.step()) throw SchemaError("index metadata is missing the schema version");
    version = std::stoi(st.text(0));
  } catch (const Error& e) {
    throw SchemaError(std::string("index is unreadable or corrupt: ") + e.what());
  } catch (const std::invalid_argument&) {
    throw SchemaError("index metadata contains an invalid schema version");
  }
  if (version > kIndexSchemaVersion) {
    throw SchemaError("index schema v" + std::to_string(version) +
                      " was written by a newer proj; this version reads up to v" +
                      std::to_string(kIndexSchemaVersion));
  }
  if (version < kIndexSchemaVersion) {
    throw SchemaError("index schema v" + std::to_string(version) +
                      " is older than the supported v" + std::to_string(kIndexSchemaVersion) +
                      "; re-run the analysis with `proj add <name> <path> --reanalyze`");
  }
  return version;
}

int registry_schema_version(Database& db) { return static_cast<int>(db.query_int("PRAGMA user_version").value_or(0)); }

void migrate_registry(Database& db) {
  db.exec("PRAGMA journal_mode=WAL");
  db.exec("PRAGMA synchronous=NORMAL");
  db.exec("PRAGMA foreign_keys=ON");
  int current = registry_schema_version(db);
  int latest = static_cast<int>(kRegistryMigrations.size());
  if (current > latest) {
    throw SchemaError("registry schema v" + std::to_string(current) +
                      " was written by a newer proj; this version supports up to v" + std::to_string(latest));
  }
  for (int v = current; v < latest; ++v) {
    Transaction tx(db);
    db.exec(kRegistryMigrations[static_cast<size_t>(v)]);
    db.exec("PRAGMA user_version=" + std::to_string(v + 1));
    tx.commit();
  }
}

}  // namespace proj::db
