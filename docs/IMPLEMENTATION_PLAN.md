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

## Phase 10: Code map and endpoint navigation
- [x] Engine `overview` operation: directory parts within a node budget, single-child chains collapsed, "other" and "outside" groups, aggregated links and per-part counts (0.37 s on Kafka)
- [x] Home screen code map: layered text graph (cycle breaking, longest-path layers over significant links, barycenter ordering, row wrapping, fan-in/fan-out buses, ASCII fallback), zoom into parts and down to files, details panel with every link
- [x] Route lists (summary, routes view, search results) show the handler method
- [x] `/show <endpoint>` in the search box; `proj show <project> <endpoint>` opens the route, or prints its flow with `--no-tui` (exit 2 when nothing matches)
- [ ] Interactive edge selection in the map (links are listed in the details panel instead)

## Phase 11: Measured static-analysis gaps
Found by measuring Kafka (non-test Java methods without any caller in the graph, refresh phase timings).
- [x] Java method references (`this::m`, `Type::m`, `obj::m`, `Type::new`) are calls from the enclosing method. Kafka: 2,636 more resolved call edges; non-test methods without callers 6,439 → 6,038
- [x] Refresh: the per-file delete and snapshot statements scanned `type_hints` and the `symbols_name` index once per changed file. Kafka refresh after every Java file changed: 233 s → 39 s (a full analysis takes 32 s); `file_scan_steps` in the refresh stats, with a regression test
- [x] Java anonymous classes: inferred edge from the creating method to their methods (Kafka non-test code: anonymous-class methods without callers 468 → 0)
- [x] Background execution: `submit`/`execute`/`schedule*`, `runAsync`/`supplyAsync`, `invokeLater`, `Timer.schedule`, `addShutdownHook`, `new Thread(task)` and `start()` on `Thread` subclasses link to the task's `run()`/`call()` as inferred edges (Kafka: `run()`/`call()` of task and thread classes without callers 77 → 15; the rest go through Kafka's own timer API)
- [ ] Narrow overloaded method references by the functional interface they are passed as

## Phase 12: Runtime tracing (Java) and the Error Tracer
See docs/DYNAMIC_TRACING.md.
- [x] Mechanism measured first: a JVMTI agent attached to a running JDK 21 gets no method or exception events, so a `java.lang.instrument` agent with ASM (9.10.1, pinned and hash-checked, compiled in under `proj.agent.asm`, Java 8 bytecode)
- [x] Agent: entry/exit hooks with a catch-all exit handler, throw and catch hooks, default uncaught handler; per-thread call trees merged by (parent, method); request names (HttpExchange, servlet); bounded queue with counted drops; reconnects; detach restores the original classes
- [x] Service (`proj-engine --dynamic-service`): per-project state machine (IDLE, TARGET_DISCOVERED, ATTACHING, ATTACHED, CAPTURING, FLUSHING, STOPPED), 127.0.0.1 + token, startup option, `--pid` and `--match` attach with bounded retries, standby agents, resume after restart, no wakeups while idle
- [x] Runtime store separate from the index: sessions, methods (JVM descriptors), call trees, aggregated edges, errors deduplicated by fingerprint; idempotent replays; retention
- [x] Mapping to static symbols (exact, lambda, implicit, enclosing, unresolved) and evidence (STATIC_CONFIRMED, STATIC_INFERRED, RUNTIME_OBSERVED, RUNTIME_UNRESOLVED) on traversals and traces
- [x] Error Tracer: CAUGHT, WRAPPED, ESCAPED, UNCAUGHT, UNKNOWN; rethrown; throw site, top application frame and handler kept apart; stacks most recent call first
- [x] CLI: `proj start|stop|status dynamic`, `proj errors`, `proj traces`; explorer screens (`x`, `t`) and runtime markers in call trees
- [x] Tests: engine (store, mapping, evidence, leak check), CLI end to end with real JVMs (lifecycle, startup, attach, termination, flows, errors, persistence); `bench/dynamic.mjs`
- [ ] Cross-thread and cross-process causality; asynchronous request completion
- [ ] Validation on an authorized legacy application (Tomcat, JBoss, WebSphere class loaders; OSGi boot delegation)
- [ ] Lower per-call cost (a local variable instead of a second thread-local lookup per exit)

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
