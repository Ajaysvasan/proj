# Index schema

Two kinds of SQLite database live in the data directory:

- `registry.db`: shared. Projects, analysis runs and run diagnostics.
  Upgraded in place by numbered migrations (`PRAGMA user_version`, currently
  1). A registry written by a newer `proj` is refused, never modified.
- `indexes/project-<id>.db`: one per project. `proj add` builds it from
  scratch and `proj refresh` updates a copy of it. Either way the new file is
  atomically renamed into place. Its version is stored in
  `index_meta.schema_version` (currently 3). An index from an older or newer
  schema is refused with a message to re-run `proj add <name> <path>
  --reanalyze`. It is never silently destroyed, and a refresh never upgrades
  it.

All paths in an index are project-relative, use `/` separators, and are never
absolute. Lines and columns are 1-based; columns count bytes.

## registry.db

### projects
| column | meaning |
|---|---|
| `id`, `name` (unique), `root_path` (unique, canonical) | identity |
| `index_path` | `indexes/project-<id>.db`. Named by id, so project names never become paths |
| `created_at`, `last_analysis_at`, `last_success_at` | ISO-8601 UTC |
| `status` | `registered`, `analyzing`, `success`, `partial`, `no_supported_files`, `failed`, `interrupted` |
| `index_schema_version`, `tool_version` | of the last successful index |
| `last_run_id`, `options_json` | analysis options reused by `--reanalyze` |

### analysis_runs
Start/finish timestamps, `duration_ms` (monotonic clock), `status` (`running`,
`success`, `partial`, `no_supported_files`, `failed`, `interrupted`), `pid`,
the counts `files_discovered`, `files_parsed`, `files_skipped`, `symbols`,
`relationships`, `unresolved_refs`, `parse_errors`, `warnings`,
`fatal_errors`, `stats_json` (`mode` = `full` or `refresh`,
`files.config` = configuration files parsed / with syntax errors / skipped,
per-status/per-language file counts, resolution statistics, phase timings,
peak RSS, index size, and for refreshes a `refresh` object with
changed/added/removed/unchanged files, reused symbol ids, changed names,
affected files, reset relationships and timings), `error_message`,
`diagnostics_total` and `diagnostics_dropped`. The newest 50 runs per
project are kept.

### run_diagnostics
`run_id`, `level` (`debug`/`info`/`warn`/`error`), `category` (`source`,
`resource`, `config`, `internal`), `code` (for example `syntax_error`,
`file_too_large`, `directory_unreadable`, `unsupported_construct`,
`analysis_failed`), optional `path` and `line`, and `message`. Storage per run
is bounded; see `diagnostics_dropped`.

## Project index

### index_meta
Key/value: `schema_version`, `tool_version`, `project_name`, `root_path`,
`run_id`, `created_at`, `analysis_status`, `stats` (JSON), `options` (JSON).

### files
One row per visited file (excluded *directories* are not traversed).

| column | meaning |
|---|---|
| `path`, `dir`, `name` | project-relative |
| `language` | `javascript`, `typescript`, `tsx`, `python`, `java`, `c`, `cpp`, `xml`, `yaml` (configuration), or NULL |
| `size_bytes`, `mtime_s` | metadata |
| `fingerprint` | FNV-1a 64 of the content (hex). Only for files that were read. Used with the size, language and extractor version by `proj refresh` for change detection, never for security |
| `status` | see below |
| `error_count`, `first_error_line` | syntax errors found by Tree-sitter |
| `line_count`, `module_name` (Java package / Python module), `extractor_version`, `note` | |

File statuses:

| status | meaning |
|---|---|
| `parsed` | parsed without syntax errors |
| `syntax_errors` | parsed; the tree contains ERROR/MISSING nodes; facts extracted best-effort |
| `parse_failed` | no usable tree (time limit) or the extractor failed |
| `unsupported` | no parser for this file type (not read) |
| `excluded` | matched a default, `.gitignore`, `.git/info/exclude`, `.projignore` or `--exclude` rule |
| `too_large` | larger than the size limit (not read) |
| `binary` | source extension but NUL bytes in the first 8 KiB |
| `unreadable` | could not be opened or read |
| `symlink` | symbolic link; never followed |

### symbols
`id`, `file_id`, `parent_id` (lexical parent), `name`, `qualified_name`,
`kind`, `language`, `start_line/start_col/end_line/end_col`, `exported`,
`visibility`, `is_static`, `is_definition` (0 for prototypes and abstract or
interface methods), `param_count` (NULL when unknown or variadic),
`container` (logical owner for C++ out-of-line members such as
`ns::UserStore`; the class of a Spring bean), `signature`, and `param_types`
(declared parameter types, comma-separated: Java types are normalized
(`List[]`, `String...`), C/C++ types are kept as written without names and
default values; used to select overloads).

Kinds: `module` (one per file; owns top-level code), `function`, `method`,
`constructor`, `class`, `interface`, `struct`, `union`, `enum`, `type_alias`,
`namespace`, `variable` (only module-level values needed for resolution:
router objects, constructed instances), `object` (named object literals),
`route_handler` and `middleware` (inline functions and lambdas passed to
route registrations), and `bean` (a Spring XML bean: `name` is its id,
`container` its class).

Qualified names are scope-qualified: `UserService.findById` (JS/TS),
`app.services.user.UserService.get` (Python, module path),
`com.example.UserService.find` (Java), `store::UserStore::find` (C++).
Symbols with the same name are separate rows and are disambiguated by file
and qualified name.

`symbols_fts` is an FTS5 trigram index over `name` and `qualified_name`. It
backs case-insensitive substring search for queries of three or more
characters.

### file_deps (file → file)
`src_file_id`, `dst_file_id` (when local), `scope_symbol_id` (module = top
level), `kind` (`import`, `type_import`, `require`, `dynamic_import`,
`reexport`, `from_import`, `static_import`, `include`), `specifier` as
written, `line`, `col`, `status`, `note`, `raw_note` (the extractor's note
before resolution) and `generated` (1 for rows the resolver derives: Python
`from pkg import submodule` dependencies).

| status | meaning |
|---|---|
| `resolved` | a project file matched the language's resolution rules |
| `heuristic` | a bare JS specifier matched a project path (`@/x`, `src/x`); `tsconfig` paths are not read |
| `external` | package, standard library or system header |
| `unresolved` | looks local, but no project file matches |
| `ambiguous` | several project files match an include equally well |
| `package` | Java wildcard import of a project package, or a Python namespace package; no single target file |

### bindings, exports, type_hints
Inputs to resolution, kept so `proj refresh` can re-resolve relationships
without re-parsing unchanged files:

- `bindings`: local name → imported name for each import (`named`, `default`,
  `namespace`, `module`, `wildcard`). The resolver may rewrite a binding
  (Python `from pkg import submodule` becomes a `module` binding to the
  submodule's dependency row). `raw_imported`, `raw_kind` and `raw_dep_id`
  keep the extracted values.
- `exports`: exported name → local name or symbol (`symbol`, `instance` (an
  exported `new X()`), `reexport`, `reexport_all`).
- `type_hints`: declared or constructed types of parameters, locals, fields
  and module values (`?` = known name, unknown type). Used to resolve
  `obj.method()` calls. Two special forms:
  - `name = '#return'` holds the declared return type of the scope function.
    It is used for chained calls `a.b().c()` and for values assigned from
    calls.
  - `type_name = '@call:<callee>'` (optionally `@call:await <callee>`) marks a
    value produced by a call, as in `user = repo.find(id)`. The resolver
    evaluates it on demand: the callee's `#return` type, or the class when the
    callee is a constructor.

### edges (symbol → symbol)
`kind`, `src_symbol_id`, `dst_symbol_id` (NULL unless exactly one target),
`file_id`/`line`/`col` (the site), `status`, `target_name`, `receiver`,
`receiver_kind`, `target_text` (expression as written), `arg_count`,
`arg_types` (static argument types at the call site, comma-separated: a type
name, `$name` for a variable whose declared type is looked up, or `?`), `note`
(the reason for the status). `edge_candidates(edge_id, symbol_id)` lists
every candidate of an `ambiguous` edge, ordered by file path and line.

Bookkeeping columns: `raw_kind` (the extracted kind; resolution may turn a
`calls` into `instantiates`), `generated` (1 for edges the resolver derives:
`loads_module`, `overridden_by`, `registers_route`, `uses_middleware`, and
inferred `calls` whose `raw_kind` is `anonymous_class` (a method that creates
a Java anonymous class -> its methods) or `thread_start` (`submit(task)`,
`new Thread(task)`, `start()` -> the task's `run()`/`call()`)) and
`receiver_edge_id` (for a chained call `a.b().c()`, the edge of `b()` whose
result is the receiver of `c()`).

Receiver kinds: `none` (plain call), `this`, `super`, `this_member`
(`this.x.m()`, `self.x.m()`), `identifier`, `qualified` (`a.b.m()`), `call`
(chained on another call), `typed` (`new X().m()`, `((X) o).m()`, `(o as
X).m()`), `module_require` (`require('./x')(...)`), `config_class` / `config_bean`
(`configures` edges: the receiver is a class or bean name from a
configuration file), and `expression` (any other receiver; recorded as
unresolved).

Semantics: `A -[calls]-> B` means a call site inside A is statically
associated with B. It does not prove that the call happens at runtime.

| kind | meaning |
|---|---|
| `calls` | call site in A resolves to B |
| `instantiates` | `new B()` / `B()` constructs class B (target: its constructor when one exists) |
| `extends`, `implements` | class/interface inheritance (unresolved bases stay visible) |
| `overridden_by` | B overrides or implements method A in a subclass (possible dynamic dispatch) |
| `registers_route` | A registers a route whose handler is B |
| `uses_middleware` | A registers middleware B for a router or route |
| `mounts` | A mounts a router defined in module B (`app.use('/api', router)`, `register_blueprint`, `include_router`) |
| `loads_module` | module A imports module B at top level (JS/TS, Python), which executes B's top-level code |
| `configures` | configuration file (or Spring bean) A wires class or method B: the container creates it (servlets, filters, listeners, beans), calls a lifecycle method (`contextInitialized`, `init`, `init-method`), or injects another bean |

| status | meaning |
|---|---|
| `resolved` | target found by scope, import, export or declared-type rules |
| `ambiguous` | several candidates; all are listed in `edge_candidates` |
| `heuristic` | inferred: receiver type unknown and matched by method name alone (only when exactly one project method has that name), or an inferred `anonymous_class` / `thread_start` call (the note says which) |
| `external` | target is in an external package, standard library or language built-in |
| `unresolved` | not determinable statically; `note` explains why (parameter of unknown type, dynamic receiver, failed import, ...) |

File dependencies and call relationships are separate tables because they
answer different questions. The logical "edges between symbols or files" of
the specification is split into `edges` and `file_deps`.

### routes, route_middleware, routers, mounts
- `routers`: recognized app/router objects (`express()`, `Router()`, Flask,
  `Blueprint` with `url_prefix`, `FastAPI`, `APIRouter` with `prefix`).
- `mounts`: `app.use([prefix,] x)` / `register_blueprint` / `include_router`
  arguments, classified by the resolver as `router` (a mount),
  `middleware`, or `unresolved`; Django `include()` targets have
  `target_kind = 'module'` and become `urlconf` (the included URLconf's module
  symbol). `raw_note` keeps the extracted note.
- `routers` also holds Pyramid route names (`kind = 'pyramid_route'`, `name`
  the route name, `prefix` the pattern) that `add_view` / `@view_config`
  routes refer to.
- `routes`: `framework` (`express`, `nestjs`, `flask`, `fastapi`, `django`,
  `spring`, `jaxrs`; configuration: `servlet`, `struts`, `struts2`,
  `spring_xml`, `openapi`, `serverless`, `config`; code without annotations:
  `httpserver`, `spark`, `javalin`, `java_http`, `tornado`, `webapp2`,
  `webpy`, `falcon`, `pyramid`, `bottle`, `http_server`, `wsgi`,
  `python_handlers`, `cpp-httplib`, `crow`, `drogon`, `pistache`, `cpp_http`,
  `cpp_table`, `dispatch_table`, `dispatch`), `method` (`ANY` when
  unrestricted; a handler class serving several methods gives one row per
  method, the extra rows pointing to the first through `origin_route_id`;
  `raw_method` keeps the declared method), `path` as declared,
  `full_path` with the resolved prefix, `mount_prefix`, `mount_status`
  (`resolved`, `class_mapping`, `none`, `unmounted`, `unknown_registrar`),
  `mount_id`, the registration site, `scope_symbol_id`, `handler_*`
  (`handler_kind`: `identifier`, `member`, `inline`, `require`, or a
  reference by name: `class` (dispatched to its request methods),
  `qualified`, `name`, `file`, `bean`, `method_ref`; `handler_status` uses
  the edge statuses), and `confidence` (`recognized`
  when the receiver is a known app/router or the class is a controller,
  `heuristic` otherwise). A router mounted at several prefixes gets one row
  per full path. The extra rows point to the declared route through
  `origin_route_id`. `declared_prefix` is the prefix written at the route
  itself (for example a class-level mapping), and `raw_handler_note` keeps the
  extracted handler note.
- `route_middleware`: ordered `scope` (`mount`, `app`, `router`, `route`)
  entries with their resolution status.

### exceptions
`kind` (`throw`, `rethrow`, `catch`, `declares` for Java `throws`),
`type_name` (`*` for catch-all handlers), `expression`, `symbol_id`, site,
and for throws `handler_id` + `handler_match` (`exact` or `catch_all`): a
lexically enclosing `try` in the same function whose handler may catch it.
Exceptions that callees may raise are computed at query time and labeled as
possible.

### entry_points
`kind` (`main_function`, `main_guard`, `module_main`, `package_main`,
`package_bin`, `package_script`, `framework_app`, `server_start`,
`deployment_descriptor` (`web.xml`), `route_config` (another configuration
file that declares routes)),
`symbol_id`, `file_id`, `line`, `reason`.

## Integrity

`proj show <name> --check` (engine op `check_index`) runs `PRAGMA
quick_check` and counts dangling references: symbols without a file or
parent, edges with a missing source or target, dangling ambiguity
candidates, dependencies, route handlers, exception sites and entry points.
It also reports relationships left `pending` and FTS row-count drift. Every
analysis also runs `quick_check` before replacing the index.

## How `proj refresh` uses the schema

- **Change detection:** `files.fingerprint`, `size_bytes`, `language` and
  `extractor_version`.
- **Stable ids:** symbols of a re-parsed file keep their id when kind,
  qualified name, parameter count and ordinal are unchanged. Edges from other
  files that point to them stay valid.
- **Resetting resolution:** every column the resolver overwrites has a
  pre-resolution copy (`raw_*`), and every row it creates is marked
  (`generated = 1`, or `origin_route_id` for route copies). A refresh deletes
  derived rows, restores `raw_*` values, sets affected rows back to
  `pending`, and runs the resolver again. A full analysis never leaves rows
  `pending`. `check_index` reports any that remain.
- **Temporary state:** a refresh works with `TEMP` tables (`refresh_*`) that
  never reach the index file.

Schema history: version 1 (initial); version 2 added the `raw_*`,
`generated`, `receiver_edge_id`, `declared_prefix` and `origin_route_id`
columns that refresh needs; version 3 added `symbols.param_types`,
`edges.arg_types` (overload selection) and `routes.raw_method` (per-method
routes of handler classes), the `configures` edge kind and configuration
files. An older index must be rebuilt with `proj add <name> <path>
--reanalyze`.
