// Schema, registry, end-to-end indexing of fixtures, queries and the protocol.
#include <map>
#include <set>

#include "core/sysinfo.hpp"
#include "core/version.hpp"
#include "db/schema.hpp"
#include "helpers.hpp"
#include "index/indexer.hpp"
#include "index/registry.hpp"
#include "query/query.hpp"

using namespace proj;
using proj::test::call;
using proj::test::fixture;
using proj::test::TempDir;
namespace fs = std::filesystem;

namespace {

// Analyzes a fixture once per test binary run and shares the data directory.
struct IndexedFixture {
  TempDir dir;
  json add;
  fs::path index_path;
};

IndexedFixture& indexed(const std::string& name) {
  static std::map<std::string, std::unique_ptr<IndexedFixture>> cache;
  auto& slot = cache[name];
  if (!slot) {
    slot = std::make_unique<IndexedFixture>();
    protocol::Engine engine;
    slot->add = call(engine, "add_project", {{"name", name}, {"path", fixture(name).string()}}, slot->dir.path());
    REQUIRE(slot->add["result"].is_object());
    slot->index_path = slot->add["result"]["index_path"].get<std::string>();
  }
  return *slot;
}

IndexReader reader(const std::string& name) { return IndexReader(indexed(name).index_path); }

int64_t symbol_id(db::Database& db, const std::string& qualified, const std::string& path_suffix = "") {
  auto st = db.prepare(
      "SELECT s.id, f.path FROM symbols s JOIN files f ON f.id = s.file_id WHERE s.qualified_name = ? ORDER BY s.id");
  st.bind(1, qualified);
  while (st.step()) {
    std::string p = st.text(1);
    if (path_suffix.empty() || (p.size() >= path_suffix.size() && p.compare(p.size() - path_suffix.size(), path_suffix.size(), path_suffix) == 0)) {
      return st.int64(0);
    }
  }
  return -1;
}

db::Database open(const std::string& name) { return db::Database(indexed(name).index_path, db::OpenMode::ReadOnly); }

}  // namespace

// ---------------------------------------------------------------------------
// Schema and registry
// ---------------------------------------------------------------------------

TEST_CASE("index schema initializes and is version-checked", "[schema]") {
  TempDir tmp;
  db::Database db(tmp.path() / "i.db", db::OpenMode::ReadWriteCreate);
  db::create_index_schema(db);
  CHECK(db::check_index_schema(db) == kIndexSchemaVersion);
  db.exec("UPDATE index_meta SET value = '999' WHERE key = 'schema_version'");
  CHECK_THROWS_AS(db::check_index_schema(db), db::SchemaError);
  db.exec("UPDATE index_meta SET value = '0' WHERE key = 'schema_version'");
  CHECK_THROWS_WITH(db::check_index_schema(db), Catch::Matchers::ContainsSubstring("--reanalyze"));
  db::Database empty(tmp.path() / "empty.db", db::OpenMode::ReadWriteCreate);
  CHECK_THROWS_AS(db::check_index_schema(empty), db::SchemaError);
}

TEST_CASE("registry migrations upgrade in place and keep data", "[schema][registry]") {
  TempDir tmp;
  {
    db::Database db(tmp.path() / "registry.db", db::OpenMode::ReadWriteCreate);
    CHECK(db::registry_schema_version(db) == 0);
    db::migrate_registry(db);
    CHECK(db::registry_schema_version(db) == kRegistrySchemaVersion);
    db.exec("INSERT INTO projects(name, root_path, index_path, created_at, status) VALUES ('p','/r','/i','t','ok')");
  }
  {
    db::Database db(tmp.path() / "registry.db", db::OpenMode::ReadWrite);
    db::migrate_registry(db);  // idempotent
    CHECK(db.query_int("SELECT COUNT(*) FROM projects") == 1);
    db.exec("PRAGMA user_version = 99");
    CHECK_THROWS_AS(db::migrate_registry(db), db::SchemaError);
    CHECK(db.query_int("SELECT COUNT(*) FROM projects") == 1);  // nothing destroyed
  }
}

TEST_CASE("registry records runs, prunes history and detects interrupted runs", "[registry]") {
  TempDir tmp;
  Registry reg(tmp.path());
  auto p = reg.create_project("demo", "/some/root", json::object());
  CHECK(p.index_path.find("project-") != std::string::npos);
  CHECK(reg.find_by_root("/some/root")->name == "demo");
  for (int i = 0; i < Registry::kRunsKeptPerProject + 5; ++i) {
    int64_t run = reg.start_run(p);
    DiagnosticSink sink(10);
    sink.add({"warn", "source", "syntax_error", "a.js", 3, "bad"});
    reg.finish_run(run, p.id, "partial", 1.0, RunCounts{}, json::object(), "", sink, true);
  }
  auto runs = reg.list_runs(p.id, 200);
  CHECK(runs.size() == Registry::kRunsKeptPerProject);
  auto detail = reg.get_run(runs[0]["id"], 10, 0);
  REQUIRE(detail);
  CHECK((*detail)["diagnostics"].size() == 1);
  CHECK((*detail)["diagnostics"][0]["line"] == 3);

  // A run whose process no longer exists is reported as interrupted.
  int64_t run = reg.start_run(p);
  db::Database raw(tmp.path() / "registry.db", db::OpenMode::ReadWrite);
  auto st = raw.prepare("UPDATE analysis_runs SET pid = 999999999 WHERE id = ?");
  st.bind(1, run).run();
  CHECK(reg.active_run(p.id) == std::nullopt);
  reg.mark_interrupted_runs();
  CHECK(reg.get_run(run, 10, 0).value()["status"] == "interrupted");
  CHECK(reg.find_by_name("demo")->status == "interrupted");
}

// ---------------------------------------------------------------------------
// End-to-end indexing of the Express fixture
// ---------------------------------------------------------------------------

TEST_CASE("express fixture: analysis summary is accurate and partial", "[index][express]") {
  auto& fx = indexed("express-api");
  const auto& res = fx.add["result"];
  CHECK(fx.add["status"] == "partial");  // src/broken.js has a syntax error
  CHECK(res["status"] == "partial");
  const auto& files = res["stats"]["files"];
  CHECK(files["discovered"] == 30);
  CHECK(files["with_syntax_errors"] == 1);
  CHECK(files["unsupported"] == 3);   // .gitignore, package.json, scripts/deploy.rb
  CHECK(files["excluded"] == 1);      // debug.log
  CHECK(files["by_language"]["typescript"] == 3);
  CHECK(res["counts"]["parse_errors"] == 1);
  CHECK(res["stats"]["routes"] == 7);
  CHECK(fs::exists(fx.index_path));
  CHECK(fx.index_path.string().find(fixture("express-api").string()) == std::string::npos);  // never inside the repo
}

TEST_CASE("express fixture: file statuses distinguish parse outcomes", "[index][express]") {
  auto db = open("express-api");
  auto status = [&](const std::string& path) {
    auto st = db.prepare("SELECT status FROM files WHERE path = ?");
    st.bind(1, path);
    return st.step() ? st.text(0) : std::string("<missing>");
  };
  CHECK(status("src/app.js") == "parsed");
  CHECK(status("src/broken.js") == "syntax_errors");
  CHECK(status("scripts/deploy.rb") == "unsupported");
  CHECK(status("debug.log") == "excluded");
  CHECK(status("dist/app.js") == "<missing>");
  CHECK(status("generated/bundle.js") == "<missing>");
  auto fp = db.prepare("SELECT fingerprint, line_count FROM files WHERE path = 'src/app.js'");
  REQUIRE(fp.step());
  CHECK(fp.text(0).size() == 16);
  CHECK(fp.int64(1) == 20);
}

TEST_CASE("express fixture: dependencies are resolved, external or unresolved", "[index][express]") {
  auto db = open("express-api");
  auto dep = [&](const std::string& src, const std::string& spec) {
    auto st = db.prepare(
        "SELECT d.status, COALESCE(f2.path, '') FROM file_deps d JOIN files f ON f.id = d.src_file_id "
        "LEFT JOIN files f2 ON f2.id = d.dst_file_id WHERE f.path = ? AND d.specifier = ?");
    st.bind(1, src).bind(2, spec);
    REQUIRE(st.step());
    return std::make_pair(st.text(0), st.text(1));
  };
  CHECK(dep("src/app.js", "./routes/users") == std::make_pair(std::string("resolved"), std::string("src/routes/users.js")));
  CHECK(dep("src/app.js", "./routes/orders") == std::make_pair(std::string("resolved"), std::string("src/routes/orders.ts")));
  CHECK(dep("src/app.js", "express").first == "external");
  CHECK(dep("src/legacy.js", "./does-not-exist").first == "unresolved");
  CHECK(dep("src/repositories/orderRepository.ts", "../services/orderService").second == "src/services/orderService.ts");
}

TEST_CASE("express fixture: controller -> service -> repository chain is resolved", "[index][express][calls]") {
  auto db = open("express-api");
  auto target_of = [&](const std::string& caller, const std::string& text) {
    auto st = db.prepare(
        "SELECT e.status, COALESCE(t.qualified_name, ''), COALESCE(tf.path, '') FROM edges e "
        "JOIN symbols s ON s.id = e.src_symbol_id LEFT JOIN symbols t ON t.id = e.dst_symbol_id "
        "LEFT JOIN files tf ON tf.id = t.file_id WHERE s.qualified_name = ? AND e.target_text = ?");
    st.bind(1, caller).bind(2, text);
    REQUIRE(st.step());
    return std::make_tuple(st.text(0), st.text(1), st.text(2));
  };
  CHECK(target_of("getUser", "userService.findById") ==
        std::make_tuple(std::string("resolved"), std::string("UserService.findById"), std::string("src/services/userService.js")));
  CHECK(std::get<1>(target_of("UserService.findById", "this.repository.findById")) == "UserRepository.findById");
  CHECK(std::get<1>(target_of("UserRepository.findById", "db.query")) == "query");
  CHECK(std::get<0>(target_of("query", "pool.query")) == "external");
  CHECK(std::get<1>(target_of("OrderService.list", "this.repo.all")) == "OrderRepository.all");
  // Unresolved and ambiguous relationships stay visible with their reason.
  CHECK(std::get<0>(target_of("getUser", "missing.lookup")) == "unresolved");
  CHECK(std::get<0>(target_of("LegacySync.sync", "this.store.findById")) == "ambiguous");
  auto cands = db.prepare(
      "SELECT COUNT(*) FROM edge_candidates c JOIN edges e ON e.id = c.edge_id WHERE e.target_text = 'this.store.findById'");
  REQUIRE(cands.step());
  CHECK(cands.int64(0) == 2);
}

TEST_CASE("express fixture: routes have resolved handlers, prefixes and middleware", "[index][express][routes]") {
  auto r = reader("express-api");
  auto found = r.search("GET /api/users/:id", {"route"}, false, 10);
  REQUIRE(found["routes"].size() >= 1);
  auto route = found["routes"][0];
  CHECK(route["full_path"] == "/api/users/:id");
  CHECK(route["handler_status"] == "resolved");
  // A concrete URL matches the parameterized route.
  CHECK(r.search("GET /api/users/42", {"route"}, false, 10)["routes"][0]["full_path"] == "/api/users/:id");

  TraverseOptions t;
  t.max_depth = 6;
  auto flow = r.route_flow(route["id"].get<int64_t>(), t);
  CHECK(flow["route"]["handler"]["symbol"]["qualified_name"] == "getUser");
  CHECK(flow["route"]["mount"]["prefix"] == "/api/users");
  std::vector<std::string> middleware;
  for (const auto& m : flow["route"]["middleware"]) middleware.push_back(m["text"]);
  CHECK(middleware == std::vector<std::string>{"express.json()", "requestLogger", "authenticate"});
  std::set<std::string> reached;
  for (const auto& n : flow["flow"]["nodes"]) {
    if (!n["symbol"].is_null()) reached.insert(n["symbol"]["qualified_name"]);
  }
  CHECK(reached.count("UserService.findById"));
  CHECK(reached.count("UserRepository.findById"));
  CHECK(reached.count("query"));
  CHECK(reached.count("formatUser"));
  // Exceptions: the handler catches locally; NotFoundError may come from a callee.
  CHECK(flow["exceptions"]["handlers"].size() == 1);
  bool not_found = false;
  for (const auto& x : flow["exceptions"]["in_callees"]) not_found |= x["type"] == "NotFoundError";
  CHECK(not_found);
  // Startup path from the package.json entry to the router module.
  REQUIRE(flow["startup"]["found"] == true);
  CHECK(flow["startup"]["path"][0]["symbol"]["path"] == "src/server.js");

  auto all = r.routes(100, 0)["routes"];
  std::set<std::string> paths;
  for (const auto& x : all) paths.insert(x["method"].get<std::string>() + " " + x["full_path"].get<std::string>());
  CHECK(paths.count("GET /health"));
  CHECK(paths.count("POST /api/users"));
  CHECK(paths.count("DELETE /api/users/:id"));
  CHECK(paths.count("GET /api/orders/:orderId"));
  bool dynamic_unmounted = false;
  for (const auto& x : all) dynamic_unmounted |= x["full_path"].get<std::string>().rfind("<dynamic", 0) == 0 && x["mount_status"] == "unmounted";
  CHECK(dynamic_unmounted);
}

TEST_CASE("express fixture: duplicate names stay distinguishable", "[index][express][search]") {
  auto r = reader("express-api");
  auto res = r.search("format", {"symbol"}, true, 20)["symbols"];
  std::set<std::string> paths;
  for (const auto& s : res) {
    if (s["name"] == "format") {
      paths.insert(s["path"]);
      CHECK(s["duplicate_name"] == true);
    }
  }
  CHECK(paths == std::set<std::string>{"src/utils/date.js", "src/utils/text.js"});
  auto get_user = r.search("getUser", {"symbol"}, true, 20)["symbols"];
  CHECK(get_user.size() == 2);  // controller and legacy module
  // Case-insensitive substring search uses the trigram index.
  auto sub = r.search("finDBy", {"symbol"}, false, 20)["symbols"];
  CHECK(sub.size() >= 2);
  auto files = r.search("userController", {"file"}, false, 5)["files"];
  REQUIRE(files.size() >= 1);
  CHECK(files[0]["path"] == "src/controllers/userController.js");
}

TEST_CASE("express fixture: traversal is cycle-safe and bounded", "[index][express][traverse]") {
  auto db = open("express-api");
  auto r = reader("express-api");
  int64_t ping = symbol_id(db, "ping");
  REQUIRE(ping > 0);
  TraverseOptions t;
  t.max_depth = 25;
  auto tree = r.traverse(ping, t);
  // ping -> pong -> ping (seen, not expanded again)
  REQUIRE(tree["nodes"].size() == 3);
  CHECK(tree["nodes"][2]["seen"] == true);

  int64_t get_user = symbol_id(db, "getUser", "userController.js");
  t.max_depth = 1;
  auto shallow = r.traverse(get_user, t);
  for (const auto& n : shallow["nodes"]) CHECK(n["depth"].get<int>() <= 1);
  t.max_depth = 10;
  t.max_nodes = 3;
  auto capped = r.traverse(get_user, t);
  CHECK(capped["nodes"].size() <= 3);
  CHECK(capped["truncated"] == true);

  TraverseOptions back;
  back.direction = "backward";
  auto callers = r.traverse(symbol_id(db, "UserService.findById"), back);
  std::set<std::string> names;
  for (const auto& n : callers["nodes"]) names.insert(n["symbol"]["qualified_name"]);
  CHECK(names.count("getUser"));
  CHECK(names.count("UserService.remove"));
}

TEST_CASE("express fixture: dependency cycles and reverse dependencies", "[index][express][deps]") {
  auto r = reader("express-api");
  auto a = r.file(std::nullopt, "src/cycle/a.js", 10);
  auto out = r.file_deps(a["id"].get<int64_t>(), "out", 10, 0)["dependencies"];
  REQUIRE(out.size() == 1);
  CHECK(out[0]["target"]["path"] == "src/cycle/b.js");
  auto in = r.file_deps(a["id"].get<int64_t>(), "in", 10, 0)["dependencies"];
  REQUIRE(in.size() == 1);
  CHECK(in[0]["source"]["path"] == "src/cycle/b.js");
}

TEST_CASE("express fixture: entry points and integrity", "[index][express]") {
  auto r = reader("express-api");
  auto entries = r.entry_points(20)["entry_points"];
  std::set<std::string> kinds;
  for (const auto& e : entries) {
    CHECK(e["path"] == "src/server.js");
    kinds.insert(e["kind"]);
  }
  CHECK(kinds == std::set<std::string>{"package_main", "package_script", "server_start"});
  auto check = r.check();
  CHECK(check["ok"] == true);
  auto snippet = r.snippet(r.file(std::nullopt, "src/app.js", 1)["id"].get<int64_t>(), 14, 0, 2);
  REQUIRE(snippet["lines"].size() == 3);
  CHECK(snippet["lines"][0]["text"] == "app.get('/health', (req, res) => {");
  CHECK(snippet["changed_since_index"] == false);
}

// ---------------------------------------------------------------------------
// Other languages
// ---------------------------------------------------------------------------

TEST_CASE("python fixture: blueprint prefixes, FastAPI routers and Django paths", "[index][python]") {
  auto r = reader("python-app");
  std::map<std::string, std::string> routes;
  json all = r.routes(50, 0);  // keep the result alive while iterating
  for (const auto& x : all["routes"]) {
    routes[x["method"].get<std::string>() + " " + x["full_path"].get<std::string>()] = x["handler_name"];
  }
  CHECK(routes.at("GET /api/users/<int:user_id>") == "get_user");
  CHECK(routes.at("POST /api/users") == "create_user");
  CHECK(routes.at("GET /v1/items/{item_id}") == "read_item");
  CHECK(routes.at("ANY /orders/<int:order_id>/") == "order_detail");
  auto db = open("python-app");
  auto st = db.prepare(
      "SELECT t.qualified_name FROM edges e JOIN symbols s ON s.id = e.src_symbol_id JOIN symbols t ON t.id = e.dst_symbol_id "
      "WHERE s.qualified_name = 'app.services.user_service.UserService.get_user' AND e.target_text = 'self.repo.find'");
  REQUIRE(st.step());
  CHECK(st.text(0) == "app.repositories.user_repository.UserRepository.find");
  auto entries = r.entry_points(10)["entry_points"];
  REQUIRE(entries.size() >= 1);
  CHECK(entries[0]["kind"] == "main_guard");
  CHECK(indexed("python-app").add["result"]["stats"]["files"]["with_syntax_errors"] == 1);
  CHECK(r.file(std::nullopt, ".venv/lib/ignored.py", 1).is_null());
}

TEST_CASE("java fixture: interface dispatch, overloads and Spring routes", "[index][java]") {
  auto r = reader("java-spring");
  auto db = open("java-spring");
  auto found = r.search("GET /api/users/{id}", {"route"}, false, 5)["routes"];
  REQUIRE(found.size() == 1);
  CHECK(found[0]["handler_name"] == "getUser");
  int64_t iface = symbol_id(db, "com.example.demo.service.UserService.findUser");
  auto callees = r.edges(iface, "out", {"overridden_by"}, 10, 0)["edges"];
  REQUIRE(callees.size() == 1);
  CHECK(callees[0]["target"]["qualified_name"] == "com.example.demo.service.UserServiceImpl.findUser");
  auto st = db.prepare(
      "SELECT t.param_count, e.note FROM edges e JOIN symbols s ON s.id = e.src_symbol_id JOIN symbols t ON t.id = e.dst_symbol_id "
      "WHERE s.qualified_name = 'com.example.demo.service.UserServiceImpl.register' AND e.target_name = 'validate'");
  REQUIRE(st.step());
  CHECK(st.int64(0) == 1);  // validate(User), not validate(User, boolean)
  auto exc = r.exceptions(symbol_id(db, "com.example.demo.service.UserServiceImpl.remove"), 2, 50);
  CHECK(exc["declared"].size() == 1);
  bool from_callee = false;
  for (const auto& x : exc["in_callees"]) from_callee |= x["type"] == "UserNotFoundException";
  CHECK(from_callee);
  auto entries = r.entry_points(10)["entry_points"];
  CHECK(entries[0]["kind"] == "main_function");
  CHECK(r.file(std::nullopt, "target/classes/Generated.java", 1).is_null());
}

TEST_CASE("cpp fixture: includes by suffix, out-of-line methods and main", "[index][cpp]") {
  auto r = reader("cpp-app");
  auto db = open("cpp-app");
  auto main_file = r.file(std::nullopt, "src/main.cpp", 10);
  auto deps = r.file_deps(main_file["id"].get<int64_t>(), "out", 10, 0)["dependencies"];
  std::map<std::string, std::string> by_spec;
  for (const auto& d : deps) by_spec[d["specifier"]] = d["status"];
  CHECK(by_spec.at("store/user_store.h") == "resolved");
  CHECK(by_spec.at("util.h") == "resolved");
  CHECK(by_spec.at("stdexcept") == "external");
  auto st = db.prepare(
      "SELECT t.qualified_name, t.is_definition, tf.path FROM edges e JOIN symbols s ON s.id = e.src_symbol_id "
      "JOIN symbols t ON t.id = e.dst_symbol_id JOIN files tf ON tf.id = t.file_id WHERE s.name = 'main' AND e.target_text = 'users.find'");
  REQUIRE(st.step());
  CHECK(st.text(0) == "store::UserStore::find");
  CHECK(st.int64(1) == 1);  // the definition in user_store.cpp, not the header declaration
  CHECK(st.text(2) == "src/user_store.cpp");
  auto exc = r.exceptions(symbol_id(db, "main"), 2, 50);
  CHECK(exc["handlers"].size() == 1);
  bool out_of_range = false;
  for (const auto& x : exc["in_callees"]) out_of_range |= x["type"] == "std::out_of_range";
  CHECK(out_of_range);
  CHECK(r.entry_points(5)["entry_points"][0]["kind"] == "main_function");
}

// ---------------------------------------------------------------------------
// Failure handling and the protocol
// ---------------------------------------------------------------------------

TEST_CASE("a failed re-analysis keeps the previous index", "[index][failure]") {
  if (geteuid() == 0) SKIP("running as root: permissions are not enforced");
  TempDir data;
  TempDir src;
  proj::test::write_file(src.path() / "a.js", "function a() { b(); }\nfunction b() {}\n");
  protocol::Engine engine;
  auto first = call(engine, "add_project", {{"name", "p"}, {"path", src.path().string()}}, data.path());
  REQUIRE(first["status"] == "ok");
  fs::path index = first["result"]["index_path"].get<std::string>();
  auto before = fs::file_size(index);
  // Make the index directory read-only so the build file cannot be created.
  fs::permissions(index.parent_path(), fs::perms::owner_read | fs::perms::owner_exec);
  auto second = call(engine, "add_project", {{"name", "p"}, {"path", src.path().string()}, {"reanalyze", true}}, data.path());
  fs::permissions(index.parent_path(), fs::perms::owner_all);
  CHECK(second["status"] == "analysis_failed");
  CHECK(second["error"]["details"]["previous_index_kept"] == true);
  CHECK(fs::file_size(index) == before);
  auto runs = call(engine, "list_runs", {{"project", "p"}}, data.path())["result"]["runs"];
  REQUIRE(runs.size() == 2);
  CHECK(runs[0]["status"] == "failed");
  CHECK(runs[1]["status"] == "success");
  // The old index still answers queries.
  auto search = call(engine, "search", {{"project", "p"}, {"query", "a"}, {"exact", true}}, data.path());
  CHECK(search["status"] == "ok");
  CHECK(search["result"]["symbols"].size() == 1);
}

TEST_CASE("projects without supported files are reported as such", "[index]") {
  TempDir data, src;
  proj::test::write_file(src.path() / "README.md", "# docs\n");
  protocol::Engine engine;
  auto res = call(engine, "add_project", {{"name", "docs"}, {"path", src.path().string()}}, data.path());
  CHECK(res["status"] == "partial");
  CHECK(res["result"]["status"] == "no_supported_files");
  CHECK(res["diagnostics"][0]["code"] == "no_supported_files");
}

TEST_CASE("protocol: argument validation and error categories", "[protocol]") {
  TempDir data, src;
  proj::test::write_file(src.path() / "a.py", "def a():\n    pass\n");
  protocol::Engine engine;
  CHECK(engine.handle_text("{not json")["status"] == "invalid_arguments");
  CHECK(engine.handle_text(R"({"protocol":2,"op":"info"})")["error"]["code"] == "unsupported_protocol_version");
  CHECK(call(engine, "nope", json::object(), data.path())["status"] == "unsupported_operation");
  CHECK(call(engine, "add_project", {{"name", "bad name!"}, {"path", src.path().string()}}, data.path())["error"]["code"] ==
        "invalid_project_name");
  CHECK(call(engine, "add_project", {{"name", "x"}, {"path", (src.path() / "missing").string()}}, data.path())["error"]["code"] ==
        "path_not_found");
  CHECK(call(engine, "add_project", {{"name", "x"}, {"path", (src.path() / "a.py").string()}}, data.path())["error"]["code"] ==
        "not_a_directory");
  CHECK(call(engine, "add_project", {{"name", "x"}}, data.path())["error"]["code"] == "missing_argument");

  auto ok = call(engine, "add_project", {{"name", "one"}, {"path", src.path().string()}}, data.path());
  CHECK(ok["status"] == "ok");
  CHECK(ok["protocol"] == kProtocolVersion);
  auto dup = call(engine, "add_project", {{"name", "one"}, {"path", src.path().string()}}, data.path());
  CHECK(dup["status"] == "conflict");
  CHECK(dup["error"]["code"] == "project_exists");
  auto other = call(engine, "add_project", {{"name", "two"}, {"path", src.path().string()}}, data.path());
  CHECK(other["error"]["code"] == "path_registered");

  CHECK(call(engine, "search", {{"project", "ghost"}, {"query", "a"}}, data.path())["status"] == "not_found");
  CHECK(call(engine, "symbol", {{"project", "one"}, {"symbol_id", 999999}}, data.path())["status"] == "not_found");
  CHECK(call(engine, "traverse", {{"project", "one"}, {"symbol_id", 1}, {"direction", "sideways"}}, data.path())["status"] ==
        "invalid_arguments");

  // A corrupt index produces an actionable index_error, not a crash.
  fs::path index = ok["result"]["index_path"].get<std::string>();
  proj::test::write_file(index, "this is not a sqlite database at all, just text");
  protocol::Engine fresh;
  auto corrupt = call(fresh, "summary", {{"project", "one"}}, data.path());
  CHECK(corrupt["status"] == "index_error");
  // A missing index is reported as not_found with a remedy.
  fs::remove(index);
  auto missing = call(fresh, "summary", {{"project", "one"}}, data.path());
  CHECK(missing["status"] == "not_found");
  CHECK(missing["error"]["code"] == "index_missing");
}

TEST_CASE("analysis records peak memory when the platform reports it", "[index][memory]") {
  auto rss = peak_rss_kib();
#if defined(__linux__) || defined(__APPLE__)
  REQUIRE(rss.has_value());
  CHECK(*rss > 0);
  CHECK(indexed("express-api").add["result"]["stats"]["peak_rss_kib"].is_number());
#endif
}
