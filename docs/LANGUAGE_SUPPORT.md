# Language support

Parsing uses Tree-sitter 0.25.10 with these grammars, compiled into the
engine: JavaScript 0.25.0, TypeScript/TSX 0.23.2, Python 0.25.0, Java 0.23.5,
C++ 0.23.4, C 0.24.1.

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

All other files are recorded as `unsupported`. Their content is not read.

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
apply); Django `path()`/`re_path()` entries in `urlpatterns` (method `ANY`;
`include()` prefixes are **not** resolved). Other decorators on a view
(`@login_required`) are recorded as route middleware.

**Exceptions:** `raise X(...)`, `raise X`, bare `raise` and re-raise of the
`except ... as e` variable (rethrow). `except` clauses with single, tuple
and bare types. A raise is linked to a local handler when the names match
exactly or the handler catches `Exception`/`BaseException`/everything.
Class hierarchies are not used for matching.

**Entry points:** `if __name__ == "__main__":` (top level), `__main__.py`,
`app.run()` on a recognized app, `uvicorn.run()`.

**Not supported:** dynamic attribute access, decorators that replace
functions (other than route registration), metaclasses, `getattr`-based
dispatch, Django class-based view dispatch beyond the class itself.

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
Chained calls (`repo.find(1).name()`) resolve through declared return types,
and so do `var u = repo.find(3)` locals. `new X().m()` and `((X) o).m()` use
`X`. Untyped lambda parameters shadow outer names and stay unresolved.
Methods inherited from an external superclass (`extends HashMap`) are
`external`. When an external base class and a static wildcard import could
both supply the method, the note names both. Overloads are selected by
argument count, and remaining ties are recorded as `ambiguous`. Calls on
interfaces resolve to the interface method, with `overridden_by` edges to
implementations (possible dynamic dispatch).

**Routes:** Spring MVC `@RestController`/`@Controller` with class-level
`@RequestMapping` and `@GetMapping/@PostMapping/@PutMapping/@DeleteMapping/@PatchMapping`
or `@RequestMapping(value/path = ..., method = RequestMethod.X)`, including
multiple paths. JAX-RS `@Path` with `@GET/@POST/...`.

**Exceptions:** `throw new X(...)`, rethrow of the catch parameter, multi-catch
`catch (A | B e)`, `throws` declarations. Local matching is by exact simple
name or a `catch (Exception|Throwable)`.

**Entry points:** `static void main(String[])`, `@SpringBootApplication`,
`SpringApplication.run`.

**Not supported:** Spring component scanning and dependency injection wiring
(startup → controller connections are not traced through the container),
generics-based resolution (`List<User>` is treated as `List`, so
`users.get(0).name()` is not followed), lambdas and method references as
call targets, reflection, annotation processors and Lombok-generated
members.

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
declaration/definition pairs are merged and overloads are filtered by argument
count); `ns::f()` / `Class::f()`; `obj.f()` / `obj->f()` through declared
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

**Classification heuristic:** an out-of-line definition `void Q::f()` is a
method when `Q` is a class in the same file, a constructor/destructor, or
starts with an uppercase letter. Otherwise it is a namespace function.

**Not supported:** templates beyond their names, overload resolution by
types, virtual dispatch beyond `overridden_by` edges between project classes,
function pointers, macros, ADL, operator overloading as calls, and C++20
modules. No HTTP routes are extracted for C/C++.

## General limitations

- Static analysis only. Callbacks, event emitters, reflection,
  dependency-injection containers, dynamic imports with computed names, and
  runtime-generated code are not followed.
- Calls on receivers of unknown type are `unresolved` unless exactly one project
  method has that name (`heuristic`) or 2–5 do (`ambiguous`, all candidates
  listed). Very common method names (`get`, `map`, `find`, …) are never
  matched by name alone.
- Exception sites in callees are reported as *possible*. Enumerating every
  exception a function can raise is not attempted.
- Files over 1 MiB (configurable), binary files and symlinks are skipped and
  reported. Syntax trees deeper than 1,500 levels are cut off and reported.
- `proj refresh` updates an index incrementally and produces the same result
  as a full re-analysis. Raising an extractor's version re-parses that
  language's files on the next refresh.
