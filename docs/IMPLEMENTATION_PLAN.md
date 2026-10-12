# Implementation plan and status

Legend: `[x]` done and tested in this repository · `[~]` partial (see note) · `[ ]` not done

Architecture: `proj` (Node.js/TypeScript CLI + Ink UI) → JSON over
stdin/stdout → `proj-engine` (C++20, Tree-sitter, SQLite) → per-project index
plus shared registry in the user data directory. Details:
[ARCHITECTURE.md](ARCHITECTURE.md).

## Phase 1: Foundation
- [x] CMake C++20 project; third-party sources pinned by URL and SHA-256 (FetchContent, build-tree only)
- [x] TypeScript CLI (Commander, Ink, Chalk) with exact versions and lockfile
- [x] Versioned JSON protocol; one-shot and `--serve` modes; distinct statuses
- [x] Registry with migrations; analysis runs; bounded diagnostics
- [x] `proj add`, `proj log`, `proj show`; exit-code convention
- [x] Smoke and protocol tests

## Phase 2: File scanning and persistence
- [x] Traversal without following symlinks; depth and file-count limits; unreadable entries reported
- [x] Default exclusions, `.gitignore` (nested), `.git/info/exclude`, `.projignore`, `--exclude`, overrides
- [x] Language by extension; binary detection; size limit; FNV-1a fingerprint; metadata
- [x] Nine file statuses distinguishing parsed / syntax errors / failed / unsupported / excluded / too large / binary / unreadable / symlink

## Phase 3: Parsing and symbol indexing
- [x] Tree-sitter runtime + JS, TS, TSX, Python, Java, C, C++ grammars
- [x] Parser manager: one reusable parser, per-file time limit, tree released per file, iterative depth-limited walk
- [x] JavaScript/TypeScript extractor
- [x] Symbol persistence; exact, prefix and trigram substring search; duplicate names disambiguated

## Phase 4: Dependency and call graphs
- [x] File dependency resolution for all languages with resolved/heuristic/external/unresolved/ambiguous/package statuses
- [x] Call extraction and resolution (scope, imports, exports, declared types, inheritance, overloads)
- [x] Ambiguity candidates; name-only heuristics labeled; reasons recorded for unresolved edges
- [x] Callers/callees; bounded, cycle-safe BFS traversal

## Phase 5: Startup, APIs, exceptions
- [x] Entry points: `main`, Python main guard/`__main__.py`, package.json main/bin/scripts, `listen()`/`run()`, `@SpringBootApplication`
- [x] Express-style routes, route chains, router mounts across files, middleware chains; NestJS
- [x] Flask (blueprints), FastAPI (routers), Django `path()`, Spring MVC, JAX-RS
- [x] Startup path from an entry point to route registration (`loads_module`, `mounts`, `registers_route` edges)
- [x] Explicit throw/raise/catch/throws sites, local handler linking, possible sites in callees
- [x] Python, Java, C/C++ extractors

## Phase 6: Interactive terminal UI
- [x] Header with project, status, root and counts; search; result navigation
- [x] Symbol view (callees/callers trees, exceptions, source snippet), file view (symbols, dependencies, dependents)
- [x] Route view (registration, mount, handler, middleware, startup path, flow, callers, exceptions)
- [x] Entry points, routes and status/limitations screens; help; clean shutdown; non-TTY fallback

## Phase 7: Reliability and performance
- [x] Synthetic repository generator and benchmark runner with an RSS regression check
- [x] Memory and query measurements (README; `bench/results/`)
- [x] Bounded resolver caches (an unbounded-memory trend found by measurement was fixed)
- [x] Linux build and full test suites (GCC 15.3, Node 22)
- [x] Leak checks: allocator-exact lifecycle, query, parse and aborted-walk tests; long-running serve RSS and CLI heap measurements
- [x] Analysis peak memory flat with repository size (a `GROUP BY` statistics regression was found by the benchmark and fixed)
- [x] Clang and macOS: GitHub Actions builds with `-Werror` and runs all tests on Ubuntu GCC, Ubuntu Clang and macOS (first green run on 2026-10-12, after a Clang-only unused-field error was fixed)
- [x] Documentation

## Phase 8: Refresh and deeper resolution
- [x] `proj refresh <project>`: fingerprint change detection, re-parse only changed/added files, remove deleted files
- [x] Stable symbol ids across refreshes; affected-set computation; full re-resolve fallback above 40 % affected
- [x] Refresh equals full analysis: randomized equivalence test, `proj_index_dump`, verified on Kafka (5 scenarios) and a FastAPI service
- [x] Name collisions: renamed or duplicated names never capture existing calls (unresolved with a reason, or ambiguous with candidates)
- [x] Chained calls through declared return types (Java, TS, Python, C++); `await`, `new X().m()`, casts
- [x] Values returned by calls (`x = f()`); Python constructor injection (`self.x = param`)
- [x] Java static imports, external base classes, typed lambda/resource/`instanceof` variables
- [x] `.gitignore` files of an enclosing Git repository apply when the project is a subdirectory

## Phase 9: Legacy servers
- [x] XML and YAML parsing (tree-sitter-xml 0.7.0, tree-sitter-yaml 0.7.2); configuration files never make a run partial
- [x] web.xml servlets, mappings, filters (route middleware), listeners and load-on-startup; Struts 1/2; Spring XML beans, wiring and URL mappings; OpenAPI; serverless.yml; generic path + handler entries
- [x] Handler classes dispatched per framework (doGet/doPost/service, execute, handleRequest, get/post, on_get, do_GET, handle, ...), one route per HTTP method
- [x] Java without annotations: `@WebServlet`, `addServlet`/`addMapping` (Servlet API, Jetty, Tomcat), JDK `HttpServer`, Spark/Javalin, handler maps; lambdas and anonymous handler classes
- [x] Python: Tornado, webapp2, web.py, Falcon, Pyramid, Bottle, Flask `add_url_rule`/`MethodView`, Django `url()`, class-based views and `include()` prefixes, `http.server`, WSGI
- [x] C/C++: registration calls (cpp-httplib, Crow, Drogon, Pistache, ...), route tables, handler objects, lambdas
- [x] Dispatch branches (`if`/`switch`/`match` on the request path) in Java, Python and C/C++
- [x] Overloads selected by static argument types (Java, C++); C++ same-arity overloads no longer merged with declarations
- [x] Route search with servlet (`/*`) and extension (`*.do`) wildcards
- [x] Schema v3; refresh equals full analysis on the legacy fixtures (randomized); Clang 22/libc++ `-Werror` build checked locally with Zig's Clang
- [ ] `@WebFilter`, servlet context paths, Spring XML wiring used for field types, JSON route files

## Not implemented (by design for the MVP, documented)
- [ ] `proj remove` (manual steps in the README)
- [ ] Generics-based resolution (`List<User>` element types)
- [ ] `tsconfig` path aliases, compiler include paths / `compile_commands.json`
- [ ] Parallel parsing (the design allows a worker pool; one worker today)

## Next milestone
`proj remove`, and typing
more receivers (generic element types, untyped lambda parameters). On Kafka,
26 % of the unresolved calls have a receiver of unknown type and another 28 %
are chained on a call that is itself unresolved.
