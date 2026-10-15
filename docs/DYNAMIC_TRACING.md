# Runtime tracing (dynamic mode) and the Error Tracer

Static analysis tells what code *can* call. Runtime tracing records what a real
run *did* call, for Java applications, and adds that evidence to the same graph:
calls the static model could not see (reflection, configuration-driven loading,
callbacks), which override actually ran, and every exception the application
code saw, with its stack and its fate. It is optional and off until started.

Everything stays on the machine: the agent talks only to a service on
127.0.0.1, and observations are stored next to the project's index.

## Quick start

```sh
proj add shop ~/src/shop                 # the static index (needed first)
proj start dynamic shop                  # starts the service; prints a -javaagent option
java -javaagent:...proj-agent.jar=project=shop,service=...,include=com.example.shop -jar shop.jar
proj traces shop                         # observed requests, startup, tasks
proj errors shop                         # Error Tracer
proj show shop                           # x: errors, t: traces; call trees show runtime evidence
proj stop dynamic                        # detaches; the application keeps running
```

To trace a JVM that is already running instead: `proj start dynamic shop --pid 12345`
(startup before the attach cannot be recovered and is reported as such), or
`--match <text>` to attach to JVMs of yours whose command line contains the
text, as they start.

## How it works

```
 JVM (your application)                 proj-engine --dynamic-service            runtime/project-N.db
┌─────────────────────────────┐  TCP    ┌──────────────────────────────┐        ┌──────────────────────┐
│ proj-agent.jar              │ 127.0.0.1│ monitors (state machine)     │ batched│ sessions, methods,   │
│  ASM bytecode hooks         │ ───────▶│ agent connections            │───────▶│ traces (call trees), │
│  per-thread call trees      │  token  │ attach helper (--pid/--match)│  txns  │ edges, errors        │
│  bounded queue, reconnects  │ ◀────── │ control: proj start|stop|... │        └──────────┬───────────┘
└─────────────────────────────┘ enable/ └──────────────────────────────┘                   │ read-only
                                detach                                    proj-engine queries map runtime
                                                                          methods to static symbols
```

- **Agent** (`agent/`, Java 8 bytecode, ASM 9.10.1 compiled in under
  `proj.agent.asm`): a `java.lang.instrument` agent. Loaded with `-javaagent`
  at JVM startup (complete startup coverage) or attached later through the
  attach API (`java -jar proj-agent.jar --attach PID ARGS`, which the service
  runs). JVMTI was measured and rejected: on JDK 21 a dynamically attached
  JVMTI agent cannot get method or exception events, and the spec requires
  attaching to running JVMs.
- **Service** (`engine/src/dynamic/`): one single-threaded process per data
  directory (an exclusive lock), started by the first `proj start dynamic` and
  gone when nothing is monitored. It listens on 127.0.0.1 only; the port and a
  random token are in `<data_dir>/dynamic/service.json` (mode 0600), and every
  connection must present the token. Java 8 has no Unix domain sockets, hence TCP.
- **Store** (`runtime/project-N.db`): SQLite in WAL mode, written only by the
  service, read by the engine's queries. Analysis and refresh never touch it,
  and it never writes the static index, so `refresh` still equals a full analysis.

## Which JVM belongs to which project

A project directory does not identify a running process, and the service never
attaches to arbitrary JVMs. A JVM is associated with a project in one of three
explicit ways:

| how | coverage | where it comes from |
|---|---|---|
| `-javaagent:...=project=NAME,...` on the JVM command line (or `JAVA_TOOL_OPTIONS`, a container's `setenv.sh`) | from JVM startup | printed by `proj start dynamic NAME` and `proj status dynamic` |
| `proj start dynamic NAME --pid PID` | from the attach on | the process must exist and belong to you |
| `proj start dynamic NAME --match TEXT` | from the attach on | JVMs of yours whose command line contains TEXT (found through `hsperfdata_<user>`; JVMs with `-XX:-UsePerfData` are not found) |

The agent's `include=` packages come from the project's index (Java packages,
grouped by their first two segments and reduced to a common prefix); override
them with `--include` (repeatable). Only classes in those packages are
instrumented, never `java.*`, `javax.*`, `jdk.*`, `sun.*`, `com.sun.*`, the
agent itself or generated proxies (`$$`).

`proj start dynamic` without a project starts every project configured by an
earlier start (`<data_dir>/dynamic/config.json`).

## Lifecycle

Each monitored project has an explicit state; transitions are listed by
`proj status dynamic` (last 32 kept).

| state | meaning |
|---|---|
| `STOPPED` | not monitored |
| `IDLE` | waiting for a JVM (startup option), a pid to attach to, or a match |
| `TARGET_DISCOVERED` | a JVM connected, or one to attach to was found |
| `ATTACHING` | the attach helper is loading the agent (bounded: 30 s; up to 4 attempts with 4/8/16 s backoff, then that pid is given up) |
| `ATTACHED` | the agent is connected and recording |
| `CAPTURING` | the first trace arrived |
| `FLUSHING` | a JVM disconnected (exit, kill, stop): its last data is committed, its session closed |

- Starting a monitored project again, or attaching to a pid already connected,
  changes nothing. A second `proj start dynamic` reuses the running service.
- `proj stop dynamic [NAME]` sends `detach`: the agent stops recording, removes
  its transformer and retransforms the classes it changed back to their
  original bytecode, flushes what it has queued, and disconnects. The
  application keeps running. Stopping something not monitored is not an error.
- An agent of a project that is not monitored is accepted on standby: it
  records nothing and keeps its connection, and `proj start dynamic NAME`
  enables it (coverage is then not "complete").
- The service saves the monitored set (`dynamic/monitors.json`) and resumes it
  when it starts again. Sessions a crashed service left open are marked
  `interrupted` when the store is next opened; a still-running agent reconnects
  (backoff 0.5 s to 30 s) and continues its session.
- Sessions end as `ended` (the JVM exited and said so), `stopped` (monitoring
  stopped) or `interrupted` (the connection was lost: killed JVM, crash).

**Idle cost**: with no attach rule the service sleeps in `poll()` with no timers
(no wakeups at all); a `--match` rule wakes it every 2 s to list
`hsperfdata_<user>` (a small directory). It never parses sources or rebuilds
indexes. Measured below.

## What a trace is

A trace is one outermost application-code invocation on a thread: a request
handled by a servlet or handler, `main()`, a task's `run()`. Inside it, calls
are merged into a tree by (parent node, method): the tree keeps the order of
first calls and counts repeats, so a loop does not grow it. Limits: 5000 nodes
per trace (further distinct calls are counted as dropped and the trace is
`truncated`), stack depth 4096.

- **Request identity**: for handler methods taking an `HttpExchange` or a
  (`javax`/`jakarta`) `ServletRequest`, the HTTP method and path (never the query
  string or body) name the trace.
- **Threads**: each thread's traces are separate; concurrent requests never share
  a tree. Work handed to another thread is its own trace there (the static
  `thread_start` edges link the two); cross-thread causality is not reconstructed.
- **Long traces** (a server's `main()` blocks forever): sent as `open` snapshots
  every 5 s, replaced by the final version; at JVM exit they are sent as
  `incomplete`.
- **Ordering**: parents precede children; siblings are in order of first call.
  Caller and callee are the innermost application frames: a call through
  library code (a framework, reflection) appears as a direct child, and its
  evidence says whether a static call site accounts for it.

Constructors get entry and normal-exit hooks but no catch-all handler (the
uninitialized `this` before `super()`); a constructor left by an exception is
detected by the next event of its caller. Methods whose frames or stores
disagree with the declared parameters (not javac output) get the same treatment.

## The Error Tracer

The agent hooks every `athrow` and every exception handler in application
methods, and the catch-all handler sees exceptions leaving a method. One record
per exception object per trace, with what happened to it:

| state | established by |
|---|---|
| `CAUGHT` | last handled by an application `catch` block (not a `finally`) and not seen again |
| `WRAPPED` | became the cause of another exception observed in the same trace (the record names the wrapper) |
| `ESCAPED` | left the trace's outermost application method; what the framework did next is not seen |
| `UNCAUGHT` | terminated its thread (the agent's default uncaught-exception handler, which then behaves exactly as before) |
| `UNKNOWN` | anything else, e.g. it passed into library code between application frames and was not seen again |

`rethrown` marks an exception caught by application code and thrown again.
The record keeps, separately, the throw site (an application `athrow`; absent
for exceptions thrown in a library or the JDK), the most recent application
frame (often, not always, the throw site), the handler that caught it, and the
sequence of events (`thrown`, `exit`, `caught`, `finally`). Stack frames are
stored exactly as the JVM recorded them, most recent call first, which is also
the display order; application frames are mapped to source files, other
frames are marked as outside the project. Causes (4 levels, 16 frames each)
and the suppressed count are kept.

Identical errors (type, state, throw site, first 8 frames) are one record
with an occurrence count, linked to the traces they occurred in. Messages are
truncated to 300 characters; `messages=off` in the agent options drops them.
Local variables, arguments and payloads are never recorded.

## Runtime evidence in the graph

Runtime identities (class, method name, JVM descriptor) are mapped to static
symbols when queried, so a re-analysis never invalidates observations:

| mapping | meaning |
|---|---|
| `exact` | the symbol of that name; overloads told apart by line, then parameter count and types |
| `lambda` | a lambda body: its enclosing method, which the static model credits with its calls |
| `implicit` | a constructor the source does not declare: its class |
| `enclosing` | no symbol of that name (e.g. an anonymous class's constructor): the method containing its first line |
| `unresolved` | not in the index (another module, generated code): the runtime identity is kept, no location is invented |

Each observed call gets one of these:

| evidence | meaning |
|---|---|
| `STATIC_CONFIRMED` | a resolved static edge accounts for it |
| `STATIC_INFERRED` | a heuristic edge, one candidate of an ambiguous call, dynamic dispatch through an overridden method, or an implicit `super()` call |
| `RUNTIME_OBSERVED` | no static call site links the two: only the runtime shows it (reflection, configuration, callbacks) |
| `RUNTIME_UNRESOLVED` | one end is not in the index |

Call trees (`traverse`, the explorer's callees and callers) show static edges
as before, add `observed` counts to the ones that ran, attach a runtime
observation to the ambiguous call or overridden method that predicted it, and
add calls only the runtime saw. Nothing is merged into the static index: a
static prediction that did not run is still shown, and a call that ran but
was never predicted is marked as such. One run does not show every possible path.

## Storage

`runtime/project-N.db` (schema version 1, `PRAGMA user_version`):
`rt_sessions` (one per agent load: pid, mode, startup coverage, attach delay,
JVM, packages, index identity, Git revision, status, agent counters),
`rt_methods` (class, name, descriptor, source file, lines), `rt_traces` (call
tree as JSON, final flag), `rt_edges` (caller, callee, calls, traces),
`rt_errors` and `rt_error_traces`.

- Writes are batched in transactions (committed at least every 200 ms).
- A trace is counted (edges, errors) once, when final; a replayed final trace
  is ignored, so retries never double counts.
- Retention: the newest 5000 traces, the 1000 most recently seen errors, the
  newest 200 sessions.

## Resources and safety

- Agent: a bounded queue (4096 items, 16 MiB); when it is full, or while no
  service is reachable and it fills, items are dropped and counted. The counts
  reach the session (`stats`), and a session with drops is not reported as
  having complete coverage. 5000 nodes and 64 exceptions per trace.
- The hooks never throw into the application: after 100 internal errors the
  agent stops recording.
- Service: memory bounded by the connections' line buffers (32 MiB per line at
  most) and one open batch per project.
- Attach only to processes of the same user (the JVM's own rule, checked before
  trying), and only those named by `--pid` or matched by `--match`.

Agent options (comma-separated after `=`): `project`, `service`, `include`
(`a.b:c.d`, `<default>` for the default package), `messages=on|off`,
`maxMessage`, `maxNodes`, `maxErrors`, `queueItems`, `queueMiB`, `snapshotMillis`.

## Measured cost

Measured with `node bench/dynamic.mjs` on the dynamic-java fixture (Fedora 43,
Intel Core Ultra 7 255HX, 20 threads, OpenJDK 21.0.12; results in
`bench/results/2026-10-15-dynamic-linux-x86_64.json`):

| measurement | without the agent | with the agent |
|---|---|---|
| a loop calling 5 trivial application methods (calls/s, median of 3 runs of 8 s) | 35.1 M | recording: 6.35 M (5.5× slower; 4.7× in a second run), about 26 ns per instrumented call; on standby (not monitored): 27.2 M (1.29×) |
| HTTP requests to the fixture server (`GET /users/<id>`, 11 application methods each; mean of 3000 sequential requests) | 718 µs | 705 µs: below the run-to-run noise |
| a burst of 20,000 short traces (4 threads, 104 ms) | | 13,696 delivered and stored at about 15,000 traces/s, 6,304 dropped by the full agent queue and counted; service peak RSS 12 MiB; store 3.9 MiB with the 5000 traces kept |
| the same burst with a 16-item agent queue | | 11,898 dropped and counted; the session's coverage reported as `interrupted` |
| the service waiting for a JVM, 30 s | | 0.00 s of CPU (10 ms resolution), 12 MiB RSS |

The cost is per call of an application method, so it shows in tight loops of
tiny methods and disappears in request handling that does real work. A
monitored JVM that is not recording (standby) pays only a field check per
call. Bursts far above the service's ingest rate fill the agent's queue: the
drops are counted and reported rather than hidden.

## Platforms and known limitations

- Linux and macOS; the agent loads into Java 8 and newer JVMs (class files up to
  Java 27 can be instrumented).
- The agent appends its jar to the bootstrap class path, so that instrumented
  classes of any class loader can reach it. HotSpot then prints "Sharing is only
  supported for boot loader classes because bootstrap classpath has been
  appended" and disables class-data sharing for application classes. OSGi
  containers need `org.osgi.framework.bootdelegation=proj.agent.*`.
- JDK 21+ prints a warning for agents loaded into a running JVM unless it was
  started with `-XX:+EnableDynamicAgentLoading`; a future JDK may require that flag.
- Instrumentation assumes javac-style bytecode (exception handlers entered only
  by exceptions); classes that fail to transform are left as they were and counted.
- Calls through library code appear as direct children; cross-thread causality,
  asynchronous request completion and calls across processes are not traced.
- Threads with their own uncaught-exception handler, or whose thread group
  overrides `uncaughtException`, never report `UNCAUGHT`; the application
  replacing the default handler after the agent starts has the same effect.
- Dynamic attach needs a JDK (the attach API) for the helper; the target JVM
  can be a JRE.
- Paths and project names containing commas cannot be passed in agent options.
