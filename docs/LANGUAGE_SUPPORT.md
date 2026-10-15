# Language support

Parsing uses Tree-sitter 0.25.10 with these grammars, compiled into the
engine: JavaScript 0.25.0, TypeScript/TSX 0.23.2, Python 0.25.0, Java 0.23.5,
C++ 0.23.4, C 0.24.1, XML 0.7.0 and YAML 0.7.2 (tree-sitter-grammars).

Tree-sitter provides syntax only. Everything below is what the extractors and
the resolver derive from that syntax. **None of it is full semantic analysis**:
there is no type checker, no build-system integration, no macro expansion, and
no runtime information. Every relationship records its resolution status (see
[INDEX_SCHEMA.md](INDEX_SCHEMA.md)), so you can tell a proven link from a
guess.

## File types

| Extensions | Language |
|---|---|
| `.js .mjs .cjs .jsx` | JavaScript (JSX supported by the grammar) |
| `.ts .mts .cts` (incl. `.d.ts`) | TypeScript |
| `.tsx` | TSX |
| `.py .pyi` | Python |
| `.java` | Java |
| `.c` | C |
| `.h` | **parsed as C++** (works for most C headers) |
| `.cc .cpp .cxx .c++ .hpp .hh .hxx .h++ .ipp .inl .tpp` | C++ |
| `.xml` | XML configuration (see [Configuration files](#configuration-files)) |
| `.yml .yaml` | YAML configuration |

All other files are recorded as `unsupported`. Their content is not read.
Configuration files are reported separately: their syntax errors (templated
YAML, for example) and size-limit skips never make an analysis partial.

## Configuration files

Legacy servers often declare endpoints outside the code and name the class
or function that handles them. `proj` reads these declarations and links them
to the code:

| Format | Recognized | Handler |
|---|---|---|
| `web.xml`, `web-fragment.xml` | `<servlet>` + `<servlet-mapping>` (every `url-pattern`), `<filter>` + `<filter-mapping>` (by URL pattern or servlet name, in declaration order), `<listener>`, `<load-on-startup>`, `<jsp-file>` | the servlet class, dispatched by HTTP method (below); filters become route middleware (`doFilter`) |
| Struts 1 `struts-config.xml` | `<action path type parameter forward>` | `execute` of the action class; `DispatchAction` with `parameter` is noted as chosen at runtime |
| Struts 2 `struts.xml` | `<package namespace>` + `<action name class method>` | the named method, else `execute` |
| Spring XML (`<beans>`) | `<bean id/name class init-method>`, `<property ref>`/`<constructor-arg ref>` wiring, `BeanNameUrlHandlerMapping` (`<bean name="/x.htm">`), `SimpleUrlHandlerMapping` (`<props>`, `<map>`, properties text), `<mvc:view-controller>` | `handleRequest` / `handleRequestInternal` of the bean's class, also for beans declared in another XML file |
| OpenAPI / Swagger YAML | `paths` × HTTP methods, `operationId`, `x-openapi-router-controller` / `x-swagger-router-controller`, `basePath` / `servers[0].url` | `module.function` (connexion) or a function/method matched by name only (`heuristic`) |
| `serverless.yml` | `functions.*.handler` with `http` / `httpApi` events (object or `"GET /path"` form) | `dir/module.function`, Java `Class` / `Class::method` |
| Any other XML or YAML | an element or mapping with a URL path (`path`, `url`, `uri`, `route`, `pattern`, `url-pattern`, `endpoint`, `mapping`) and a code reference (`handler`, `class`, `controller`, `servlet`, `action`, `callback`, `function`, `operationId`, `target`, `bean`, `impl`, ...), optional `method`/`methods`, and `prefix`/`basePath`/`contextPath` on enclosing entries; also maps keyed by path (`/users: com.acme.Users`) | `heuristic` routes |

A code reference may be a Java class (`com.acme.web.UserServlet`,
`Outer$Inner`), a method (`com.acme.Users#list`, `com.acme.Users.list`), a
Python dotted path (`app.views.home`, `app.views:home`), a C++ name
(`api::Users`, `api::Users::list`), a source file (`handlers/users.py`,
`WEB-INF/jsp/home.jsp`) or `dir/module.function`. Classes are found by exact
qualified name, then by the end of their qualified name (`app.X` matches
`src.app.X`), then (for a bare name) by class name. A qualified name whose
package or module is not part of the project is `external`
(`org.apache.struts.action.ActionServlet`). JSP pages are recognized but not
analyzed.

Configuration files also get `configures` edges: from the file (or a Spring
bean) to each class the container creates (servlets, filters, listeners,
beans), to lifecycle methods it calls (`contextInitialized` of listeners,
`init` of `load-on-startup` servlets, bean `init-method`s), and from beans to
the beans injected into them. `web.xml` is an entry point
(`deployment_descriptor`), so the startup path of listener code and servlet
handlers leads back to it; other files that declare routes are `route_config`
entry points.

**Handler classes.** When a route names a class (in configuration, or in
code such as `server.createContext("/x", new StatusHandler())`), the resolver
finds the methods that serve requests, in the class or its project base
classes, using the framework's convention. A class serving several HTTP
methods becomes one route per method:

| Framework | Methods |
|---|---|
| Servlets (web.xml, `@WebServlet`, `addServlet`) | `doGet`, `doPost`, `doPut`, `doDelete`, `doPatch`, `doHead`, `doOptions`, `doTrace`; else `service` |
| JDK `HttpServer` | `handle` |
| Struts 1/2 | `execute` (or the configured method) |
| Spring XML controllers | `handleRequest`, `handleRequestInternal` |
| Tornado, webapp2, Django class-based views, Flask `MethodView`, Pyramid | `get`, `post`, ... |
| Falcon | `on_get`, `on_post`, ... |
| web.py | `GET`, `POST`, ... |
| `http.server` | `do_GET`, `do_POST`, ... |
| Other (configuration files, C/C++, front-controller maps) | `doGet`, `get`, `onGet`, `handleGet`, `on_get`, `do_GET`, `GET`; else `handle`, `handleRequest`, `service`, `execute`, `process`, `onRequest`, `invoke`, `dispatch`, `operator()`, `__call__`, `call`, `run`, `apply` |

A class without any of them is reported as unresolved, with the methods
that were looked for (a servlet that only inherits `HttpServlet`'s methods,
for example).

**Dispatch on the request path.** Hand-written dispatchers are recognized as
`heuristic` routes in all three server languages: `if`/`else if` conditions
and `switch`/`match` cases that compare the request path (the condition must
mention `path`, `uri`, `url`, `request`, `action`, `command`, ...) with a
literal such as `"/users"`. The handler is the first call in the branch that
handles the request (names containing `handle`, `serve`, `process`,
`dispatch`, `render`, ...), else the first call that is not logging, output
or a request accessor. Bare `/` and wildcard-only literals are ignored.

## JavaScript / TypeScript / TSX

**Symbols:** function declarations and signatures (overloads, as
declarations); named function and arrow expressions bound to variables,
object properties, class fields, `module.exports`/`exports.x` or `export
default`; classes (including class expressions); methods, constructors,
static members, accessibility modifiers, `#private`; TS interfaces, type
aliases, enums, namespaces; named object literals at module level (their
function-valued properties become methods); module-level variables holding
routers or `new X()` instances.

**Imports/exports:** ESM default, named, aliased, namespace, side-effect and
`import type`; `export` declarations, export lists, re-exports (`export * /
{a} from`); CommonJS `require` (whole module, destructured, `.prop`),
`module.exports =` (identifier, instance, function, class, object literal,
`require(...)`), `exports.x =`; dynamic `import('...')`; TS `import x =
require()`. Relative specifiers resolve with extension and `index` probing,
including the TS convention that `./x.js` refers to `x.ts`. Bare specifiers
are external. `@/…`, `~/…` and slash-containing bare specifiers that match a
project path are labeled **heuristic**, because `tsconfig.json` `paths` and
`baseUrl` are not read.

**Calls:** direct calls, member calls, `new`, `super(...)`,
`require('./x')(...)`. Receivers are resolved through lexical scope
(parameters and locals shadow outer names), import bindings, exports
(including CommonJS objects and exported instances), `this`/`super` with
inheritance, TS parameter and field type annotations, `this.x = new X()`
field hints, and `const x = new X()` locals. Declared return types type
chained calls (`a.b().c()`) and values assigned from calls (`const repo =
await connect()`). `await` unwraps `Promise<T>`. `new X().m()` and `(x as
X).m()` use `X`. Methods of JS built-in types (`Promise`, `Array`, ...) are
`external`. Calls to JS globals (`console`, `JSON`, `Math`, `setTimeout`, …)
are not stored.

**Routes (Express-style):** `app|router.get/post/put/patch/delete/options/head/all(path, ...middleware, handler)`
with literal or substitution-free template paths, `router.route('/p').get(...).put(...)`
chains, inline handlers (indexed as `route_handler` symbols), handlers wrapped
as `asyncHandler(fn)`, `app.use([prefix,] ...)` classified as router mounts or
middleware, nested mounts across files, and middleware chains (app/mount-level
middleware registered before the mount, router-level `use` before the route,
and route-level arguments). The registrar is **recognized** when it was created
by `express()`, `express.Router()`, `Router()`, `new Router()` or `fastify()`.
Other receivers (for example an `app` parameter) produce **heuristic** routes.
Calls such as `axios.get('/x', config)` are filtered out. Fastify, Koa-router and
similar libraries with the same call shape are therefore matched only
heuristically.

**Routes (NestJS):** `@Controller('prefix')` classes with `@Get/@Post/@Put/@Patch/@Delete/@Options/@Head/@All(path)`
methods.

**Exceptions:** `throw new X()` (type X), `throw X(...)`, rethrow of the catch
parameter, `try/catch` handlers (catch-all). A throw is linked to the
innermost enclosing `try` in the same function. A throw inside a nested
function or inside the catch block is not linked.

**Entry points:** `package.json` `main`, `bin` and `start`/`dev`/`serve`/`server`
scripts that run a file; `listen()` on a recognized app or on `app`/`server`.
Top-level imports create `loads_module` edges, so startup files reach route
registration.

**Not supported:** computed member names (`router[method](...)`, recorded as
unresolved), routes with computed paths (recorded with a `<dynamic: …>` path
and a diagnostic), decorator metadata beyond NestJS routing, `tsconfig` path
mapping, prototype assignment (`X.prototype.m = …`), callbacks and higher-order
functions (calls inside anonymous callbacks are attributed to the enclosing
named function), JSX component usage as calls, and dependency injection
containers.

## Python

**Symbols:** functions, nested functions, classes, methods (`__init__` as
constructor), `@staticmethod`/`@classmethod`, module-level router objects.
Module path (`app.services.user`) is derived from the file path.

**Imports:** `import a.b`, `import a.b as c`, `from x import y [as z]`,
relative imports (`from . import x`, `from ..pkg import y`), `from x import *`,
`from pkg import submodule`. Absolute imports are resolved against the project
root, `src/`, and each ancestor directory of the importing file.
Namespace packages without `__init__.py` are recognized. `sys.path`
manipulation and installed packages are not analyzed (external).

**Calls:** names in lexical scope and imports; `self.m()`, `cls.m()`,
`super().m()`; `self.x.m()` via `self.x = X()` or annotated attributes;
`obj.m()` via `obj = X()` / annotations / parameter annotations; calling a
class is an instantiation (`__init__`). `self.x = param` takes the
annotation of the parameter (constructor injection). Return annotations
(`-> User`, `-> 'User'`) type chained calls (`self.repo.find(1).name()`) and
values assigned from calls (`client = make_client()`, also with `await`).
Values returned by external calls (`logger = logging.getLogger(...)`) make
calls on them `external`. A call on a value whose producer declares no
return type stays `unresolved` with a note saying so. Python built-ins are
not stored, and methods on built-in return types (`str`, `dict`, ...) are
`external`.

**Routes:** Flask `@app.route/@bp.route(path, methods=[...])` (default `GET`),
Flask 2 shortcuts `@bp.get(...)`; `Blueprint(url_prefix=...)` and
`register_blueprint(bp, url_prefix=...)` (the registration prefix overrides the
blueprint's, as in Flask); FastAPI `@app.get(...)` / `@router.post(...)`,
`APIRouter(prefix=...)` and `include_router(router, prefix=...)` (both prefixes
apply); Django `path()`/`re_path()`/`url()` entries in `urlpatterns` (method
`ANY`), with `include('app.urls')` / `include(module)` prefixes resolved
through any number of URLconfs, and class-based views
(`View.as_view()`) dispatched to `get`/`post`/...; Flask `add_url_rule`
(functions and `MethodView.as_view()`); Bottle `@route`/`@get`/... and
`@app.route(..., method=...)`; Tornado `Application([...])` handler lists
(tuples, `url(...)`, lists assigned to a variable), webapp2
`WSGIApplication`; web.py URL tuples naming classes by string; Falcon
`add_route(path, Resource())`; Pyramid `config.add_route(name, pattern)` with
`config.add_view(view, route_name=...)` and `@view_config(route_name=...,
request_method=...)` (the pattern is found in whichever file declares the
route); `http.server` `HTTPServer(addr, Handler)` (`do_GET`/`do_POST`) and
`wsgiref` `make_server(host, port, app)`; and `if`/`elif`/`match` dispatch
on `self.path` or similar (see above). Regular-expression paths are shown
with named groups as parameters (`^/users/(?P<id>\d+)/?$` →
`/users/{id}`). Other decorators on a view (`@login_required`) are recorded
as route middleware.

**Exceptions:** `raise X(...)`, `raise X`, bare `raise` and re-raise of the
`except ... as e` variable (rethrow). `except` clauses with single, tuple
and bare types. A raise is linked to a local handler when the names match
exactly or the handler catches `Exception`/`BaseException`/everything.
Class hierarchies are not used for matching.

**Entry points:** `if __name__ == "__main__":` (top level), `__main__.py`,
`app.run()` on a recognized app, `uvicorn.run()`.

**Not supported:** dynamic attribute access, decorators that replace
functions (other than route registration), metaclasses, `getattr`-based
dispatch, Django `include()` of a list (instead of a module), and CherryPy.

## Java

**Symbols:** classes, interfaces, enums, records, annotation types, nested
types, methods, constructors, modifiers (visibility, `static`), `throws`
clauses, parameter counts (for overloads).

**Imports:** single-type imports (resolved by package path, or by declared
qualified name for non-standard layouts), wildcard imports of project packages
(`package` status), static imports, same-package types without imports.
`java.lang` and unknown packages are external.

**Calls:** unqualified calls resolve through the enclosing class, its
superclasses and outer classes, then static imports (a single-method import
before a wildcard import); `this.`/`super.`; `obj.m()` through declared
types of locals, parameters (including `catch`, enhanced-`for`, typed lambda
and try-with-resources variables, and `instanceof` pattern variables) and
fields; static calls `Type.m()`; `new X()` to the matching constructor.
Method references (`this::drain`, `Util::print`, `worker::work`,
`Repo::new`) are calls from the enclosing method, as the calls inside a
lambda body are, and resolve the same way. Their argument count is not
visible, so an overloaded name stays `ambiguous` among its overloads.
Methods of an anonymous class (`new Runnable() { public void run() {...} }`)
are reached from the method that creates the object by a `heuristic` edge:
they run when that object is used. Work handed to another thread is linked
the same way: `pool.submit(task)`, `execute`, `schedule`,
`scheduleAtFixedRate`, `scheduleWithFixedDelay`, `CompletableFuture.runAsync`
/ `supplyAsync`, `invokeLater`, `timer.schedule(task, ...)`,
`Runtime.addShutdownHook(hook)` and `new Thread(task)` link to the `run()` or `call()` (`get()` for `supplyAsync`) of
the project class named by the argument's declared type, and `start()` on a
subclass of `Thread` links to its `run()`.
Chained calls (`repo.find(1).name()`) resolve through declared return types,
and so do `var u = repo.find(3)` locals. `new X().m()` and `((X) o).m()` use
`X`. Untyped lambda parameters shadow outer names and stay unresolved.
Methods inherited from an external superclass (`extends HashMap`) are
`external`. When an external base class and a static wildcard import could
both supply the method, the note names both. Calls on interfaces resolve to
the interface method, with `overridden_by` edges to implementations
(possible dynamic dispatch).

**Overloads** are selected by argument count, then by the static types of the
arguments where they are visible: literals (`"x"` is `String`, `42L` is
`long`, `null`), `new X()`, casts, and variables, parameters and fields with
a declared type. Exact matches beat widening (`int` → `long`), boxing
(`int` → `Integer`) and subtypes (project class hierarchies and common JDK
types such as `ArrayList` → `List`); overloads whose parameter types cannot
accept an argument are dropped. If more than one candidate is equally good,
the call stays `ambiguous` among those.

**Routes:** Spring MVC `@RestController`/`@Controller` with class-level
`@RequestMapping` and `@GetMapping/@PostMapping/@PutMapping/@DeleteMapping/@PatchMapping`
or `@RequestMapping(value/path = ..., method = RequestMethod.X)`, including
multiple paths. JAX-RS `@Path` with `@GET/@POST/...`. Without annotations:
servlets in `web.xml` and Struts/Spring XML (see
[Configuration files](#configuration-files)), `@WebServlet(urlPatterns)`,
programmatic servlet registration (`ServletContext.addServlet(...)
.addMapping(...)`, also through a `ServletRegistration` variable; Jetty
`addServlet(holder | X.class | "com.x.X", "/path")` and
`addServletWithMapping`, `ServletHolder` variables; Tomcat `addServlet` +
`addServletMapping(Decoded)`), the JDK `HttpServer.createContext("/x",
handler)`, Spark and Javalin `get("/x", ...)` / `app.post("/x", ...)`, handler
maps of front controllers (`commandMap.put("/login", new LoginCommand())`) and
`if`/`switch` dispatch on the request path. Handlers may be classes, method
references (`Users::show`, `this::list`), lambdas (indexed as
`route_handler` symbols that own the calls inside them), anonymous classes
(their `handle`/`service`/... method) or variables with a declared type.

**Exceptions:** `throw new X(...)`, rethrow of the catch parameter, multi-catch
`catch (A | B e)`, `throws` declarations. Local matching is by exact simple
name or a `catch (Exception|Throwable)`.

**Entry points:** `static void main(String[])`, `@SpringBootApplication`,
`SpringApplication.run`, `@WebListener` classes, `web.xml`.

**Not supported:** Spring component scanning and dependency injection wiring
(startup → controller connections are not traced through the container;
Spring XML `<property ref>` wiring is recorded but not used to type the
injected field), generics-based resolution (`List<User>` is treated as
`List`, so `users.get(0).name()` is not followed), calls through a stored
lambda or method reference (it is linked from the method where it is
written; `handler.apply(x)` resolves to the external functional interface),
tasks whose declared type is an interface or JDK type (`Runnable r = new
Task(); pool.submit(r)` is not linked to `Task.run()`), anonymous classes in
field initializers and static blocks (their methods look like members of the
enclosing class), reflection,
annotation processors, Lombok-generated members, `@WebFilter`, and servlet
context paths (routes are shown relative to the web application).

## C and C++

**Symbols:** namespaces (including `a::b`), classes/structs/unions (with
bodies; `typedef struct {…} Name` named by the typedef), enums, functions,
methods declared in class bodies and defined out of line (`void A::f()`,
linked through `container`), constructors, prototypes (`is_definition = 0`),
access specifiers, `static`.

**Includes:** quoted includes resolve relative to the including file, then the
project root, then by unique path suffix anywhere in the project (for
`include/` layouts), choosing the nearest file when several match.
Angle-bracket includes resolve by suffix when a project file matches, and are
otherwise external. Compiler include paths, `compile_commands.json` and the
preprocessor are **not** used, and `#ifdef`'d code is all parsed.

**Calls:** unqualified calls (enclosing class first, then functions by name;
declaration/definition pairs are merged; overloads are selected by argument
count, then by argument types as in Java: exact match over promotion
(`char` → `int`, `float` → `double`) over conversion, string literals to
`std::string`/`string_view`, `nullptr` to pointers, derived to base
pointers); `ns::f()` / `Class::f()`; `obj.f()` / `obj->f()` through declared
types of locals, parameters, catch parameters and fields (including `auto x =
X(...)`, `new X`, `std::make_unique<X>`, and `unique_ptr/shared_ptr<X>`
members); `this->f()`; `new X`; chained calls (`a.get().size()`) through
declared return types. A name-only match is **resolved** when a
declaration is visible through the file's include closure, and **heuristic**
otherwise. `std::` calls and types are external.

**Exceptions (C++):** `throw X(...)`, `throw;` (rethrow), `catch (const X&)`,
`catch (...)`. Local matching is by exact or simple name, or a
`std::exception`/`...` catch-all.

**Entry points:** `main` at global scope.

**Routes:** registration calls with a path literal and a handler
(cpp-httplib `svr.Get("/x", h)`, Pistache `Routes::Get(router, "/x",
Routes::bind(&Api::f, this))`, Drogon `registerHandler("/x", h, {Get})`,
civetweb `mg_set_request_handler`, `server.on("/x", HTTP_GET, h)` and any call
named like `route`, `addRoute`, `registerHandler`, `handle`, ...), Crow
`CROW_ROUTE(app, "/x")(...)`, route tables (`{"GET", "/users",
handle_users}` entries in arrays or maps, `routes["/x"] = h`,
`routes.emplace("/x", h)`), `if` dispatch on the request path
(`uri == "/x"`, `strcmp(req->path, "/x") == 0`, Poco-style factories
returning `new XHandler`), and handler classes named in XML/YAML
configuration. Handlers may be functions, `&Class::method`, `std::bind(...)`,
lambdas (indexed as `route_handler` symbols) or handler objects (`new X`,
`std::make_shared<X>()`, `X()`), dispatched by the generic convention above.
The HTTP library is taken from `#include`s (`httplib.h`, `crow`, `drogon`,
`pistache`, `mongoose`, `civetweb`, `microhttpd`, `cpprest`, `oatpp`,
`Poco/Net`, `boost/beast`); other registrations are `heuristic`.

**Classification heuristic:** an out-of-line definition `void Q::f()` is a
method when `Q` is a class in the same file, a constructor/destructor, or
starts with an uppercase letter. Otherwise it is a namespace function.

**Not supported:** templates beyond their names, overload resolution
involving templates or user-defined conversions, virtual dispatch beyond
`overridden_by` edges between project classes, function pointers outside
route tables and registrations, macros (other than `CROW_ROUTE`), ADL,
operator overloading as calls, and C++20 modules.

## General limitations

- Static analysis only. Callbacks, event emitters, reflection,
  dependency-injection containers, dynamic imports with computed names, and
  runtime-generated code are not followed.
- Calls on receivers of unknown type are `unresolved` unless exactly one project
  method has that name (`heuristic`) or 2–5 do (`ambiguous`, all candidates
  listed). Very common method names (`get`, `map`, `find`, …) are never
  matched by name alone.
- Routes found by heuristics (generic configuration entries, dispatch
  branches, registration calls outside a recognized library) are labeled
  `heuristic`. A route whose handler is an overloaded method named without its
  arguments (a dispatch branch calling `users.find(...)`) is `ambiguous`;
  the call itself, inside the dispatching function, is resolved precisely.
- Exception sites in callees are reported as *possible*. Enumerating every
  exception a function can raise is not attempted.
- Files over 1 MiB (configurable), binary files and symlinks are skipped and
  reported. Syntax trees deeper than 1,500 levels are cut off and reported.
- `proj refresh` updates an index incrementally and produces the same result
  as a full re-analysis. Raising an extractor's version re-parses that
  language's files on the next refresh.
