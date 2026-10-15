// Language extractors: symbols, imports, calls, routes, exceptions, entry points.
#include <algorithm>
#include <set>

#include "helpers.hpp"

using namespace proj;
using proj::test::find_call;
using proj::test::find_symbol;
using proj::test::parse;

namespace {
int count_kind(const FileFacts& f, const std::string& kind) {
  return static_cast<int>(std::count_if(f.symbols.begin(), f.symbols.end(), [&](const SymbolFact& s) { return s.kind == kind; }));
}
const ImportFact* find_import(const FileFacts& f, const std::string& spec) {
  for (const auto& i : f.imports) {
    if (i.specifier == spec) return &i;
  }
  return nullptr;
}
}  // namespace

TEST_CASE("JavaScript declarations, ranges and nesting", "[extract][js]") {
  auto r = parse(Language::JavaScript, R"(
function outer(a, b) {
  function inner() { return helper(); }
  return inner();
}
class Service extends Base {
  constructor(repo) { super(); this.repo = repo; }
  async find(id) { return this.repo.get(id); }
  static create() { return new Service(); }
}
const handler = async (req, res) => { res.send(1); };
)", "src/svc.js");
  REQUIRE(r.status == "parsed");
  const auto& f = r.facts;
  REQUIRE(f.symbols[0].kind == "module");
  auto* outer = find_symbol(f, "outer");
  REQUIRE(outer);
  CHECK(outer->kind == "function");
  CHECK(outer->span.start.line == 2);
  CHECK(outer->span.end.line == 5);
  CHECK(outer->param_count == 2);
  auto* inner = find_symbol(f, "outer.inner");
  REQUIRE(inner);
  CHECK(f.symbols[static_cast<size_t>(inner->parent)].name == "outer");
  auto* find = find_symbol(f, "Service.find");
  REQUIRE(find);
  CHECK(find->kind == "method");
  CHECK(find_symbol(f, "Service.constructor")->kind == "constructor");
  CHECK(find_symbol(f, "Service.create")->is_static);
  CHECK(find_symbol(f, "handler")->kind == "function");
  REQUIRE(f.inherits.size() == 1);
  CHECK(f.inherits[0].base == "Base");
  auto* call = find_call(f, "get");
  REQUIRE(call);
  CHECK(call->receiver_kind == "this_member");
  CHECK(call->receiver == "repo");
  CHECK(f.symbols[static_cast<size_t>(call->caller)].qualified_name == "Service.find");
  auto* ctor_call = find_call(f, "Service");
  REQUIRE(ctor_call);
  CHECK(ctor_call->kind == "instantiates");
  auto* super_call = find_call(f, "constructor");
  REQUIRE(super_call);
  CHECK(super_call->receiver_kind == "super");
}

TEST_CASE("JavaScript ES module and CommonJS imports and exports", "[extract][js]") {
  auto r = parse(Language::JavaScript, R"(
import def, { a, b as c } from './lib';
import * as ns from '../ns';
const fs = require('fs');
const { x, y: z } = require('./util');
const only = require('./m').only;
export function exported() {}
export { a as renamed };
export * from './all';
module.exports.extra = function () {};
exports.other = exported;
)", "src/index.js");
  const auto& f = r.facts;
  auto* lib = find_import(f, "./lib");
  REQUIRE(lib);
  REQUIRE(lib->bindings.size() == 3);
  CHECK(lib->bindings[0].kind == "default");
  CHECK(lib->bindings[2].local == "c");
  CHECK(lib->bindings[2].imported == "b");
  CHECK(find_import(f, "../ns")->bindings[0].kind == "namespace");
  CHECK(find_import(f, "fs")->bindings[0].kind == "module");
  auto* util = find_import(f, "./util");
  REQUIRE(util->bindings.size() == 2);
  CHECK(util->bindings[1].local == "z");
  CHECK(util->bindings[1].imported == "y");
  CHECK(find_import(f, "./m")->bindings[0].imported == "only");
  CHECK(find_import(f, "./all")->kind == "reexport");
  auto has_export = [&](const std::string& name) {
    return std::any_of(f.exports.begin(), f.exports.end(), [&](const ExportFact& e) { return e.exported == name; });
  };
  CHECK(has_export("exported"));
  CHECK(has_export("renamed"));
  CHECK(has_export("*"));
  CHECK(has_export("extra"));
  CHECK(has_export("other"));
  CHECK(find_symbol(f, "extra"));
}

TEST_CASE("Express routes, router mounts and middleware are recognized syntactically", "[extract][js][routes]") {
  auto r = parse(Language::JavaScript, R"(
const express = require('express');
const app = express();
const router = express.Router();
router.get('/:id', auth, controller.show);
router.post('/', asyncHandler(async (req, res) => { save(req.body); }));
router.route('/items').get(listItems).put(updateItem);
app.use('/api', router);
app.use(logger);
app.listen(3000);
axios.get('/users', { params: {} });
cache.get('key');
)", "src/app.js");
  const auto& f = r.facts;
  REQUIRE(f.routers.size() == 2);
  REQUIRE(f.routes.size() == 4);
  const auto& show = f.routes[0];
  CHECK(show.method == "GET");
  CHECK(show.path == "/:id");
  CHECK(show.registrar == "router");
  CHECK(show.confidence == "recognized");
  CHECK(show.handler.kind == "member");
  CHECK(show.handler.receiver == "controller");
  CHECK(show.handler.name == "show");
  REQUIRE(show.middleware.size() == 1);
  CHECK(show.middleware[0].name == "auth");
  const auto& post = f.routes[1];
  CHECK(post.handler.kind == "inline");
  CHECK(post.handler.wrapper == "asyncHandler");
  REQUIRE(post.handler.symbol > 0);
  CHECK(f.symbols[static_cast<size_t>(post.handler.symbol)].kind == "route_handler");
  // Calls inside the inline handler are attributed to the handler symbol.
  auto* save = find_call(f, "save");
  REQUIRE(save);
  CHECK(save->caller == post.handler.symbol);
  // router.route('/items').get(...).put(...): the outer call (PUT) is visited first.
  std::set<std::string> chain;
  for (const auto& r : f.routes) {
    if (r.path == "/items") chain.insert(r.method);
  }
  CHECK(chain == std::set<std::string>{"GET", "PUT"});
  REQUIRE(f.mounts.size() == 2);
  CHECK(f.mounts[0].prefix == "/api");
  CHECK(f.mounts[0].target.name == "router");
  CHECK_FALSE(f.mounts[1].has_prefix);
  REQUIRE(f.entries.size() == 1);
  CHECK(f.entries[0].kind == "server_start");
}

TEST_CASE("NestJS controller decorators produce routes", "[extract][ts][routes]") {
  auto r = parse(Language::TypeScript, R"(
@Controller('users')
export class UsersController {
  constructor(private readonly users: UsersService) {}
  @Get(':id')
  findOne(@Param('id') id: string) { return this.users.find(id); }
  @Post()
  create() {}
}
)", "src/users.controller.ts");
  const auto& f = r.facts;
  REQUIRE(f.routes.size() == 2);
  CHECK(f.routes[0].framework == "nestjs");
  CHECK(f.routes[0].method == "GET");
  CHECK(f.routes[0].prefix == "/users");
  CHECK(f.routes[0].path == "/:id");
  CHECK(f.symbols[static_cast<size_t>(f.routes[0].handler.symbol)].qualified_name == "UsersController.findOne");
  bool field_hint = std::any_of(f.type_hints.begin(), f.type_hints.end(), [](const TypeHintFact& h) {
    return h.name == "users" && h.type == "UsersService" && h.kind == "field";
  });
  CHECK(field_hint);
}

TEST_CASE("JavaScript throw sites are linked to enclosing local handlers", "[extract][js][exceptions]") {
  auto r = parse(Language::JavaScript, R"(
function f() {
  try {
    throw new NotFound('x');
  } catch (err) {
    throw err;
  }
  throw new Other();
}
function g() {
  try { const h = () => { throw new Inner(); }; } catch (e) {}
}
)", "src/e.js");
  const auto& ex = r.facts.exceptions;
  auto find = [&](const std::string& kind, const std::string& type) -> const ExceptionFact* {
    for (const auto& e : ex) {
      if (e.kind == kind && e.type == type) return &e;
    }
    return nullptr;
  };
  auto* inner = find("throw", "NotFound");
  REQUIRE(inner);
  REQUIRE(inner->handler >= 0);
  CHECK(ex[static_cast<size_t>(inner->handler)].kind == "catch");
  CHECK(inner->handler_match == "catch_all");
  auto* rethrow = find("rethrow", "");
  REQUIRE(rethrow);
  CHECK(rethrow->handler < 0);  // throw inside the catch block is not handled by that try
  REQUIRE(find("throw", "Other"));
  CHECK(find("throw", "Other")->handler < 0);
  // A throw in a nested function is not caught by the try around its definition.
  auto* nested = find("throw", "Inner");
  REQUIRE(nested);
  CHECK(nested->handler < 0);
}

TEST_CASE("malformed JavaScript is reported but still yields facts", "[extract][js]") {
  auto r = parse(Language::JavaScript, "function broken( {\n  return 42\n}\n\nfunction stillIndexed() {\n  return 1;\n}\n");
  CHECK(r.status == "syntax_errors");
  CHECK(r.error_count >= 1);
  CHECK(r.first_error_line >= 1);
  CHECK(find_symbol(r.facts, "stillIndexed"));
}

TEST_CASE("Python declarations, imports, routes, exceptions and main guard", "[extract][python]") {
  auto r = parse(Language::Python, R"(
from flask import Blueprint
from .models import User as U
import os.path
from pkg import *

bp = Blueprint("users", __name__, url_prefix="/users")

class Service(Base):
    def __init__(self):
        self.repo = Repo()

    def get(self, user_id: int):
        try:
            return self.repo.find(user_id)
        except (KeyError, ValueError) as e:
            raise NotFound(user_id) from e

@bp.route("/<int:user_id>", methods=["GET", "DELETE"])
@login_required
def show(user_id):
    return Service().get(user_id)

if __name__ == "__main__":
    show(1)
)", "app/views.py");
  REQUIRE(r.status == "parsed");
  const auto& f = r.facts;
  CHECK(f.module_name == "app.views");
  CHECK(find_symbol(f, "app.views.Service")->kind == "class");
  CHECK(find_symbol(f, "app.views.Service.__init__")->kind == "constructor");
  CHECK(find_symbol(f, "app.views.Service.get")->param_count == 1);
  auto* rel = find_import(f, ".models");
  REQUIRE(rel);
  CHECK(rel->bindings[0].local == "U");
  CHECK(rel->bindings[0].imported == "User");
  CHECK(find_import(f, "os.path")->bindings[0].local == "os.path");
  CHECK(find_import(f, "pkg")->bindings[0].kind == "wildcard");
  REQUIRE(f.routers.size() == 1);
  CHECK(f.routers[0].kind == "blueprint");
  CHECK(f.routers[0].prefix == "/users");
  REQUIRE(f.routes.size() == 2);
  CHECK(f.routes[0].method == "GET");
  CHECK(f.routes[1].method == "DELETE");
  CHECK(f.routes[0].path == "/<int:user_id>");
  CHECK(f.symbols[static_cast<size_t>(f.routes[0].handler.symbol)].name == "show");
  REQUIRE(f.routes[0].middleware.size() == 1);
  CHECK(f.routes[0].middleware[0].name == "login_required");
  auto* find = find_call(f, "find");
  REQUIRE(find);
  CHECK(find->receiver_kind == "this_member");
  CHECK(find->receiver == "repo");
  bool repo_hint = std::any_of(f.type_hints.begin(), f.type_hints.end(),
                               [](const TypeHintFact& h) { return h.name == "repo" && h.type == "Repo" && h.kind == "field"; });
  CHECK(repo_hint);
  int catches = 0;
  for (const auto& e : f.exceptions) catches += e.kind == "catch";
  CHECK(catches == 2);  // KeyError and ValueError
  bool raise_found = std::any_of(f.exceptions.begin(), f.exceptions.end(),
                                 [](const ExceptionFact& e) { return e.kind == "throw" && e.type == "NotFound"; });
  CHECK(raise_found);
  REQUIRE(f.entries.size() == 1);
  CHECK(f.entries[0].kind == "main_guard");
}

TEST_CASE("Python raise is matched against except types", "[extract][python][exceptions]") {
  auto r = parse(Language::Python, R"(
def f():
    try:
        raise KeyError("k")
    except KeyError:
        pass
    try:
        raise ValueError("v")
    except TypeError:
        pass
    try:
        raise Custom()
    except Exception:
        raise
)", "m.py");
  const auto& ex = r.facts.exceptions;
  auto throw_of = [&](const std::string& type) {
    for (const auto& e : ex) {
      if (e.kind == "throw" && e.type == type) return &e;
    }
    return static_cast<const ExceptionFact*>(nullptr);
  };
  REQUIRE(throw_of("KeyError"));
  CHECK(throw_of("KeyError")->handler_match == "exact");
  REQUIRE(throw_of("ValueError"));
  CHECK(throw_of("ValueError")->handler < 0);
  REQUIRE(throw_of("Custom"));
  CHECK(throw_of("Custom")->handler_match == "catch_all");
  CHECK(std::any_of(ex.begin(), ex.end(), [](const ExceptionFact& e) { return e.kind == "rethrow"; }));
}

TEST_CASE("Java classes, overloads, Spring mappings and throws", "[extract][java]") {
  auto r = parse(Language::Java, R"(
package com.acme.web;

import com.acme.service.UserService;
import java.util.*;

@RestController
@RequestMapping("/api/users")
public class UserController extends BaseController implements Api {
    private final UserService service;

    @GetMapping("/{id}")
    public User get(@PathVariable Long id) throws NotFoundException {
        try {
            return service.find(id);
        } catch (IllegalStateException | NotFoundException e) {
            throw new ApiException(e);
        }
    }

    @RequestMapping(value = {"/a", "/b"}, method = RequestMethod.POST)
    public void post() { this.helper(1); helper(1, 2); }

    private void helper(int a) {}
    private void helper(int a, int b) {}

    public static void main(String[] args) { new UserController(); }
}
)", "src/main/java/com/acme/web/UserController.java");
  REQUIRE(r.status == "parsed");
  const auto& f = r.facts;
  CHECK(f.module_name == "com.acme.web");
  auto* cls = find_symbol(f, "com.acme.web.UserController");
  REQUIRE(cls);
  CHECK(cls->exported);
  CHECK(count_kind(f, "method") == 5);
  CHECK(find_import(f, "com.acme.service.UserService")->bindings[0].local == "UserService");
  CHECK(find_import(f, "java.util.*")->bindings.empty());
  REQUIRE(f.inherits.size() == 2);
  CHECK(f.inherits[0].kind == "extends");
  CHECK(f.inherits[1].kind == "implements");
  REQUIRE(f.routes.size() == 3);
  CHECK(f.routes[0].method == "GET");
  CHECK(f.routes[0].prefix == "/api/users");
  CHECK(f.routes[0].path == "/{id}");
  CHECK(f.routes[1].method == "POST");
  CHECK(f.routes[1].path == "/a");
  CHECK(f.routes[2].path == "/b");
  bool field = std::any_of(f.type_hints.begin(), f.type_hints.end(),
                           [](const TypeHintFact& h) { return h.name == "service" && h.type == "UserService"; });
  CHECK(field);
  auto* find = find_call(f, "find");
  REQUIRE(find);
  CHECK(find->receiver == "service");
  CHECK(find->arg_count == 1);
  int declares = 0, catches = 0;
  for (const auto& e : f.exceptions) {
    declares += e.kind == "declares";
    catches += e.kind == "catch";
  }
  CHECK(declares == 1);
  CHECK(catches == 2);
  bool main_entry = std::any_of(f.entries.begin(), f.entries.end(), [](const EntryFact& e) { return e.kind == "main_function"; });
  CHECK(main_entry);
}

TEST_CASE("Java method references are calls from the enclosing method", "[extract][java]") {
  auto r = parse(Language::Java, R"(
package a;
import static spark.Spark.get;
public class Jobs extends Base {
    private Repo repo;
    void start(Executor pool, Worker w) {
        pool.execute(this::drain);
        list.forEach(Util::print);
        pool.submit(w::work);
        pool.submit(this.repo::flush);
        pool.submit(super::stop);
        pool.submit(factory()::build);
        Supplier<Repo> s = Repo::new;
        IntFunction<int[]> arrays = int[]::new;
        get("/jobs", Handlers::list);
    }
}
)", "src/a/Jobs.java");
  REQUIRE(r.status == "parsed");
  const auto& f = r.facts;
  auto by_text = [&](const std::string& t) -> const CallFact* {
    for (const auto& c : f.calls) {
      if (c.text == t) return &c;
    }
    return nullptr;
  };
  auto* start = find_symbol(f, "a.Jobs.start");
  REQUIRE(start);
  auto* drain = by_text("this::drain");
  REQUIRE(drain);
  CHECK(drain->kind == "calls");
  CHECK(drain->name == "drain");
  CHECK(drain->receiver_kind == "this");
  CHECK(drain->arg_count == -1);  // not visible: overloads stay ambiguous
  CHECK(f.symbols[static_cast<size_t>(drain->caller)].qualified_name == "a.Jobs.start");
  REQUIRE(by_text("Util::print"));
  CHECK(by_text("Util::print")->receiver_kind == "identifier");
  CHECK(by_text("Util::print")->receiver == "Util");
  REQUIRE(by_text("w::work"));
  CHECK(by_text("w::work")->receiver == "w");
  REQUIRE(by_text("this.repo::flush"));
  CHECK(by_text("this.repo::flush")->receiver_kind == "this_member");
  CHECK(by_text("this.repo::flush")->receiver == "repo");
  REQUIRE(by_text("super::stop"));
  CHECK(by_text("super::stop")->receiver_kind == "super");
  // factory()::build: the receiver is the value returned by factory().
  auto* build = by_text("factory()::build");
  REQUIRE(build);
  CHECK(build->receiver_kind == "call");
  REQUIRE(build->receiver_call >= 0);
  CHECK(f.calls[static_cast<size_t>(build->receiver_call)].name == "factory");
  auto* ctor = by_text("Repo::new");
  REQUIRE(ctor);
  CHECK(ctor->kind == "instantiates");
  CHECK(ctor->name == "Repo");
  CHECK_FALSE(by_text("int[]::new"));
  // A route handler reference is recorded with its route, not as a call.
  REQUIRE(f.routes.size() == 1);
  CHECK_FALSE(by_text("Handlers::list"));
}

TEST_CASE("C++ namespaces, out-of-line methods, includes and catch handlers", "[extract][cpp]") {
  auto r = parse(Language::Cpp, R"(
#include "store/user_store.h"
#include <vector>

namespace store {

class Cache : public Base {
 public:
  int size() const;
 private:
  std::vector<int> items_;
};

int Cache::size() const { return static_cast<int>(items_.size()); }

void run(Cache* cache) {
  try {
    cache->size();
    helper::log("x");
    throw std::runtime_error("bad");
  } catch (const std::exception& e) {
  }
}

}  // namespace store

int main(int argc, char** argv) { return 0; }
)", "src/cache.cpp");
  REQUIRE(r.status == "parsed");
  const auto& f = r.facts;
  REQUIRE(f.imports.size() == 2);
  CHECK(f.imports[0].specifier == "store/user_store.h");
  CHECK_FALSE(f.imports[0].system);
  CHECK(f.imports[1].system);
  CHECK(find_symbol(f, "store")->kind == "namespace");
  CHECK(find_symbol(f, "store::Cache")->kind == "class");
  auto* decl = find_symbol(f, "store::Cache::size");
  REQUIRE(decl);
  // Declaration in the class body and the out-of-line definition are both indexed.
  int sizes = 0, defs = 0;
  for (const auto& s : f.symbols) {
    if (s.qualified_name == "store::Cache::size") {
      ++sizes;
      defs += s.is_definition;
    }
  }
  CHECK(sizes == 2);
  CHECK(defs == 1);
  const CallFact* call = nullptr;
  for (const auto& c : f.calls) {
    if (c.name == "size" && c.receiver == "cache") call = &c;
  }
  REQUIRE(call);
  CHECK(call->receiver_kind == "identifier");
  CHECK(f.symbols[static_cast<size_t>(call->caller)].qualified_name == "store::run");
  auto* qualified = find_call(f, "log");
  REQUIRE(qualified);
  CHECK(qualified->receiver_kind == "qualified");
  CHECK(qualified->receiver == "helper");
  bool caught = std::any_of(f.exceptions.begin(), f.exceptions.end(), [](const ExceptionFact& e) {
    return e.kind == "throw" && e.type == "std::runtime_error" && e.handler_match == "catch_all";
  });
  CHECK(caught);
  CHECK(std::any_of(f.entries.begin(), f.entries.end(), [](const EntryFact& e) { return e.kind == "main_function"; }));
  CHECK(f.inherits.size() == 1);
}

TEST_CASE("C files use the C grammar", "[extract][c]") {
  auto r = parse(Language::C, "#include <stdio.h>\nstatic int helper(int x) { return x; }\nint compute(int v) { return helper(v); }\n",
                 "legacy.c");
  REQUIRE(r.status == "parsed");
  CHECK(find_symbol(r.facts, "helper")->is_static);
  CHECK(find_call(r.facts, "helper"));
}

TEST_CASE("deeply nested code is bounded rather than overflowing the stack", "[extract][limits]") {
  std::string code = "const x = ";
  for (int i = 0; i < 3000; ++i) code += "[";
  for (int i = 0; i < 3000; ++i) code += "]";
  code += ";\nfunction after() {}\n";
  auto r = parse(Language::JavaScript, code, "deep.js");
  CHECK(r.status != "parse_failed");
  CHECK(r.facts.depth_limited);
  CHECK(find_symbol(r.facts, "after"));
}
