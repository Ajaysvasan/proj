# proj

`proj` is a local, static code-flow explorer for unfamiliar codebases. You
register a project directory once. `proj` then parses its sources with
Tree-sitter, builds a persistent SQLite index of symbols and relationships
(kept current with `proj refresh`), and opens a keyboard-driven terminal UI
for searching and navigating:

- files and their dependencies (imports, requires, includes), forward and reverse
- functions, methods, classes and other declarations, with exact source locations
- callers and callees as bounded, cycle-safe trees
- application entry points and how startup reaches route registration
- HTTP routes (`GET /api/users/:id`), their handler, middleware, mount prefix and call flow,
  including legacy servers whose endpoints are declared in XML/YAML (web.xml,
  Struts, Spring XML, OpenAPI, custom route files), registered in code without
  annotations, or dispatched by hand on the request path
- explicit `throw`/`raise` sites and recognized `catch`/`except` handlers
- analysis history, durations and diagnostics

Everything runs locally. There are no AI models, no network calls, no
telemetry, no daemon, and the analyzed project is never executed or modified.

Results are **statically discovered possibilities**, not traced runtime
behavior. Every relationship carries a resolution status (`resolved`,
`ambiguous`, `heuristic`, `external`, `unresolved`) and its source location, so
uncertain links are visible rather than hidden.

## Build

### Requirements

- A C++20 compiler: GCC or Clang (tested: GCC 15.3 locally; GCC, Clang and
  Apple Clang in CI)
- CMake 3.24 or newer
- Node.js 20 or newer, with npm
- Network access the first time you build. CMake downloads pinned,
  SHA-256-verified dependency sources (SQLite, Tree-sitter and its grammars,
  nlohmann/json) into `build/`, and npm installs the CLI's packages into
  `cli/node_modules`. Nothing is installed system-wide.

| System | Install the requirements |
|---|---|
| Fedora | `sudo dnf install gcc-c++ cmake nodejs npm` |
| Debian / Ubuntu | `sudo apt install build-essential cmake nodejs npm` (check that `node --version` is 20 or newer; otherwise install Node from nodejs.org) |
| macOS | `xcode-select --install`, then `brew install cmake node` |

### Build everything with one command

```sh
git clone https://github.com/Ajaysvasan/proj.git
cd proj
./scripts/build.sh
```

`scripts/build.sh` runs these steps:

1. Builds the engine in release mode (`build/release`).
2. Copies the binary to `bin/proj-engine`.
3. Installs the CLI's dependencies (`npm ci`) and compiles it (`cli/dist`).

The first build takes a few minutes because it compiles the language
grammars. Later builds are incremental.

### Prebuilt engine

The repository includes a prebuilt `bin/proj-engine` for **Linux x86_64**,
built on Fedora 43 with GCC 15.3. It needs glibc 2.38 or newer and the C++
runtime of GCC 13 or newer (for example Ubuntu 24.04+, Debian 13+ or
Fedora 39+); so far it has been run only on Fedora 43. If it runs on your
system (`bin/proj-engine --version`), you only need to build the CLI:

```sh
cd cli && npm ci && npm run build
```

On macOS, on other architectures or on older Linux distributions, run
`./scripts/build.sh`, which replaces `bin/proj-engine` with a build for your
machine.

## Install: use `proj` from any directory

`bin/proj` is a small launcher. It runs the CLI from this checkout with the
engine in `bin/`. Make it reachable from anywhere in one of two ways:

```sh
# Option 1: add bin/ to PATH (put this line in ~/.bashrc or ~/.zshrc)
export PATH="/path/to/proj/bin:$PATH"

# Option 2: symlink the launcher into a directory that is already on PATH
ln -s "$PWD/bin/proj" ~/.local/bin/proj
```

Then open a new terminal and check it with `proj --version`. Keep the checkout
where it is: the launcher uses `cli/` and `bin/` from it. After pulling
changes, run `./scripts/build.sh` again.

## Run

```sh
proj add my-server ~/projects/server   # register a project and analyze it
proj show my-server                    # code map + search in the terminal UI
proj show my-server GET /api/users/42  # jump straight to an API endpoint (--no-tui prints its flow)
proj refresh my-server                 # after editing files: re-parses only what changed
proj log                               # analysis history and diagnostics
proj --help                            # all commands and options
```

`proj add` prints an analysis summary:

```
Analysis complete with problems (partial)
  Project            express
  Status             partial
  Duration           13 ms
  Files              30 discovered, 25 parsed, 1 with syntax errors, 0 failed, 0 skipped, 3 unsupported, 1 excluded
  Symbols            71
  Relationships      152 (79 calls, 32 file dependencies)
  Call resolution    33 resolved, 24 external, 1 ambiguous, 0 heuristic, 21 unresolved
  Routes             7
  ...
```

`proj show` opens on a **code map**: the project's main parts drawn as boxes
joined by arrows (calls, route registrations, configuration wiring), from
where requests and startup come in down to the code they reach. Arrow keys
select a part, Enter zooms in, Backspace zooms out, `a` shows more links.
Type `/show GET /api/users/42` to open an API endpoint. Press `/` and type `getUser`, `UserService.find`,
`routes/users` or `GET /api/users/:id` (concrete URLs such as
`/api/users/42` match parameterized routes). Press Enter to open a result,
Tab to switch between callees, callers, exceptions and source, and `?` for all
shortcuts.

## Commands

| Command | Purpose |
|---|---|
| `proj add <name> <path>` | Register and analyze a project. `--reanalyze` re-indexes a registered project. |
| `proj refresh <name>` | Update the index after files changed: re-parses changed and added files, drops deleted ones, re-resolves affected relationships. The result equals a full re-analysis. |
| `proj log [project]` | List analysis runs; `--run <id>` shows one run with its diagnostics. |
| `proj show <name>` | Interactive explorer. Falls back to a text summary without a TTY; `--check` verifies index integrity. |

Exit codes: `0` success, `1` internal failure, `2` user error, `3` partial
analysis, `4` no supported source files, `130` interrupted. See
[docs/CLI_REFERENCE.md](docs/CLI_REFERENCE.md) for every flag and key binding.

## Language support (summary)

| Language | Declarations | Imports | Calls | Routes | Exceptions | Entry points |
|---|---|---|---|---|---|---|
| JavaScript / TypeScript / TSX | yes | ESM, CommonJS | yes | Express-style, NestJS | throw / try | package.json, `listen()` |
| Python | yes | absolute, relative, star | yes | Flask, FastAPI, Django (incl. `include()`, class-based views), Tornado, webapp2, web.py, Falcon, Pyramid, Bottle, `http.server`, path dispatch | raise / except | `__main__` guard, `__main__.py`, `run()` |
| Java | yes | single, wildcard, static | yes: overloads by argument types, chained calls, static imports | Spring MVC, JAX-RS, servlets (web.xml, `@WebServlet`, `addServlet`), JDK `HttpServer`, Spark/Javalin, Struts, Spring XML, front-controller dispatch | throw / catch / throws | `main`, `@SpringBootApplication`, `web.xml`, `@WebListener` |
| C / C++ | yes | `#include` | yes, overloads by argument types | cpp-httplib, Crow, Drogon, Pistache, registration calls, route tables, path dispatch | throw / catch (C++) | `main` |
| XML / YAML (configuration) | Spring beans | | `configures` edges to wired classes | web.xml, Struts 1/2, Spring XML, OpenAPI, serverless.yml, custom path + handler entries | | `web.xml`, route files |

Coverage is partial by design. The detailed constructs and known limitations
are listed in [docs/LANGUAGE_SUPPORT.md](docs/LANGUAGE_SUPPORT.md).

## Legacy servers

Older back ends rarely use annotation-based frameworks. `proj` also follows:

- **Endpoints declared in configuration.** A `web.xml` servlet mapping
  becomes one route per HTTP method its class implements (`doGet`,
  `doPost`, ..., inherited ones included, or `service`), with the mapped
  filters as middleware in declaration order. Struts actions, Spring XML
  handler mappings, OpenAPI `operationId`s, `serverless.yml` functions and
  custom XML/YAML entries that pair a URL with a handler class or function
  work the same way. A class named in a configuration file is found by its
  qualified name, and the file links to the classes it wires (`configures`
  edges). `web.xml` is an entry point, so startup code such as a listener's
  `contextInitialized` traces back to it.
- **Endpoints registered in code without annotations.** Servlet containers
  (`addServlet`/`addMapping`, Jetty, Tomcat), the JDK `HttpServer`, Spark and
  Javalin, Tornado/webapp2/web.py/Falcon/Pyramid/Bottle handler lists and
  classes, Django class-based views and `include()`, C/C++ registration
  calls and route tables. Handler classes are dispatched to their request
  methods; handler lambdas become symbols that own their calls.
- **Hand-written dispatchers.** `if`/`switch`/`match` branches that compare
  the request path with a literal (`"/users".equals(path)`,
  `self.path == "/pets"`, `strcmp(uri, "/health") == 0`) are `heuristic`
  routes to the call that handles the branch.
- **Overloads.** Java and C++ overloads with the same number of parameters
  are told apart by the static types of the arguments (literals, `new X()`,
  casts, declared variables), so `find(42L)` and `find("bob")` reach different
  methods.
- **Wildcard route search.** `/api/users/42/orders` finds the servlet mapping
  `/api/users/*`, and `/login.do` finds `*.do`.

## Results on real repositories

Measured on 2026-10-12 on the machine described below. The repositories were
only read; refresh experiments ran on copies.

| Repository | Source files | Analysis | Peak RSS | Call sites | Unresolved | Routes | Entry points |
|---|---|---:|---:|---:|---:|---:|---:|
| Apache Kafka | 6,190 Java, 184 Python, 106 XML/YAML | 14.7 s (see below) | 54.6 MiB | 802,087 | 7.4 % | 49 | 169 |
| A FastAPI service backend | 115 Python | 0.1 s | 14.2 MiB | 2,823 | 30.8 % | 5 (FastAPI) | 22 |

On Kafka, 91 % of call sites resolve to a project method or are identified as
external (JDK or library), and 1.5 % are ambiguous with all candidates listed;
choosing overloads by argument types took ambiguous calls from 30,045 to
12,150. Kafka's 49 routes are its 45 JAX-RS resources, the Jersey containers
that Kafka Connect and Trogdor register with Jetty at `/*` (3, external), and
one `http.server` handler in the system tests. Its 106 XML/YAML files
(build, CI and test configuration) produce no false routes. Python has fewer
declared types, so more calls on unannotated parameters stay unresolved.
Each one carries a reason.

The 14.7 s analysis time was measured in the morning with the previous engine.
Later the same day the machine ran about twice as slow; measured back to back
then, the current engine took 33 s and the previous one 31 s on Kafka (about
7 % for argument types and overload scoring), and 14.43 s versus 14.23 s on
20,000 synthetic files (about 1.4 %, +1 MiB peak memory).

`proj refresh` on a Kafka copy took 2.2 s with no changes and 2.6–4.8 s after
a method-body edit, a rename plus a new overload plus a duplicate class name,
a class-hierarchy change, a return-type change, or a deleted file. Full
analyses in the same runs took 15–26 s. In every scenario the refreshed index
was identical to a fresh full analysis (compared with `proj_index_dump`; see
[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)).

## Where data is stored

The index is never written inside the analyzed repository. The data directory
holds `registry.db` (projects and analysis history), `indexes/project-<id>.db`
(one SQLite index per project) and `logs/proj.log` (rotated application log):

- `--data-dir <dir>` or `$PROJ_HOME`, if set
- Linux: `$XDG_DATA_HOME/proj`, defaulting to `~/.local/share/proj`
- macOS: `~/Library/Application Support/proj`

To inspect an index safely, open a copy or use read-only mode:
`sqlite3 -readonly ~/.local/share/proj/indexes/project-1.db`. The schema is
documented in [docs/INDEX_SCHEMA.md](docs/INDEX_SCHEMA.md).

There is no `proj remove` command yet. Until one exists, a project can be
removed manually without touching its source repository: delete its row from
`registry.db` (`DELETE FROM projects WHERE name = '...'`, plus its
`analysis_runs` and `run_diagnostics` rows), then delete the index file it
pointed to under `indexes/`. Never delete the project's own directory.

## Performance and memory (measured)

Measured on Linux x86_64 (Intel Core Ultra 7 255HX, 20 threads, 15 GiB RAM,
GCC 15.3, release build) with synthetic repositories from
`bench/generate.mjs` (seed 42). Raw data:
[bench/results/2026-10-12-linux-x86_64.json](bench/results/2026-10-12-linux-x86_64.json).

| Files | Analysis | Peak RSS | Index | Symbols | Edges | Search median / p95 | Traverse depth 6 median / p95 |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 502 | 0.16 s | 15.6 MiB | 3.9 MiB | 4,482 | 8,991 | 0.27 / 0.62 ms | 1.84 / 2.06 ms |
| 2,002 | 0.63 s | 31.6 MiB | 15 MiB | 17,952 | 35,970 | 0.63 / 1.48 ms | 1.80 / 2.36 ms |
| 8,002 | 2.61 s | 33.1 MiB | 62.9 MiB | 71,832 | 143,931 | 1.44 / 1.54 ms | 3.10 / 5.79 ms |
| 20,002 | 6.76 s | 34.4 MiB | 158 MiB | 179,592 | 359,631 | 2.44 / 4.91 ms | 2.70 / 3.41 ms |
| 50,002 | 17.31 s | 35.0 MiB | 400 MiB | 448,992 | 899,023 | 5.68 / 9.13 ms | 2.84 / 3.48 ms |

Peak memory stays roughly flat as the repository grows, because files are
parsed one at a time, syntax trees are discarded after extraction, and
relationships are resolved through indexed SQL lookups with bounded caches.
Leak checks: repeated analyze/refresh/query cycles return the allocator to
the same byte count, and a 30,000-request `--serve` session on the Kafka index
stays at 17 MiB RSS (see [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md#memory-and-leak-checks)).
These numbers come from synthetic code and one machine; real repositories with
very large files or very large single directories will differ. Performance
on macOS has not been measured; CI only checks that peak memory stays under
128 MiB there.

## Platform status

- **Linux (Fedora 43, x86_64, GCC 15.3, Node 22):** built and fully tested
  (engine: 75 test cases plus 4 leak checks; CLI: 45 tests).
- **CI (GitHub Actions; `.github/workflows/ci.yml`):** on Ubuntu with GCC,
  Ubuntu with Clang, and macOS with Apple Clang, every push builds the engine
  with warnings as errors and runs the engine tests, the CLI tests, the
  benchmark memory check, and `scripts/build.sh` with the `bin/proj`
  launcher. All jobs pass. The leak checks need glibc, so they are skipped on
  macOS.

## Documentation

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): components, data flow, engine protocol
- [docs/INDEX_SCHEMA.md](docs/INDEX_SCHEMA.md): database schema and relationship semantics
- [docs/LANGUAGE_SUPPORT.md](docs/LANGUAGE_SUPPORT.md): extracted constructs and limitations
- [docs/CLI_REFERENCE.md](docs/CLI_REFERENCE.md): commands, flags, keys, exit codes
- [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md): building, testing, debugging, benchmarks
- [docs/IMPLEMENTATION_PLAN.md](docs/IMPLEMENTATION_PLAN.md): milestones and status
