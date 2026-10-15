# AGENT.md: notes for agents working on this repository

## What this is
`proj` is a local static code-flow explorer: `proj add <name> <path>` indexes a
project, `proj refresh <name>` updates the index incrementally, `proj show
<name>` explores it in a terminal UI, and `proj log` shows analysis history. The original specification is `Prompt.md`, kept locally
and excluded from Git (`.gitignore`). Current status is in
`docs/IMPLEMENTATION_PLAN.md`.

## Hard rules (from the specification)
- No AI/LLM/embedding functionality or dependencies, no network calls, no
  telemetry. Analyzed code is never executed, modified or uploaded.
- The C++ engine owns all parsing, indexing, resolution and graph queries. The
  TypeScript CLI must not parse source code or rebuild the graph.
- Never present guesses as facts: every relationship keeps a status
  (`resolved`, `ambiguous`, `heuristic`, `external`, `unresolved`) and a source
  location.
- Keep memory bounded: one file at a time, no retained ASTs, resolution via
  indexed SQL, capped caches and query limits. Measure (`bench/run.mjs`) before
  and after performance changes.
- Don't claim platform support, test results or performance that you have not
  observed. Clang and macOS are verified only through CI (GitHub Actions),
  where the glibc-based leak checks are skipped on macOS.
- No destructive commands against the user's files. Tests only delete temporary
  directories they created themselves.

## Build and test
`scripts/build.sh` builds a release engine into `bin/proj-engine` and the CLI.
`bin/proj` is the launcher users put on PATH. `bin/proj-engine` is committed
as a prebuilt Linux x86_64 binary, so rebuild and commit it when engine
behavior changes.

```sh
cmake -S engine -B build/engine -DCMAKE_BUILD_TYPE=Release && cmake --build build/engine --parallel
ctest --test-dir build/engine --output-on-failure   # 75 test cases + 4 leak checks
cd cli && npm ci && npm test                  # 45 tests (unit, graph, client, CLI, UI)
node bench/run.mjs --sizes 500,2000 --queries 10 --max-rss-mib 64
```

## Map
- Engine entry and protocol: `engine/src/main.cpp`, `engine/src/protocol/`
- Pipeline: `scan/` → `parse/` (extractors) → `index/store.cpp` →
  `index/resolver.cpp` → `index/indexer.cpp`; refresh: `index/refresh.cpp`
- Index comparison: `engine/tests/index_dump.*` and the `proj_index_dump`
  tool (canonical, id-independent dump)
- Queries: `engine/src/query/query.cpp`; schema: `engine/src/db/schema.cpp`
  (documented in `docs/INDEX_SCHEMA.md`)
- CLI: `cli/src/index.ts` (commands), `cli/src/engine/client.ts` (process
  handling), `cli/src/ui/` (Ink)
- Fixtures with known expected results: `fixtures/*` (tests assert facts
  derived from these sources; keep them in sync when editing)

## Pitfalls already hit (don't reintroduce)
- References into `FileFacts::symbols` dangle after `add_symbol` reallocates.
  Copy `kind`/`name` strings before adding symbols.
- In C++20, `for (x : f()["key"])` iterates a destroyed temporary. Store the
  result first.
- Ink re-subscribes `useInput` handlers after paint. Read state through
  `useLatest` refs, and update mode flags (search focus) synchronously via refs.
- Ink's `waitUntilExit()` promise is created lazily. Request it immediately
  after `render()`.
- `EngineSession` must only fail requests owned by the process that died
  (restart races).
- Tree-sitter: on `export class`, class decorators belong to the
  `export_statement`. Method decorators are preceding siblings in `class_body`.
- `proj refresh` must produce exactly the index a full analysis produces.
  `test_refresh.cpp` checks this with randomized edits. A new resolver output
  must be marked `generated = 1` or keep its pre-resolution value in a `raw_*`
  column, or refresh cannot reset it. A changed extractor output needs a bumped
  `extractor_version`, so refresh re-parses those files.
- Resolver lookups go through `cached(sql)`. Preparing statements per call
  made Kafka's resolution phase about 2x slower.
- Finalization statistics must not use `GROUP BY` over `edges`. SQLite's
  sorter buffers up to the page-cache size, and peak RSS then grew with the
  repository (52 MiB at 50k files instead of 35 MiB).
- Legacy routing (`[legacy]` tests, `fixtures/legacy-*`): configuration
  routes and code registrations name handlers by reference; the resolver
  dispatches handler classes per framework (`resolver_config.cpp`) and may
  turn one route into one row per HTTP method. Anything it changes on a
  route must be restorable by refresh (`raw_method`, `origin_route_id`).
- Dispatch heuristics (`route_util.hpp`) must stay conservative: the
  condition must mention the request, literals must name a resource, and the
  branch must contain a handler call. Check new patterns against Kafka for
  false routes.
- `for (x : reader.search(...)["routes"])` iterates a destroyed temporary
  (it bit the legacy tests). Store the json first.
- `mallinfo2()` counts glibc tcache chunks as in use. The leak tests need
  `GLIBC_TUNABLES=glibc.malloc.tcache_count=0`, which ctest sets.

## Open work
See "Not implemented" and "Next milestone" in `docs/IMPLEMENTATION_PLAN.md`.
