# CLI reference

```
proj [global options] <command> [options]
```

## Global options

| Option | Meaning |
|---|---|
| `--data-dir <dir>` | Data directory (registry, indexes, logs). Default: `$PROJ_HOME`, else `~/.local/share/proj` (`$XDG_DATA_HOME/proj` if set) on Linux, `~/Library/Application Support/proj` on macOS |
| `--engine <path>` | `proj-engine` binary. Default: `$PROJ_ENGINE` (the `bin/proj` launcher sets it to `bin/proj-engine`), else `<repo>/build/engine/proj-engine`, else `PATH` |
| `-v, --verbose` | Stream engine progress (INFO) to stderr |
| `--debug` | Stream DEBUG engine logging to stderr |
| `-q, --quiet` | Print nothing on success; use the exit code |
| `-V, --version`, `-h, --help` | Version / help |

Environment: `PROJ_HOME`, `PROJ_ENGINE`, `NO_COLOR` (disables colors),
`PROJ_ASCII=1` (ASCII tree glyphs; also automatic for `TERM=dumb` and
non-UTF-8 locales).

## proj add

```
proj add <name> <path> [--reanalyze] [--exclude <pattern>]... [--include-default <name>]...
         [--no-default-excludes] [--no-gitignore] [--max-file-size <size>] [--max-files <n>] [--json]
```

Registers `<path>` (resolved to its canonical absolute path) under `<name>`
(1–64 characters: letters, digits, `.`, `_`, `-`, starting with a letter or
digit) and runs a full analysis.

- Rejects a name that is already registered unless `--reanalyze` is given.
  Rejects a path registered under another name, a missing or unreadable
  directory, and a file path.
- `--reanalyze` re-indexes a registered project, reusing its stored options
  unless new ones are given.
- `--exclude` takes `.gitignore` syntax (`dir/`, `*.gen.ts`, `/root-only`,
  `!negation`), repeatable.
- `--include-default vendor` scans a directory excluded by default.
  `--no-default-excludes` disables the built-in list.
- `--no-gitignore` ignores `.gitignore` and `.git/info/exclude`.
  `.projignore` in the project root is always applied. When the project is a
  subdirectory of a Git work tree (for example `repo/backend`), the
  `.gitignore` files of the enclosing directories up to the repository root
  and the repository's `.git/info/exclude` also apply, as they do for Git.
- `--max-file-size 2m` (bytes or `k`/`m` suffix; default 1 MiB, range 1 KiB to
  64 MiB). `--max-files` (default 200,000).
- `--json` prints the engine's summary as JSON on stdout.

Default exclusions: `.git .hg .svn node_modules bower_components
jspm_packages vendor dist build out target .venv venv __pycache__ .mypy_cache
.pytest_cache .ruff_cache .tox .nox .eggs site-packages .gradle .idea .vscode
.next .nuxt .svelte-kit .parcel-cache .turbo .cache coverage .nyc_output
.terraform Pods DerivedData cmake-build-* *.egg-info .angular .expo
.dart_tool`, and files `*.min.js *.min.mjs *.bundle.js *.chunk.js *.map *.pyc
*.class *.o *.obj *.so *.dylib *.a`.

The summary shows status, duration, file counts (discovered, parsed, with
syntax errors, failed, skipped, unsupported, excluded), configuration files
(XML/YAML parsed, with syntax errors, skipped; these never make an analysis
partial), languages, symbols,
relationships, call-resolution breakdown, routes, entry points, exception
sites, unresolved references, parse problems, peak memory, index path and the
first diagnostics.

## proj refresh

```
proj refresh <project> [--json]
```

Brings the index of a registered project up to date after files were edited,
added, deleted or renamed, without analyzing everything again.

- Scans the project again with the options stored by the last `proj add`.
- Reads and fingerprints every source file (FNV-1a 64 and size). It re-parses
  only new files and files whose fingerprint, language or extractor version
  changed. Modification times are not trusted: touching a file causes no work,
  and an edit that keeps the timestamp is still detected.
- Removes the facts of deleted files. A symbol whose identity (kind, qualified
  name, parameter count, position among same-named symbols) is unchanged
  keeps its id, so relationships from untouched files to it stay valid.
- Re-resolves only relationships that could now resolve differently:
  - calls and references naming a symbol that was added, removed, renamed or
    changed signature, including subclasses of a changed class, functions
    returning a changed type, and calls chained on those
  - files whose declared types name such a symbol
  - files that import a file whose exported interface changed
  - files whose import resolution changed
  Route, mount, middleware, override and module-load rows are regenerated. When
  more than 40 % of the source files are affected, all relationships are
  re-resolved instead, which is then just as fast.
- Like `add`, it works on a private copy and replaces the index atomically. If
  it fails, the previous index is kept.
- The result is identical to what `proj add <name> <path> --reanalyze` would
  produce. Tests and the `proj_index_dump` tool verify this
  ([DEVELOPMENT.md](DEVELOPMENT.md)).

The summary is the `add` summary plus two lines:

```
Refresh complete with problems (partial)
  ...
  Changes            1 changed, 1 added, 1 removed, 119 unchanged (re-parsed only changed and added files)
  Re-resolved        642 relationships in 16 affected files
```

Errors: an unknown project, a project directory that no longer exists, or a
project without an index (`proj add <name> <path> --reanalyze` creates one)
exit with 2. An index from another schema version also needs `--reanalyze`,
because a refresh cannot upgrade it (exit 1). A refresh is refused while
another analysis of the same project is running.

## proj log

```
proj log [project] [--limit <n>] [--json]
proj log [project] --run <id> [--diagnostics <n>] [--json]
```

Lists recent runs (id, project, start time, duration, status, parsed/discovered
files, symbols, relationships, unresolved references, parse errors, warnings),
followed by the reasons for failed or interrupted runs and the latest
application warnings and errors from `logs/proj.log`.

`--run <id>` shows one run: timestamps, phase timings, tool and schema version,
counts, peak memory, error message, and its diagnostics. Internal
(application) problems are listed separately from source-analysis diagnostics
(syntax errors, skipped files, unsupported constructs, exclusions). Only a
bounded number of diagnostics is stored per run; the output says how many
were dropped.

## proj show

```
proj show <name> [endpoint...] [--no-tui] [--check]
```

Opens the interactive explorer. It first verifies that the project is
registered and that its index exists and is readable; otherwise it explains
how to fix it (for example `proj add <name> <path> --reanalyze`). Without a
TTY on stdin and stdout, or with `--no-tui`, it prints a text overview:
counts, languages, entry points and routes (each with the method that handles
it).

With an endpoint (`proj show api GET /api/users/42`, `proj show api
/login.do`), the explorer opens directly on the best-matching route; Esc lists
every match. With `--no-tui`, the route is printed instead: registration,
handler, middleware or filters, startup path, the call tree from the handler
(depth 4), and the other matches. An endpoint that matches nothing exits with
2.

`--check` runs the index integrity check and exits 0 if the index is
consistent, 1 otherwise.

### Screens

- **Home / code map**: while the search box is empty, the home screen draws
  the project's main parts as boxes joined by arrows. A part is a directory
  group (single-directory chains such as `src/main/java/com/acme` are
  collapsed; small directories are grouped into `other` when space runs
  out), and an arrow means calls, route registrations, configuration wiring,
  mounts or module loads from one part to another. Parts with routes or entry
  points are drawn at the top and in green, so the picture reads from where
  requests and startup come in down to the code they reach. Arrow keys select
  a part; the panel below shows its files, symbols, languages, entry points,
  routes, and its strongest links in both directions. Enter zooms into the part
  (or lists its files, which Enter opens); Backspace or Esc zooms out; `a`
  switches between the strongest link per part, the top 3, and all links (the
  selected part always shows its strongest links). Cycles are drawn as an
  upward arrow (▲).
- **Search**: case-insensitive substring search over symbols (name and
  qualified name), file paths and routes, run 120 ms after typing stops (at most
  15 results per kind on screen). Exact name matches rank first, then names
  starting with the query, then shorter names. Queries of one or two characters
  match symbol-name prefixes only. `GET /api/users/:id`, `/api/users/42` and
  `users` all match routes; parameters (`:id`, `{id}`, `<int:id>`) match any
  segment, servlet wildcards match any number of segments
  (`/api/users/42/orders` finds `/api/users/*`) and extension mappings match
  by suffix (`/login.do` finds `*.do`). A query that starts with an HTTP method word (`get`, `post`,
  `delete`, ...) searches routes only. `/show <endpoint>` (also typed as
  `show <endpoint>`) lists API endpoints only; Enter opens the route. Results
  show kind, name, `path:line` and the qualified name, so duplicate names are
  distinguishable; routes show the method that handles them
  (`UserServlet.doGet`).
- **Symbol**: location, parent, visibility, counts of outgoing and incoming
  relationships by status, handled routes, entry-point role. Tabs:
  *Callees* (forward tree), *Callers* (backward tree), *Exceptions* (explicit
  sites, local handlers, possible sites in callees), *Source* (snippet read
  from disk; warns if the file changed since indexing).
- **File**: language, parse status, syntax errors. Tabs: *Symbols*,
  *Dependencies* (local, external, unresolved), *Dependents*.
- **Route**: method and full path, registration site, mount prefix and site,
  handler (or why it is unresolved), middleware chain, startup path from an
  entry point, and the call flow from the handler (tabs: *Flow*, *Callers*,
  *Exceptions*).
- **Entry points**, **Routes**, **Status and limitations**.
- **Errors** (`x`, the Error Tracer) and **Traces** (`t`): what runtime tracing
  recorded (see `proj errors`, `proj traces`). An error shows its state, throw
  site, handler, stack (most recent call first; Enter on a frame opens its
  method) and causes; a trace shows the observed call tree with each call's
  evidence.

Trees are bounded (depth adjustable, at most 300 nodes per view). A node
reached a second time shows `shown above` instead of expanding again, so
cycles terminate. Edge markers: `[unresolved: reason]`, `[ambiguous:
candidates]`, `[heuristic]`, `[external]`; `xN` means N call sites. With
runtime tracing data: `[ran N×]` the call was observed N times (`as X` when the
static edge was an interface or ambiguous call and X ran), `[runtime only]` no
static call site accounts for it, `[runtime only, not in the index]` the
callee is code proj did not analyze.

### Keys

| Key | Action |
|---|---|
| `/` | focus the search box (home); type `/show GET /api/users/42` to open an endpoint |
| arrows | select a part of the code map (home, empty search) |
| Enter | zoom into the selected part or list its files (code map) |
| Backspace, Esc | zoom out (code map) |
| `a` | links drawn on the code map: strongest per part, top 3, all |
| typing, Backspace, Ctrl+U | edit / clear the query |
| Esc | leave the search box; elsewhere go back |
| Backspace, `b` | go back (when not typing) |
| Up / Down | move the selection |
| Enter | open the selected result or tree node |
| Tab, Right / Left | next / previous tab |
| `+` / `-` | increase / decrease traversal depth |
| `u` | show or hide unresolved and external edges |
| `f` | open the symbol's file |
| `o` | open the route handler |
| `e`, `r`, `s` | entry points, routes, status (home) |
| `x`, `t` | Error Tracer, observed traces (home; need runtime tracing data) |
| `h` | home |
| `?` | help |
| `q`, Ctrl+C | quit (`q` is typed normally inside the search box) |

The terminal is restored on exit, Ctrl+C, SIGTERM and SIGHUP, and the engine
session process is closed.

## proj start dynamic / proj stop dynamic / proj status dynamic

Runtime tracing of Java applications (see [DYNAMIC_TRACING.md](DYNAMIC_TRACING.md)).

```
proj start dynamic [project] [--pid <pid> | --match <text>] [--include <package>]... [--json]
proj stop dynamic [project] [--json]
proj status dynamic [project] [--json]
```

`start` starts the service if needed (a background `proj-engine
--dynamic-service`) and monitors the project, or every project configured by
an earlier start. It prints the `-javaagent:` option that records a JVM from
its startup. `--pid` attaches to that running JVM of yours; `--match` attaches
to JVMs of yours whose command line contains the text, as they appear (both
record only from the attach on). `--include` replaces the application
packages found in the index. Starting a monitored project again changes
nothing.

`stop` detaches the agents (they restore the original classes) and stops
monitoring; the applications keep running. The service exits when nothing is
monitored. Stopping when nothing runs is not an error.

`status` lists each monitored project's state (`IDLE`, `TARGET_DISCOVERED`,
`ATTACHING`, `ATTACHED`, `CAPTURING`, `FLUSHING`), connected JVMs with their
startup coverage, the agent option and the recent transitions.

Needs a JDK for building the agent (`bin/proj-agent.jar`); the agent finds the
service through `<data_dir>/dynamic/service.json`. Environment: `PROJ_AGENT_JAR`
(agent location), `PROJ_JAVA` or `JAVA_HOME` (the JDK used to attach).

## proj errors

```
proj errors <project> [id] [--limit <n>] [--json]
```

The Error Tracer. Without an id: exceptions observed at runtime, most recently
seen first, with their state (`CAUGHT`, `WRAPPED`, `ESCAPED`, `UNCAUGHT`,
`UNKNOWN`), the most recent application frame and the occurrence count. With
an id: the exception, its state, throw site, most recent application frame and
handler, the stack from the most recent call to the oldest (each frame with
its file and line, and its project file when it is application code), causes,
and the traces it occurred in. Works without the service.

## proj traces

```
proj traces <project> [id] [--limit <n>] [--json]
```

Execution traces observed at runtime, newest first: requests (`GET /users/42`),
startup (`main`), tasks. With an id: the call tree in order of first calls,
each call placed in the static index with its evidence (`[static: inferred]`,
`[runtime only]`, `[not in the index]`). Works without the service.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | success (for `add`: analysis complete with no problems) |
| 1 | internal failure: engine crash/timeout/malformed output, failed analysis, corrupt or incompatible index, SQLite or I/O error, failed integrity check |
| 2 | user error: invalid arguments, unknown command or project, invalid or vanished path, missing index, duplicate registration, analysis already running |
| 3 | `add`, `refresh`: analysis completed but is partial (syntax errors, failed/skipped/unreadable files, limits reached) |
| 4 | `add`, `refresh`: no files in a supported language were found |
| 130 | interrupted (Ctrl+C) |

## Examples

```sh
proj add api ~/src/api --exclude 'fixtures/' --exclude '*.generated.ts'
proj add api ~/src/api --reanalyze
proj refresh api
proj refresh api --json | jq .stats.refresh
proj -v add mono ~/src/monorepo --include-default vendor --max-file-size 2m
proj log
proj log api --run 12
proj show api
proj show api GET /api/users/42
proj show api POST /api/orders --no-tui
proj show api --check
PROJ_HOME=/tmp/proj-scratch proj add demo ./demo --json | jq .counts
```
