// Legacy server code: routes declared in configuration files (web.xml, Struts,
// Spring XML, OpenAPI, serverless.yml, custom XML/YAML), registered in code
// without annotations (servlet containers, the JDK HttpServer, Spark, Tornado,
// web.py, Falcon, Pyramid, Django class-based views, C/C++ route tables and
// registration calls) or dispatched on the request path, plus overloads
// selected by argument types.
#include <fstream>
#include <map>
#include <random>
#include <regex>
#include <sstream>

#include "db/sqlite.hpp"
#include "helpers.hpp"
#include "index_dump.hpp"
#include "query/query.hpp"

using namespace proj;
using proj::test::call;
using proj::test::fixture;
using proj::test::TempDir;
using proj::test::write_file;
namespace fs = std::filesystem;

namespace {

struct Route {
  std::string handler, status, note, confidence;
};

struct Indexed {
  TempDir data;
  json result;
  // "METHOD full_path framework registrar" -> route
  std::map<std::string, Route> routes;
  std::map<std::string, std::vector<std::string>> middleware;  // same key -> "name=status:target"
  std::unique_ptr<db::Database> db;

  std::string handler(const std::string& key) const {
    auto it = routes.find(key);
    return it == routes.end() ? "<no route " + key + ">" : it->second.handler;
  }
  std::string status(const std::string& key) const {
    auto it = routes.find(key);
    return it == routes.end() ? "<no route>" : it->second.status;
  }
  std::string note(const std::string& key) const {
    auto it = routes.find(key);
    return it == routes.end() ? "<no route>" : it->second.note;
  }
  int64_t count(const std::string& sql) const { return db->query_int(sql).value_or(-1); }
};

std::unique_ptr<Indexed> index_fixture(const fs::path& root) {
  auto a = std::make_unique<Indexed>();
  protocol::Engine engine;
  a->result = call(engine, "add_project", {{"name", "p"}, {"path", root.string()}}, a->data.path());
  REQUIRE(a->result["result"].is_object());
  a->db = std::make_unique<db::Database>(a->result["result"]["index_path"].get<std::string>(), db::OpenMode::ReadOnly);
  auto st = a->db->prepare(
      "SELECT r.method || ' ' || r.full_path || ' ' || r.framework || ' ' || IFNULL(r.registrar, ''), "
      "IFNULL(h.qualified_name, ''), r.handler_status, IFNULL(r.handler_note, ''), r.confidence, r.id FROM routes r "
      "LEFT JOIN symbols h ON h.id = r.handler_symbol_id");
  auto mw = a->db->prepare(
      "SELECT IFNULL(m.name, '') || '=' || m.status || ':' || IFNULL(s.qualified_name, '-') FROM route_middleware m "
      "LEFT JOIN symbols s ON s.id = m.symbol_id WHERE m.route_id = ? ORDER BY m.position");
  while (st.step()) {
    std::string key = st.text(0);
    a->routes[key] = {st.text(1), st.text(2), st.text(3), st.text(4)};
    mw.bind(1, st.int64(5));
    while (mw.step()) a->middleware[key].push_back(mw.text(0));
    mw.reset();
  }
  return a;
}

}  // namespace

TEST_CASE("web.xml: servlets dispatch to doGet/doPost/service, filters, listeners", "[legacy][java][config]") {
  auto a = index_fixture(fixture("legacy-java"));
  // One servlet mapping becomes one route per HTTP method the class implements,
  // including methods inherited from project base classes.
  CHECK(a->handler("GET /api/users/* servlet users") == "com.acme.legacy.web.UserServlet.doGet");
  CHECK(a->handler("POST /api/users/* servlet users") == "com.acme.legacy.web.UserServlet.doPost");
  CHECK(a->handler("DELETE /api/users/* servlet users") == "com.acme.legacy.web.BaseServlet.doDelete");
  // A servlet overriding service() handles every method; two url-patterns -> two routes.
  CHECK(a->handler("ANY /api/orders servlet orders") == "com.acme.legacy.web.OrderServlet.service");
  CHECK(a->handler("ANY /api/orders/* servlet orders") == "com.acme.legacy.web.OrderServlet.service");
  // Filters run in declaration order; external ones stay visible as external.
  REQUIRE(a->middleware["GET /api/users/* servlet users"].size() == 2);
  CHECK(a->middleware["GET /api/users/* servlet users"][0] ==
        "org.apache.catalina.filters.SetCharacterEncodingFilter=external:-");
  CHECK(a->middleware["GET /api/users/* servlet users"][1] ==
        "com.acme.legacy.web.AuthFilter=resolved:com.acme.legacy.web.AuthFilter.doFilter");
  CHECK(a->middleware["ANY /health servlet health"].size() == 1);  // /api/* filter does not apply
  // Honest failures.
  CHECK(a->status("ANY /health servlet health") == "unresolved");
  CHECK(a->note("ANY /health servlet health").find("no request-handling method") != std::string::npos);
  CHECK(a->status("ANY *.do servlet struts") == "external");
  CHECK(a->note("ANY /about servlet about").find("not analyzed") != std::string::npos);
  // The descriptor is an entry point; startup code it configures is reachable.
  CHECK(a->count("SELECT COUNT(*) FROM entry_points WHERE kind = 'deployment_descriptor'") == 1);
  CHECK(a->count("SELECT COUNT(*) FROM edges e JOIN symbols t ON t.id = e.dst_symbol_id WHERE e.kind = 'configures' "
                 "AND t.qualified_name = 'com.acme.legacy.web.AppListener.contextInitialized'") == 1);
  IndexReader reader(a->result["result"]["index_path"].get<std::string>());
  json found = reader.search("Bootstrap.start", {"symbol"}, false, 5);
  auto boot = found["symbols"];
  REQUIRE(!boot.empty());
  auto startup = reader.startup_path(boot[0]["id"].get<int64_t>(), 10);
  CHECK(startup["found"] == true);
  CHECK(startup["entry_kind"] == "deployment_descriptor");
}

TEST_CASE("Struts, Spring XML, custom YAML and OpenAPI routes name their handler classes", "[legacy][java][config]") {
  auto a = index_fixture(fixture("legacy-java"));
  CHECK(a->handler("ANY /login struts action-mappings") == "com.acme.legacy.struts.LoginAction.execute");
  CHECK(a->note("ANY /admin/users struts action-mappings").find("request parameter 'method'") != std::string::npos);
  CHECK(a->handler("ANY /reports/list struts2 reports") == "com.acme.legacy.struts.ReportAction.list");
  CHECK(a->handler("ANY /reports/export struts2 reports") == "com.acme.legacy.struts.ReportAction.execute");
  CHECK(a->handler("ANY /hello.htm spring_xml bean name") == "com.acme.legacy.spring.HelloController.handleRequest");
  CHECK(a->handler("ANY /account.htm spring_xml SimpleUrlHandlerMapping") ==
        "com.acme.legacy.spring.AccountController.handleRequestInternal");
  // A bean defined in another XML file.
  CHECK(a->handler("ANY /legacy-report.htm spring_xml SimpleUrlHandlerMapping") ==
        "com.acme.legacy.spring.ReportController.handleRequestInternal");
  // Custom format: prefix + path + handler class or Class#method, methods given.
  CHECK(a->handler("GET /gw/invoices config routes.yaml") == "com.acme.legacy.handlers.InvoiceHandler.handle");
  CHECK(a->handler("PUT /gw/invoices/{id} config routes.yaml") == "com.acme.legacy.handlers.InvoiceHandler.update");
  CHECK(a->handler("DELETE /gw/invoices/{id} config routes.yaml") == "com.acme.legacy.handlers.InvoiceHandler.update");
  CHECK(a->handler("GET /gw/shipments config routes.yaml") == "com.acme.legacy.handlers.ShipmentHandler.doGet");
  CHECK(a->handler("POST /gw/shipments config routes.yaml") == "com.acme.legacy.handlers.ShipmentHandler.doPost");
  CHECK(a->routes["GET /gw/invoices config routes.yaml"].confidence == "heuristic");
  CHECK(a->note("ANY /gw/ping config routes.yaml").find("not found in the project") != std::string::npos);
  // OpenAPI operationId matched by name; the interface declaration is not chosen.
  CHECK(a->handler("GET /v2/customers/{id} openapi paths") == "com.acme.legacy.api.CustomerResource.getCustomer");
  CHECK(a->status("GET /v2/customers/{id} openapi paths") == "heuristic");
  // Spring beans are symbols wired to their classes and to each other.
  CHECK(a->count("SELECT COUNT(*) FROM symbols WHERE kind = 'bean'") == 4);
  CHECK(a->count("SELECT COUNT(*) FROM edges e JOIN symbols s ON s.id = e.src_symbol_id JOIN symbols t ON "
                 "t.id = e.dst_symbol_id WHERE e.kind = 'configures' AND s.name = 'userService' AND t.name = 'userDao'") == 1);
  // Deployment and build files produce no routes.
  CHECK(a->count("SELECT COUNT(*) FROM routes r JOIN files f ON f.id = r.file_id WHERE f.path LIKE 'deploy/%' "
                 "OR f.path = 'pom.xml'") == 0);
}

TEST_CASE("Java routes registered in code: servlet containers, HttpServer, Spark, front controllers", "[legacy][java]") {
  auto a = index_fixture(fixture("legacy-java"));
  CHECK(a->handler("ANY /status httpserver server") == "com.acme.legacy.server.StatusHandler.handle");
  CHECK(a->handler("ANY /status/shared httpserver server") == "com.acme.legacy.server.StatusHandler.handle");
  CHECK(a->handler("ANY /metrics httpserver server") == "com.acme.legacy.server.EmbeddedServer.main.ANY /metrics");
  CHECK(a->handler("ANY /echo httpserver server") == "com.acme.legacy.server.EmbeddedServer.main.handle");
  CHECK(a->handler("GET /v1/users/* servlet context") == "com.acme.legacy.web.UserServlet.doGet");
  CHECK(a->handler("ANY /v1/orders/* servlet context") == "com.acme.legacy.web.OrderServlet.service");
  CHECK(a->handler("GET /v1/export-all servlet context") == "com.acme.legacy.server.ExportServlet.doGet");
  CHECK(a->handler("GET /v1/reports/* servlet reports") == "com.acme.legacy.server.ReportServlet.doGet");
  CHECK(a->handler("PUT /v1/audit/* servlet ctx.addServlet(\"audit\", AuditServlet.class)") ==
        "com.acme.legacy.server.AuditServlet.doPut");
  CHECK(a->handler("GET /v1/export servlet ExportServlet") == "com.acme.legacy.server.ExportServlet.doGet");
  CHECK(a->handler("GET /spark/users/:id spark (static)") == "com.acme.legacy.server.UserRoutes.show");
  CHECK(a->handler("GET /spark/hello spark (static)") == "com.acme.legacy.server.SparkApp.main.GET /spark/hello");
  // The handler lambda owns the calls inside it.
  CHECK(a->count("SELECT COUNT(*) FROM edges e JOIN symbols s ON s.id = e.src_symbol_id JOIN symbols t ON "
                 "t.id = e.dst_symbol_id WHERE s.kind = 'route_handler' AND t.qualified_name = "
                 "'com.acme.legacy.server.Greeter.greet'") == 1);
  // A front controller servlet dispatching on the path itself.
  CHECK(a->handler("ANY /profile dispatch doGet") == "com.acme.legacy.web.FrontControllerServlet.showProfile");
  CHECK(a->handler("ANY /orders dispatch doGet") == "com.acme.legacy.service.OrderService.list");
  CHECK(a->handler("ANY /help dispatch doGet") == "com.acme.legacy.web.FrontControllerServlet.renderHelp");
  CHECK(a->handler("ANY /cmd/login dispatch_table commandMap") == "com.acme.legacy.web.LoginCommand.execute");
}

TEST_CASE("overloads are selected by the static types of the arguments", "[legacy][java][cpp][resolution]") {
  auto a = index_fixture(fixture("legacy-java"));
  auto target = [&](const std::string& caller, const std::string& args) {
    auto st = a->db->prepare(
        "SELECT IFNULL(t.qualified_name || '(' || IFNULL(t.param_types, '') || ')', e.status) FROM edges e "
        "JOIN symbols s ON s.id = e.src_symbol_id LEFT JOIN symbols t ON t.id = e.dst_symbol_id "
        "WHERE s.name = ? AND IFNULL(e.arg_types, '') = ? AND e.target_name IN ('find','place')");
    st.bind(1, caller).bind(2, args);
    return st.step() ? st.text(0) : std::string("<no edge>");
  };
  CHECK(target("doGet", "long") == "com.acme.legacy.service.UserService.find(long)");
  CHECK(target("doGet", "String") == "com.acme.legacy.service.UserService.find(String)");
  CHECK(target("doGet", "String,int") == "com.acme.legacy.service.UserService.find(String,int)");
  CHECK(target("service", "?,int") == "com.acme.legacy.service.OrderService.place(String,int)");

  auto c = index_fixture(fixture("legacy-cpp"));
  auto render = [&](const std::string& args) {
    auto st = c->db->prepare(
        "SELECT t.param_types FROM edges e JOIN symbols t ON t.id = e.dst_symbol_id WHERE e.target_name = 'render' "
        "AND e.arg_types = ? AND e.status = 'resolved'");
    st.bind(1, args);
    return st.step() ? st.text(0) : std::string("<unresolved>");
  };
  CHECK(render("char*") == "const std::string&");
  CHECK(render("int") == "int");
}

TEST_CASE("Python legacy frameworks: handler classes, URL lists and dispatch", "[legacy][python]") {
  auto a = index_fixture(fixture("legacy-python"));
  CHECK(a->handler("GET /users/{param} tornado tornado.web.Application") == "tornado_app.handlers.UserHandler.get");
  CHECK(a->handler("POST /users/{param} tornado tornado.web.Application") == "tornado_app.handlers.BaseHandler.post");
  CHECK(a->handler("DELETE /orders/{order_id} tornado tornado.web.Application") ==
        "tornado_app.handlers.OrderHandler.delete");
  CHECK(a->handler("POST /items/{param} webpy urls") == "webpy_app.app.Item.POST");
  CHECK(a->handler("GET /things falcon app") == "falcon_app.app.ThingsResource.on_get");
  CHECK(a->handler("GET /health falcon app") == "falcon_app.app.HealthResource.on_get");
  CHECK(a->handler("ANY / pyramid home") == "pyramid_app.views.home");
  CHECK(a->handler("GET /users/{id} pyramid user") == "pyramid_app.views.user_view");
  // Django: include() prefix and a class-based view dispatched per method.
  CHECK(a->handler("GET /api/products/{pk}/ django urlpatterns") == "shop.views.ProductView.get");
  CHECK(a->handler("POST /api/products/{pk}/ django urlpatterns") == "shop.views.ProductView.post");
  CHECK(a->handler("ANY /api/cart/ django urlpatterns") == "shop.views.cart");
  CHECK(a->handler("POST /submit bottle app") == "bottle_app.submit");
  CHECK(a->handler("GET /users/<int:user_id> flask app") == "flask_legacy.UserAPI.get");
  CHECK(a->handler("GET /pets openapi paths") == "api.pets.list_pets");
  CHECK(a->handler("GET /pets/{pet_id} openapi paths") == "api.pets.get_pet");
  CHECK(a->handler("POST /pets serverless createPet") == "api.handlers.create_pet_handler");
  CHECK(a->handler("GET /* http_server HTTPServer") == "http_server_app.PetHandler.do_GET");
  CHECK(a->handler("ANY /pets dispatch do_GET") == "api.pets.list_pets");
  CHECK(a->handler("ANY /pets/import dispatch do_POST") == "http_server_app.PetHandler.import_pets");
}

TEST_CASE("C and C++ legacy routes: tables, registration calls, dispatch, configured classes", "[legacy][cpp]") {
  auto a = index_fixture(fixture("legacy-cpp"));
  CHECK(a->handler("POST /jobs cpp_table ROUTES") == "handle_submit_job");
  CHECK(a->handler("GET /status cpp_table ROUTES") == "handle_status");
  CHECK(a->handler("ANY /health dispatch dispatch") == "handle_health");
  CHECK(a->handler("GET /metrics dispatch dispatch") == "handle_metrics");  // log_request is skipped
  CHECK(a->handler("GET /ping cpp-httplib svr") == "api::ping");
  CHECK(a->handler("GET /users cpp-httplib svr") == "api::run::GET /users");
  CHECK(a->handler("POST /users cpp-httplib svr") == "api::Users::create");
  CHECK(a->handler("ANY /reports dispatch createRequestHandler") == "reports::ReportHandler::handleRequest");
  CHECK(a->handler("GET /v2/reports config route") == "reports::ReportHandler::handleRequest");
  CHECK(a->handler("ANY /v2/export config route") == "reports::ExportHandler::handleRequest");
}

TEST_CASE("route search matches servlet and extension wildcards", "[legacy][query]") {
  auto a = index_fixture(fixture("legacy-java"));
  IndexReader reader(a->result["result"]["index_path"].get<std::string>());
  auto paths = [&](const std::string& q) {
    std::vector<std::string> out;
    json found = reader.search(q, {"route"}, false, 50);  // keep the result alive while iterating
    for (const auto& r : found["routes"]) {
      out.push_back(r["method"].get<std::string>() + " " + r["full_path"].get<std::string>());
    }
    return out;
  };
  auto has = [](const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
  };
  CHECK(has(paths("/api/users/42/orders"), "GET /api/users/*"));
  CHECK(has(paths("GET /api/users/42"), "GET /api/users/*"));
  CHECK(has(paths("/login.do"), "ANY *.do"));
  CHECK(has(paths("/gw/invoices/7"), "PUT /gw/invoices/{id}"));
}

TEST_CASE("configuration files never make an analysis partial", "[legacy][config]") {
  TempDir src;
  write_file(src.path() / "app.py", "def main():\n    return 1\n");
  // A Helm template is not valid YAML; large XML is usually data.
  write_file(src.path() / "chart/templates/deploy.yaml", "{{- if .Values.enabled }}\nkind: Deployment\n{{- end }}\n");
  write_file(src.path() / "data/dump.xml", "<rows>" + std::string(2 * 1024 * 1024, ' ') + "</rows>");
  auto a = index_fixture(src.path());
  CHECK(a->result["result"]["status"] == "success");
  CHECK(a->result["result"]["stats"]["files"]["config"]["skipped"] == 1);
}

TEST_CASE("refresh of legacy projects matches a full analysis", "[legacy][refresh]") {
  auto name = GENERATE(std::string("legacy-java"), std::string("legacy-python"), std::string("legacy-cpp"));
  TempDir dir;
  fs::path src = dir.path() / "src";
  fs::copy(fixture(name), src, fs::copy_options::recursive);
  fs::path data = dir.path() / "data";
  protocol::Engine engine;
  auto first = call(engine, "add_project", {{"name", "p"}, {"path", src.string()}}, data);
  REQUIRE(first["result"].is_object());
  std::vector<fs::path> files;
  for (auto& e : fs::recursive_directory_iterator(src)) {
    if (e.is_regular_file()) files.push_back(e.path());
  }
  std::sort(files.begin(), files.end());
  // Names whose changes move routes, dispatch targets, overloads and configured classes.
  const std::vector<std::string> names = {"doGet", "doPost", "service", "handle", "execute", "UserServlet",
                                          "InvoiceHandler", "get", "post", "on_get", "find", "render",
                                          "handleRequest", "handle_status", "ProductView", "users", "api"};
  std::mt19937 rng(20261012);
  auto read = [](const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
  };
  for (int round = 0; round < 12; ++round) {
    std::vector<fs::path> present;
    for (const auto& f : files) {
      if (fs::exists(f)) present.push_back(f);
    }
    REQUIRE(!present.empty());
    auto pick = present[rng() % present.size()];
    std::string id = names[rng() % names.size()];
    int op = static_cast<int>(rng() % 3);
    if (op == 0) {
      std::string s = read(pick);
      write_file(pick, std::regex_replace(s, std::regex("\\b" + id + "\\b"), id + "X"));
    } else if (op == 1) {
      fs::copy_file(pick, pick.parent_path() / ("copy" + std::to_string(round) + "_" + pick.filename().string()));
    } else {
      fs::remove(pick);
    }
    INFO(name << " round " << round << " op " << op << " " << pick.filename() << " " << id);
    auto r = call(engine, "refresh_project", {{"project", "p"}, {"max_affected_fraction", 1.0}}, data);
    REQUIRE(r["result"].is_object());
    TempDir ref;
    protocol::Engine other;
    auto full = call(other, "add_project", {{"name", "p"}, {"path", src.string()}}, ref.path());
    REQUIRE(full["result"].is_object());
    db::Database da(first["result"]["index_path"].get<std::string>(), db::OpenMode::ReadOnly);
    db::Database db_(full["result"]["index_path"].get<std::string>(), db::OpenMode::ReadOnly);
    auto x = proj::test::canonical_dump(da);
    auto y = proj::test::canonical_dump(db_);
    std::vector<std::string> only_refresh, only_full;
    std::set_difference(x.begin(), x.end(), y.begin(), y.end(), std::back_inserter(only_refresh));
    std::set_difference(y.begin(), y.end(), x.begin(), x.end(), std::back_inserter(only_full));
    for (size_t i = 0; i < only_refresh.size() && i < 8; ++i) UNSCOPED_INFO("only in refreshed: " << only_refresh[i]);
    for (size_t i = 0; i < only_full.size() && i < 8; ++i) UNSCOPED_INFO("only in full:      " << only_full[i]);
    CHECK(only_refresh.empty());
    CHECK(only_full.empty());
  }
}

TEST_CASE("code map: parts of the project and the links between them", "[legacy][query][overview]") {
  auto a = index_fixture(fixture("legacy-java"));
  IndexReader reader(a->result["result"]["index_path"].get<std::string>());
  json map = reader.overview("", 14);
  std::map<std::string, json> by_label;
  for (const auto& n : map["nodes"]) by_label[n["label"].get<std::string>()] = n;
  REQUIRE(map["nodes"].size() <= 14);
  // Single-directory chains collapse: src/main/java/com/acme/legacy/web -> "legacy/web".
  REQUIRE(by_label.count("legacy/web"));
  REQUIRE(by_label.count("webapp/WEB-INF"));
  CHECK(by_label["legacy/web"]["path"] == "src/main/java/com/acme/legacy/web");
  CHECK(by_label["webapp/WEB-INF"]["routes_declared"].get<int64_t>() > 0);
  CHECK(by_label["legacy/web"]["route_handlers"].get<int64_t>() > 0);
  auto link = [&](const std::string& from, const std::string& to) -> json {
    for (const auto& e : map["edges"]) {
      if (e["from"] == by_label[from]["id"] && e["to"] == by_label[to]["id"]) return e;
    }
    return nullptr;
  };
  // web.xml registers routes to the servlets and configures filters and listeners.
  json wiring = link("webapp/WEB-INF", "legacy/web");
  REQUIRE(wiring.is_object());
  CHECK(wiring["kinds"]["registers_route"].get<int64_t>() > 0);
  CHECK(wiring["kinds"]["configures"].get<int64_t>() > 0);
  CHECK(link("legacy/web", "legacy/service").is_object());
  CHECK(link("legacy/service", "legacy/dao").is_object());

  // Zooming into a leaf part lists its files; code outside becomes one node.
  json server = reader.overview("src/main/java/com/acme/legacy/server", 14);
  REQUIRE(server["files"].is_array());
  CHECK(server["files"].size() == 15);
  CHECK(server["parent"] == "src/main/java/com/acme/legacy");
  bool outside = false;
  for (const auto& n : server["nodes"]) outside = outside || n["kind"] == "outside";
  CHECK(outside);
  json direct = reader.overview("src/main/resources", 14, true);
  CHECK(direct["files"].size() == 3);  // routes.yaml, struts.xml, applicationContext.xml (not api/)

  // A small budget groups the smallest directories into "other".
  json small = reader.overview("src/main/java/com/acme/legacy", 4);
  CHECK(small["nodes"].size() <= 5);  // up to 4 parts + the rest of the project
  bool other = false;
  for (const auto& n : small["nodes"]) other = other || n["kind"] == "other";
  CHECK(other);

  // Route lists name the method that handles the route.
  json routes = reader.routes(500, 0);
  bool found = false;
  for (const auto& r : routes["routes"]) {
    if (r["full_path"] == "/api/users/*" && r["method"] == "GET") {
      found = true;
      CHECK(r["handler"]["qualified_name"] == "com.acme.legacy.web.UserServlet.doGet");
    }
  }
  CHECK(found);
}
