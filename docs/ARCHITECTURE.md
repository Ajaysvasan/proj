# Architecture

```
proj (Node.js / TypeScript)            proj-engine (C++20)                 SQLite files
┌──────────────────────────┐  JSON   ┌────────────────────────────────┐   ┌──────────────────────┐
│ Commander CLI            │ ──────▶ │ protocol: validate & dispatch  │   │ registry.db          │
│  add/refresh/log/show    │ stdin   │ registry (projects, runs)      │──▶│  projects, runs,     │
│ Ink terminal UI          │ ◀────── │ scanner → parser → extractors  │   │  run_diagnostics     │
│ engine client            │ stdout  │ fact writer → resolver         │   ├──────────────────────┤
│ app log (JSONL, rotated) │         │ query layer (bounded)          │──▶│ indexes/project-N.db │
└──────────────────────────┘         └────────────────────────────────┘   └──────────────────────┘
```

The engine owns all parsing, indexing, symbol resolution, graph construction
and graph queries. The CLI only validates user input, renders results and
manages the engine process. It contains no parser and never rebuilds the graph
itself.

## Components (engine/src)

| Module | Responsibility |
|---|---|
| `core/` | Paths and data directory, structured logging (stderr plus rotated JSONL file), monotonic timing, FNV-1a fingerprints, peak RSS |
| `db/sqlite.*` | RAII wrappers: `Database`, prepared `Statement`, `Transaction` |
| `db/schema.*` | Versioned index schema; registry migrations (`PRAGMA user_version`) |
| `scan/` | Safe traversal (no symlinks, depth and file limits), gitignore-style rules, language by extension |
| `parse/parser.*` | One reusable Tree-sitter parser; per-file time limit; parse status classification |
| `parse/extract_*.cpp` | Language extractors that produce the shared `FileFacts` representation |
| `parse/extract_config.cpp` | XML and YAML: one small tree for both, then recognizers for web.xml, Struts 1/2, Spring XML, OpenAPI, serverless.yml and generic path + handler entries |
| `parse/route_util.hpp` | Shared rules for routes registered without a framework: route-like literals, HTTP method spellings, dispatch-branch handler choice |
| `index/store.*` | Writes one file's facts with prepared statements; cross-file references stay `pending` |
| `index/resolver.*` | Resolves dependencies, inheritance, calls (including receiver types and chained calls), routes, mounts and middleware through indexed SQL lookups |
| `index/resolver_config.cpp` | References by name (classes, `Class#method`, `module.function`, files, beans) and the per-framework dispatch from a handler class to its request methods |
| `index/resolver_overloads.cpp` | Overload selection by static argument types (Java, C/C++) |
| `index/refresh.*` | Incremental refresh: change detection, symbol id reuse, affected-set computation, reset of derived rows |
| `index/indexer.*` | Orchestrates a full run or a refresh; builds a private file and atomically renames it over the old index |
| `index/registry.*` | Projects, runs and bounded per-run diagnostics |
| `query/query.*` | Search, symbol/file details, edges, bounded BFS traversal, route flow, exceptions, startup path, integrity check, source snippets |
| `query/overview.cpp` | Code map: the project's directories grouped into a bounded number of parts, with per-part counts and aggregated links between parts |
| `protocol/protocol.*` | JSON envelope, argument validation, error categories, read-only index cache in serve mode |

## Analysis data flow

1. **Validate** (`add_project`): project name, existing readable directory,
   canonical path, duplicate name or path, a concurrent run (live pid).
2. **Start a run** in `registry.db` (`status = running`, pid recorded).
3. **Build a new index** in `indexes/project-N.db.building-<pid>` (private file;
   journaling off because the file is discarded on failure).
4. **Scan and parse, one file at a time.** Each eligible file is read,
   fingerprinted and parsed. Facts are extracted in a single iterative,
   depth-limited walk and written immediately. The syntax tree and source
   buffer are released before the next file. Writes are committed every 256
   files.
5. **Resolve** in bounded passes: file dependencies → Python submodule
   bindings → module-load edges → inheritance → calls (64 files per
   transaction) → override/dispatch edges → mounts → route handlers → Java
   anonymous-class and thread-start edges (inferred) → route middleware →
   route prefixes → `package.json` entry points. Every lookup is
   an indexed query; caches are capped at 20,000 entries and cleared when
   full.
6. **Finalize**: FTS5 trigram index rebuild, counts, `index_meta`, `ANALYZE`,
   `PRAGMA quick_check`.
7. **Replace atomically**: `rename()` the build file over the live index.
   On any failure the build file is deleted and the previous index stays.
8. **Record** the run result, counts, timings, peak RSS and bounded
   diagnostics. Run history is pruned to 50 runs per project.

A crash leaves the run in `running`. The next registry access sees that the
pid no longer exists and marks the run `interrupted`.

## Refresh data flow (`proj refresh`)

1. **Validate** (`refresh_project`): registered project, directory still
   exists, index present and of the current schema version, no concurrent run.
2. **Copy** the live index to `project-N.db.building-<pid>` and work on the
   copy. The live index stays readable and is untouched on failure.
3. **Scan with the stored options.** Every source file is read and
   fingerprinted. A file whose fingerprint, size, language and extractor
   version match its row is counted as unchanged and not parsed (only its
   `mtime_s` is updated). Others are new or changed.
4. **Forget and re-extract changed files.** Before deleting a changed file's
   facts, its exported interface and symbol signatures are snapshotted into
   temporary `refresh_*` tables. The file is then parsed and its facts written
   again. Symbols keep their previous id when their key (kind, qualified name,
   parameter count, ordinal among equal keys) is unchanged. Files the scan no
   longer finds are deleted with their facts. These per-file statements run
   once per changed file, so each must use an index: a full table scan there
   makes a refresh with many changed files quadratic (`file_scan_steps` in
   `stats.refresh` counts the rows they scanned).
5. **Compute the affected set** (`IncrementalUpdate::prepare`):
   - **changed names:** definitions that were added, removed, or whose
     signature, return type or base classes changed. Closed over subclasses and
     over functions declared to return a changed type.
   - **edges reset to `pending`:** edges whose callee or receiver token is a
     changed name, and edges chained on those (`receiver_edge_id`)
   - **files reset completely:** changed and added files, files with a
     relationship to a deleted symbol, files whose type hints mention a changed
     name, importers (transitively) of a file whose exported interface changed
     or that was added or removed, and files whose import resolution changed
   - **derived rows:** rows with `generated = 1` (module loads, overrides,
     anonymous-class and thread-start calls, route registration and middleware edges, Python submodule dependencies,
     per-prefix route copies) are deleted. Columns the resolver overwrote are
     restored from their `raw_*` copies. Route mounts, prefixes, handlers and
     middleware are always re-resolved, which is cheap.
   When more than `max_affected_fraction` (default 0.4) of the source files are
   affected, every relationship is reset instead.
6. **Resolve** dependencies, then only the `pending` relationships, with the
   same resolver passes as a full analysis. Derived rows are regenerated.
7. **Finalize and replace** as in a full analysis (FTS rebuild, statistics,
   `quick_check`, `VACUUM` only when more than a quarter of the pages are free,
   atomic rename). The run is recorded with `stats.mode = "refresh"` and a
   `stats.refresh` breakdown (changed/added/removed/unchanged files, reused
   symbol ids, changed names, affected files, reset relationships, timings,
   `file_scan_steps`).

Correctness criterion: a refresh must produce the same index as a fresh full
analysis of the same files, apart from row ids. `engine/tests/test_refresh.cpp`
checks this with randomized edit rounds, and `proj_index_dump` compares real
repositories (see DEVELOPMENT.md).

## How a call is resolved

Extractors record, for every call site: the callee name, the receiver text and
its kind (`none`, `this`, `super`, `this_member`, `identifier`, `qualified`,
`call` for chained calls, `typed` for `new X().m()` or casts, `expression`
for anything else), the argument
count, and the enclosing symbol. They also record type hints: declared types
of parameters, locals and fields, constructed values (`x = new X()`), a
function's declared return type (`#return`), and values produced by calls
(`@call:<callee>`, evaluated lazily by the resolver and memoized).

The resolver then works through the evidence in a fixed order and stops at the
first conclusive answer:

1. **Chained receiver.** For `a.b().c()`, `b()` is resolved first (linked
   through `receiver_edge_id`). Its declared return type, or the class it
   constructs, becomes the receiver type of `c()`.
2. **Receiver type.** From the hints along the scope chain (block, function,
   class fields, module), from import bindings and exports, or from `@call`
   hints through the callee's `#return` type. With a project class, the
   method is looked up in the class and then its resolved base classes. A type
   from a library or the standard library makes the call `external`.
3. **No receiver.** Lexical scope, imports, the enclosing class and its outer
   classes, then language rules (Java single static imports before wildcard
   static imports, C/C++ declarations visible through the include closure).
4. **Overloads** are filtered by argument count, then by the static types of
   the arguments (literals, `new X()`, casts, declared variables and fields;
   exact over widening/boxing/subtype). Remaining ties become `ambiguous`,
   with every candidate in `edge_candidates`, ordered by file and line.
5. **Fallback by name.** If exactly one project method has the name, the call
   is `heuristic`. Two to five give `ambiguous`. Otherwise, and always for very
   common names (`get`, `map`, ...), it is `unresolved` with a note naming the
   missing evidence (for example `receiver 'x' has no statically known
   type`).

Calls whose receiver resolves to a project class but whose method is
inherited from an external base class (`extends Thread`) are `external`, and
the note names that base.

## Routes without annotations

Legacy servers declare endpoints in configuration files, register them in
code without annotations, or dispatch on the request path by hand. The
extractors turn all of these into ordinary `routes` rows whose handler may be
a reference by name (`handler_kind` `class`, `qualified`, `name`, `file`,
`bean`, `method_ref`):

1. **Configuration files** (`parse/extract_config.cpp`): XML and YAML are
   parsed with Tree-sitter and converted to one tree of named nodes with
   attributes, text and children. Recognizers for known formats (web.xml,
   Struts, Spring XML, OpenAPI, serverless.yml) run first; otherwise any
   entry with a URL path and a code reference becomes a `heuristic` route.
   They also emit `configures` edges for the classes a container creates.
2. **Code registrations** in the Java, Python and C/C++ extractors:
   `addServlet`/`addMapping`, `createContext`, Spark/Javalin, Tornado/web.py/
   Falcon/Pyramid/Bottle/Django lists and calls, C/C++ registration calls and
   route tables. Lambdas passed as handlers become `route_handler` symbols.
3. **Dispatch branches**: `if`/`switch`/`match` on the request path with a
   handler call in the branch (`heuristic`).

During resolution, a handler that is a class (named in configuration, a
handler object, a class-based view) is dispatched to its request methods by
the framework's convention (`doGet`/`doPost`/`service`, `execute`,
`handleRequest`, `get`/`post`, `on_get`, `do_GET`, `handle`, ...), searched
in the class and its project base classes. A class serving several HTTP
methods becomes one route per method (`origin_route_id`, with the declared
method kept in `raw_method` so a refresh can redo it). Servlet filters from
web.xml become route middleware resolved to `doFilter`. Django `include()`
prefixes and Pyramid route names are applied when prefixes are computed.

## Search

`search` (engine op; the UI's search box) runs up to three independent
lookups and returns each list separately:

- **Symbols.** Exact mode compares `name` or `qualified_name` for equality.
  Otherwise a query of three or more characters is matched as a phrase against
  the FTS5 **trigram** index over `name` and `qualified_name`. This is a true
  case-insensitive substring match that does not scan the table. Shorter
  queries use a `name LIKE 'q%'` prefix match. Ranking: exact name, then names
  starting with the query, then shorter names, then path and line. Module
  symbols are not returned. Results with the same name are flagged
  `duplicate_name`.
- **Files.** Substring of the project-relative path. Exact file names rank
  first, then names starting with the query, then supported languages, then
  shorter paths.
- **Routes.** An optional leading HTTP method (`GET`, `post`, ...) filters by
  method (`ALL`/`ANY` routes always match). When present, only routes are
  searched. Paths are normalized to a *shape* with parameters (`:id`, `{id}`,
  `<int:id>`) replaced by `*` (one segment), a literal `*` or `**` (servlet
  `/api/*`, Ant `/**`) as a wildcard for any number of segments, `*.do`
  (servlet extension mapping) kept as a suffix pattern, and segments
  lower-cased. A route scores 3 when its shape equals the query's shape or
  matches it segment by segment (a concrete query segment such as `42`
  matches a parameter; `/api/users/42/orders` matches `/api/users/*`;
  `/login.do` matches `*.do`), 2 when its shape starts with the query's
  shape, and 1 when its full path contains the query as text.

Every list is capped by `limit` (at most 500). The UI asks for 15 per kind,
waits 120 ms after the last keystroke, and discards responses to superseded
queries.

In the UI's search box, `/show <endpoint>` (for example `/show GET
/api/users/42`) searches routes only; `proj show <project> <endpoint>` runs
the same search and opens the best match (or prints its flow with
`--no-tui`).

## Code map

The home screen draws the project as a graph of its main parts. The engine's
`overview` operation (`query/overview.cpp`) computes the parts; the CLI only
lays them out.

- **Parts.** Analyzed files under the focus directory form a directory tree;
  chains of directories with a single child and no files are collapsed
  (`src/main/java/com/acme` becomes one step). The largest group is expanded
  into its children while the count stays within `max_nodes` (2 to 40, 14 by
  default; the UI picks 8 to 16 by terminal width). When the top level alone
  has too many parts, the largest are kept and the rest become one "other"
  group. Files directly in an expanded directory form a "files" group.
- **Counts.** Per part: files, symbols, languages, entry points, routes
  declared there, and route handlers defined there.
- **Links.** Resolved and heuristic `calls`, `instantiates`,
  `registers_route`, `configures`, `mounts`, `uses_middleware` and
  `loads_module` edges, counted between the parts of their source file and
  target symbol. Inside a zoomed-in part, links to the rest of the project
  go to an "outside" node.
- **Files.** With `direct_files`, or when the focus has no subdirectories,
  the result lists the files instead.

The CLI renderer (`cli/src/ui/graph.ts`) is a layered (Sugiyama-style)
layout in plain text: cycles are broken by depth-first search, parts are
layered by longest path over each part's significant links (its strongest,
or at least 5 % of its outgoing count), long links pass through dummy
positions, and four barycenter sweeps order each row. Rows wider than the
terminal wrap. Links run along horizontal buses in the gap between rows: a
link joins its target's bus when three or more links converge on that
target (and more than leave its source), otherwise its source's bus;
overlapping buses get separate lanes. Lines are
box-drawing characters (ASCII with `PROJ_ASCII=1`), with `▼` into a part and
`▲` for a link that goes back up a cycle. By default each part draws its
strongest link plus the selected part's top 6; `a` cycles to 3 per part or
all, and the details panel lists every link of the selected part.
`cli/src/ui/codemap.tsx` handles selection, zoom (Enter, Backspace) and the
details panel.

## Memory model

- One source file, one syntax tree and one file's facts in memory at a time.
- No AST or graph is retained across files. Resolution reads from SQLite.
- SQLite page cache: 16 MiB while building, 8 MiB while reading; temporary
  data spills to disk while building.
- Diagnostics: at most 2,000 warnings/errors and 500 debug/info entries
  stored per run (all counted).
- Queries carry hard caps: 500 results, traversal depth 25, 2,000 nodes,
  100 children per node.

- After `add_project` and `refresh_project`, the engine hands freed heap
  pages back to the operating system (`malloc_trim` on glibc). A long-lived
  `--serve` process therefore idles at its query footprint: 8.5 MiB after a
  Kafka refresh, instead of keeping its 51 MiB analysis peak.
- Every SQLite handle, prepared statement, Tree-sitter parser, tree and
  cursor is owned by an RAII wrapper, so error paths release them too.
  `engine/tests/test_memory.cpp` checks this (see DEVELOPMENT.md).

Measured peak RSS is in the README (35 MiB at 50,000 synthetic files).

## Engine protocol

The CLI starts `proj-engine` itself, without a shell. Arguments travel inside
the JSON document, so paths and queries are never interpreted as commands.

- One-shot: `proj-engine` reads one request from stdin and writes one
  response line to stdout (used by `add`, `log`, `show --no-tui`).
- Serve: `proj-engine --serve` reads newline-delimited requests and writes one
  response line per request (used by the interactive UI; one process per
  session). Read-only index connections are cached per project and dropped
  when that project is analyzed or refreshed.
- `proj-engine --version`; `proj-engine --dump-ast <language> <file>`
  (development aid).

stdout carries only protocol JSON. Diagnostics go to stderr.

Request:

```json
{"protocol": 1, "id": 7, "op": "search",
 "args": {"project": "my-server", "query": "getUser", "limit": 20},
 "options": {"data_dir": "/home/me/.local/share/proj", "log_level": "warn"}}
```

Response:

```json
{"protocol": 1, "id": 7, "op": "search", "status": "ok",
 "result": {"symbols": [...], "files": [...], "routes": [...]},
 "diagnostics": [], "error": null}
```

| `status` | Meaning | CLI exit code |
|---|---|---|
| `ok` | complete result | 0 |
| `partial` | result returned, but the analysis or check is incomplete | 3 (or 4 for no supported files) |
| `invalid_arguments` | bad or missing arguments, malformed request | 2 |
| `not_found` | unknown project, symbol, file, route, run, or missing index | 2 |
| `conflict` | duplicate name/path, analysis already running | 2 |
| `unsupported_operation` | unknown `op` | 1 |
| `index_error` | index corrupt, unreadable or from an incompatible schema | 1 |
| `analysis_failed` | analysis aborted; `error.details` says whether the previous index was kept | 1 |
| `internal_error` | unexpected engine error (also logged) | 1 |

Errors carry `error.code` (for example `project_exists`, `path_not_found`,
`index_missing`, `index_corrupt`) and a human-readable `error.message`.

Operations: `info`, `add_project`, `refresh_project` (optional
`max_affected_fraction` in (0, 1], default 0.4), `list_projects`, `project_status`,
`list_runs`, `get_run`, `summary`, `search`, `symbol`, `edges`, `traverse`,
`file`, `file_deps`, `entry_points`, `routes`, `route_flow`, `exceptions`,
`startup_path`, `overview`, `check_index`, `snippet`. Their arguments are validated in
`engine/src/protocol/protocol.cpp`.

The CLI treats a nonzero engine exit, a signal, invalid JSON, an envelope
with the wrong protocol version or unknown status, an empty response, and a
timeout as engine failures (exit code 1, recorded in `logs/proj.log`). The
UI's serve session restarts the engine after a crash or timeout and fails
only the requests sent to the dead process.

## Locating the engine

`--engine <path>`, then `$PROJ_ENGINE`, then `<repo>/build/engine/proj-engine`
(found relative to the CLI's own file, not the working directory), then
`proj-engine` on `PATH`. The engine has no runtime resources: the grammars are
compiled in. The `bin/proj` launcher sets `PROJ_ENGINE` to the
`bin/proj-engine` next to it, unless `PROJ_ENGINE` is already set.

## Logging

- Engine: `<timestamp> LEVEL [component] message` on stderr at the requested
  level (`--verbose` = info, `--debug` = debug, default warn). INFO and above
  are also appended to `logs/proj.log`.
- CLI: engine process failures and unexpected errors go to `logs/proj.log`
  (JSON lines).
- The log rotates at 1 MiB with three backups. Source contents are never
  logged.
- `proj log` lists runs (source-analysis diagnostics) separately from recent
  application warnings and errors.
