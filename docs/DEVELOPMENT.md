# Development

## Layout

```
bin/               proj (launcher) and proj-engine (prebuilt Linux x86_64 engine)
scripts/build.sh   one-step release build of the engine and the CLI
engine/            C++20 engine (CMake)
  cmake/Dependencies.cmake   pinned third-party sources (URL + SHA-256)
  src/                       see docs/ARCHITECTURE.md
  tests/                     Catch2 unit and integration tests
cli/               TypeScript CLI and Ink UI (npm, package-lock.json)
  src/  test/
fixtures/          small self-contained projects used by tests
  express-api/  python-app/  java-spring/  cpp-app/
  legacy-java/  legacy-python/  legacy-cpp/   (servlets, XML/YAML routes, dispatch, overloads)
bench/             synthetic repository generator and benchmark runner
docs/
build/             (generated) CMake build trees and downloaded sources
```

## Prerequisites

| Tool | Version used here | Notes |
|---|---|---|
| C++ compiler | GCC 15.3 | C++20; CI also builds with Clang (Ubuntu) and Apple Clang (macOS) |
| CMake | 3.31 (≥ 3.24 required) | |
| Node.js / npm | 22.22 / 10.9 (Node ≥ 20 required) | |
| network | first configure only | downloads pinned archives into `build/` |

Native dependencies (downloaded by CMake, verified by SHA-256, compiled into
the engine; nothing is installed system-wide):

| Dependency | Version |
|---|---|
| SQLite amalgamation | 3.50.4 (FTS5 enabled, extension loading disabled) |
| Tree-sitter runtime | 0.25.10 |
| tree-sitter-javascript / typescript / python / java / cpp / c | 0.25.0 / 0.23.2 / 0.25.0 / 0.23.5 / 0.23.4 / 0.24.1 |
| tree-sitter-xml / yaml (tree-sitter-grammars) | 0.7.0 / 0.7.2 |
| nlohmann/json | 3.11.3 |
| Catch2 (tests only) | 3.8.1 (amalgamated) |

Node dependencies are pinned exactly in `cli/package.json` and locked in
`cli/package-lock.json`: commander 12.1.0, ink 6.3.1, react 19.1.1, chalk
5.6.2; dev: typescript 5.9.3, @types/node 22.19.17, @types/react 19.1.17.

**Linux (Debian/Ubuntu):** `sudo apt install build-essential cmake nodejs npm`
(or Node from nodejs.org). **Fedora:** `sudo dnf install gcc-c++ cmake nodejs npm`.
**macOS:** `xcode-select --install`, then `brew install cmake node`.

## Build

For users, `scripts/build.sh` does everything. It writes a release engine
(without tests) to `build/release`, copies it stripped to `bin/proj-engine`,
and builds the CLI. `bin/proj` is the launcher (see the README). For
development, build the engine with its tests:

```sh
cmake -S engine -B build/engine -DCMAKE_BUILD_TYPE=Release
cmake --build build/engine --parallel
cd cli && npm ci && npm run build
```

The first build compiles the generated grammar parsers (several MB of C) and
takes a few minutes. Later builds are incremental.

Options: `-DPROJ_BUILD_TESTS=OFF` skips Catch2 and the test binary;
`-DPROJ_WARNINGS_AS_ERRORS=ON` adds `-Werror` (used in CI).

**Offline builds:** point FetchContent at extracted copies, for example
`-DFETCHCONTENT_SOURCE_DIR_PROJ_SQLITE=/path/to/sqlite-amalgamation-3500400`
(names: `PROJ_SQLITE`, `PROJ_JSON`, `PROJ_TREE_SITTER`, `PROJ_TS_JAVASCRIPT`,
`PROJ_TS_TYPESCRIPT`, `PROJ_TS_PYTHON`, `PROJ_TS_JAVA`, `PROJ_TS_CPP`,
`PROJ_TS_C`, `PROJ_CATCH2`), or reuse an existing download with
`-DFETCHCONTENT_BASE_DIR=$PWD/build/engine/_deps`.

## Test

```sh
# Engine: 72 Catch2 test cases (paths, ignore rules, scanner, extractors for all
# languages, schema/migrations, registry, end-to-end indexing of every fixture,
# queries, traversal limits, failure handling, protocol errors, call
# resolution, refresh equivalence, legacy routing and overloads, code map)
# plus 4 leak checks.
ctest --test-dir build/engine --output-on-failure  # both test groups
./build/engine/tests/proj_engine_tests "~[leak]"   # everything except the leak checks
./build/engine/tests/proj_engine_tests "[python]"  # by tag
./build/engine/tests/proj_engine_tests "[legacy]"  # configuration routes, code registration, dispatch, overloads

# CLI: 45 tests: unit, code map layout, engine-client (fake crashing/garbage/
# hanging engines), CLI integration (spawns the real `proj`), and interactive
# UI tests.
cd cli && npm test
```

The CLI tests need the engine built at `build/engine/proj-engine` (or
`PROJ_ENGINE`). Tests create their own temporary data directories and never
touch your real `~/.local/share/proj`.

Tests that rely on file permissions (unreadable directories, read-only index
directory) are skipped when running as root.

### Refresh equivalence

`proj refresh` must leave exactly the index a full analysis of the same files
would build. Targeted cases in `test_refresh.cpp` cover body edits, renames,
duplicate names, deleted files, routes and mounts, inheritance, exclusions,
and Python, Java and C++ specifics. A randomized case runs 40 rounds of
edits on a fixture copy: duplicating a file under a new name, deleting a
file, renaming an identifier, adding a caller of an existing name, and adding
a module that redefines an existing name. It runs once forcing selective
re-resolution and once with the default 40 % fallback threshold. After every
round it compares a canonical dump of the refreshed index with one from a
fresh analysis.

The same dump is available as a tool for real repositories. Run it on copies,
never on a repository you are working in:

```sh
proj refresh p                                     # updates <home>/indexes/project-N.db
PROJ_HOME=/tmp/ref proj add p /path/to/copy        # full analysis of the same files
build/engine/tests/proj_index_dump <home>/indexes/project-N.db > refreshed.txt
build/engine/tests/proj_index_dump /tmp/ref/indexes/project-1.db > full.txt
diff refreshed.txt full.txt && echo identical
```

The dump replaces row ids with stable keys (file path, qualified name, kind,
position), so two indexes compare equal when they describe the same facts.

### Memory and leak checks

`engine/tests/test_memory.cpp` (tag `[leak]`) measures allocator usage with
glibc's `mallinfo2()`, which covers C++ `new`, SQLite and Tree-sitter alike.
After a warm-up cycle it checks that:

- five complete lifecycles (analyze, edit, refresh, every query type,
  destroy the engine) return to the same byte count
- 6,000 queries on one long-lived engine allocate nothing permanently
- 10,000 parses with extraction, including a file with syntax errors, release
  every tree
- 2,000 tree walks aborted by an exception release their cursors (this
  caught a real leak, fixed with an RAII cursor guard)

The tolerance is 4 KiB. A leak of even one small allocation per file or query
exceeds it. glibc's per-thread cache keeps freed chunks that `mallinfo2()`
counts as in use, so the counts are exact only with
`GLIBC_TUNABLES=glibc.malloc.tcache_count=0`. The `proj_engine_leaks` ctest
entry sets it, and the tests skip without it (and on non-glibc platforms):

```sh
GLIBC_TUNABLES=glibc.malloc.tcache_count=0 ./build/engine/tests/proj_engine_tests "[leak]" -s
```

Long-running behavior was checked by hand on 2026-10-12, without valgrind or
ASan (neither is installed on the development machine):

- `proj-engine --serve` against the Kafka index for 30,000 mixed requests
  (search, symbol, edges, two traversals, exceptions, entry points, summary,
  route flow), sampling `/proc/<pid>/status`: VmRSS 17.0 MiB after warm-up
  and 17.0 MiB at the end.
- Three consecutive refreshes in the same session: RSS 8.5 MiB after each,
  with no growth.
- The CLI's `EngineSession` under `node --expose-gc`: heap flat across 20,000
  requests. Opening and closing sessions repeatedly grows the heap by about
  190 bytes per session, and the same loop written with bare
  `child_process.spawn` grows identically. The growth is in Node 22 itself,
  not in `proj`.

### Checked build

The sanitizer runtimes were not available on the development machine, so the
engine was verified with UBSan in trap mode plus libstdc++ assertions, which
need no runtime library:

```sh
cmake -S engine -B build/engine-checked -DCMAKE_BUILD_TYPE=Debug \
  "-DCMAKE_CXX_FLAGS=-D_GLIBCXX_ASSERTIONS -fsanitize=undefined -fsanitize-trap=all" \
  -DFETCHCONTENT_BASE_DIR=$PWD/build/engine/_deps
cmake --build build/engine-checked --parallel && ./build/engine-checked/tests/proj_engine_tests
```

Where `libasan`/`libubsan` are installed, use
`-fsanitize=address,undefined` instead.

## Benchmarks

```sh
node bench/generate.mjs /tmp/synthetic --files 5000 --seed 42   # inspect a generated tree
node bench/run.mjs --sizes 500,2000,8000,20000,50000 --queries 30 \
  --max-rss-mib 64 --out bench/results/<date>-<os>.json
```

`run.mjs` generates each size in a temporary directory (deleted afterwards
unless `--keep`), analyzes it, and times search, traversal and route-flow
queries over one `--serve` session. It prints a Markdown table and records the
OS, CPU, memory, Node, compiler and engine versions. `--max-rss-mib` fails the
run when peak analysis RSS exceeds the limit, and the runner also fails if a
traversal returns more nodes than requested. Together these detect memory
regressions and accidental unbounded graph materialization. CI runs it with a
generous limit.

## Debugging

- `proj --debug add ...` streams engine DEBUG logs (resolver phases, timings)
  to stderr.
- `build/engine/proj-engine --dump-ast typescript file.ts` prints the
  Tree-sitter tree, which is useful when extending an extractor.
- Talk to the engine directly:
  `echo '{"protocol":1,"id":1,"op":"search","args":{"project":"x","query":"getUser"},"options":{"data_dir":"/tmp/d"}}' | build/engine/proj-engine`
- Inspect an index read-only: `sqlite3 -readonly <data>/indexes/project-1.db`,
  for example `SELECT status, COUNT(*) FROM edges GROUP BY status;`.
- `proj show <name> --check` verifies index integrity.
- Application errors: `proj log`, or `<data>/logs/proj.log` (JSON lines).

## Extending

- **A new language:** add the grammar to `engine/cmake/Dependencies.cmake` and
  the extension mapping in `scan/language.cpp`. Write `parse/extract_<lang>.cpp`
  producing `FileFacts`, register it in `parse/parser.cpp`, and add
  dependency and call rules in `index/resolver.cpp` (the `family()` switch).
  Use `--dump-ast` to learn the node types, and add a fixture plus tests.
  Add refresh coverage by including the language in `test_refresh.cpp`.
- **A new framework:** emit `RouteFact`/`RouterFact`/`MountFact` from the
  language extractor. Prefix semantics live in
  `Resolver::compute_route_prefixes`.
- **Schema changes:** bump `kIndexSchemaVersion` for index changes (old
  indexes are refused and rebuilt by `--reanalyze`). For registry changes,
  append a migration to `kRegistryMigrations` and never edit released steps.
  Bump an extractor's `extractor_version` when its output changes. `proj
  refresh` then re-parses that language's files.
- **New resolver output:** a row the resolver creates must be marked
  `generated = 1` (or `origin_route_id` for route copies), and a column it
  overwrites needs a `raw_*` copy, so that refresh can undo resolution before
  re-resolving. `test_refresh.cpp` fails when this is missed.
