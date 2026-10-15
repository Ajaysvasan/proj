// Runtime tracing on the engine side: the runtime store (ingest, idempotent replays, error
// deduplication, retention), runtime methods placed in the static index, and the evidence shown on
// traversals. Agent messages are written by hand here; cli/test/dynamic.test.ts runs real JVMs.
#include <set>

#include "db/sqlite.hpp"
#include "dynamic/runtime_store.hpp"
#include "dynamic/service.hpp"
#include "helpers.hpp"
#include "query/query.hpp"

using namespace proj;
using proj::test::call;
using proj::test::fixture;
using proj::test::TempDir;
namespace fs = std::filesystem;
using dynamic::RuntimeStore;

namespace {

json hello(const std::string& session, const std::string& mode = "startup", bool since_start = true) {
  return {{"type", "hello"}, {"session", session}, {"pid", 4242}, {"mode", mode}, {"agent", "0.1.0"},
          {"jvm", {{"version", "21"}, {"vendor", "test"}, {"start_ms", 1000}}}, {"loaded_ms", 1000},
          {"since_start", since_start}, {"include", "com.example.shop"}};
}

json method_def(int id, const std::string& cls, const std::string& name, const std::string& desc,
                const std::string& source, int first, int last) {
  return {{"type", "method"}, {"id", id}, {"class", cls}, {"name", name}, {"desc", desc},
          {"source", source}, {"first_line", first}, {"last_line", last}};
}

json trace_msg(int64_t id, json nodes, const std::string& status = "complete", json errors = json::array()) {
  return {{"type", "trace"}, {"id", id}, {"status", status}, {"thread", {{"id", 1}, {"name", "worker-1"}}},
          {"start_ms", 5000 + id}, {"duration_us", 120}, {"nodes", std::move(nodes)}, {"errors", std::move(errors)}};
}

json error_msg(const std::string& type, const std::string& state, json frames, json throw_site = nullptr) {
  return {{"exception", type}, {"message", "m"}, {"state", state}, {"rethrown", false}, {"wrapped_by", nullptr},
          {"throw", throw_site}, {"caught", nullptr}, {"escaped_from", nullptr}, {"frames", std::move(frames)},
          {"frames_omitted", 0}, {"causes", json::array()}, {"suppressed", 0}, {"events", json::array()}};
}

int64_t count(RuntimeStore& s, const std::string& sql) { return s.db().query_int(sql).value_or(-1); }

}  // namespace

TEST_CASE("runtime store: sessions, idempotent traces, aggregated edges, deduplicated errors", "[dynamic]") {
  TempDir dir;
  RuntimeStore s(dir.path() / "rt.db");
  int64_t session = s.open_session(hello("s-1"), json::object());
  CHECK(s.open_session(hello("s-1"), json::object()) == session);  // a reconnect reuses the session
  s.begin();
  s.method(session, method_def(1, "a.Root", "run", "()V", "Root.java", 3, 9));
  s.method(session, method_def(2, "a.Svc", "work", "(I)V", "Svc.java", 5, 8));
  s.method(session, method_def(3, "a.Repo", "load", "(J)La/User;", "Repo.java", 7, 7));
  // An open snapshot is stored but not counted.
  CHECK(s.trace(session, trace_msg(7, {{1, -1, 1, 0, 0}, {2, 0, 1, 5, 0}}, "open")));
  CHECK(count(s, "SELECT COUNT(*) FROM rt_edges") == 0);
  // The final version replaces it and is counted once; a replay changes nothing.
  CHECK(s.trace(session, trace_msg(7, {{1, -1, 1, 0, 0}, {2, 0, 3, 5, 0}, {3, 1, 2, 9, 0}})));
  CHECK_FALSE(s.trace(session, trace_msg(7, {{1, -1, 1, 0, 0}, {2, 0, 3, 5, 0}, {3, 1, 2, 9, 0}})));
  CHECK(s.trace(session, trace_msg(8, {{1, -1, 1, 0, 0}, {2, 0, 1, 5, 0}})));
  CHECK(count(s, "SELECT COUNT(*) FROM rt_traces") == 2);
  CHECK(count(s, "SELECT calls FROM rt_edges e JOIN rt_methods a ON a.id = e.caller JOIN rt_methods b ON b.id = e.callee "
                 "WHERE a.name = 'run' AND b.name = 'work'") == 4);
  CHECK(count(s, "SELECT traces FROM rt_edges e JOIN rt_methods a ON a.id = e.caller WHERE a.name = 'run'") == 2);
  CHECK(count(s, "SELECT calls FROM rt_edges e JOIN rt_methods a ON a.id = e.caller WHERE a.name = 'work'") == 2);
  // A malformed tree (parent after child) is rejected rather than stored half-way.
  CHECK_FALSE(s.trace(session, trace_msg(9, {{1, -1, 1, 0, 0}, {2, 5, 1, 0, 0}})));
  // The same exception, fate and frames in two traces: one record, counted twice.
  json frames = {{"a.Repo", "load", "Repo.java", 7, 1}, {"a.Svc", "work", "Svc.java", 6, 1}};
  json site = {{"method", 3}, {"line", 7}};
  CHECK(s.trace(session, trace_msg(10, {{1, -1, 1, 0, 0}}, "complete", {error_msg("java.io.IOException", "CAUGHT", frames, site)})));
  CHECK(s.trace(session, trace_msg(11, {{1, -1, 1, 0, 0}}, "complete", {error_msg("java.io.IOException", "CAUGHT", frames, site)})));
  CHECK(s.trace(session, trace_msg(12, {{1, -1, 1, 0, 0}}, "complete", {error_msg("java.io.IOException", "ESCAPED", frames, site)})));
  CHECK(count(s, "SELECT COUNT(*) FROM rt_errors") == 2);
  CHECK(count(s, "SELECT occurrences FROM rt_errors WHERE state = 'CAUGHT'") == 2);
  CHECK(count(s, "SELECT COUNT(*) FROM rt_error_traces l JOIN rt_errors e ON e.id = l.error_id WHERE e.state = 'CAUGHT'") == 2);
  // Overloads are separate methods: the descriptor is part of the identity.
  s.method(session, method_def(4, "a.Svc", "work", "(J)V", "Svc.java", 10, 12));
  CHECK(count(s, "SELECT COUNT(*) FROM rt_methods WHERE name = 'work'") == 2);
  s.commit();

  // Dropped items after a complete start: coverage is no longer complete.
  s.session_stats(session, {{"since_start", true}, {"dropped_items", 3}});
  CHECK(count(s, "SELECT startup_coverage = 'interrupted' FROM rt_sessions") == 1);
  int64_t attached = s.open_session(hello("s-2", "attach", false), json::object());
  CHECK(count(s, "SELECT COUNT(*) FROM rt_sessions WHERE startup_coverage = 'attached_late'") == 1);
  s.close_session(session, "ended", "the JVM exited");
  CHECK(s.mark_interrupted("service restarted") == 1);  // only the one still capturing
  auto st = s.db().prepare("SELECT status FROM rt_sessions WHERE id = ?");
  st.bind(1, attached);
  REQUIRE(st.step());
  CHECK(st.text(0) == "interrupted");
}

TEST_CASE("runtime store: an uncaught exception takes over the escaped record; retention keeps the newest", "[dynamic]") {
  TempDir dir;
  RuntimeStore s(dir.path() / "rt.db", dynamic::Retention{3, 2, 10});
  int64_t session = s.open_session(hello("s-1"), json::object());
  s.begin();
  s.method(session, method_def(1, "a.Crash", "run", "()V", "Crash.java", 4, 6));
  s.method(session, method_def(2, "a.Crash", "explode", "()V", "Crash.java", 8, 8));
  json frames = {{"a.Crash", "explode", "Crash.java", 8, 1}, {"a.Crash", "run", "Crash.java", 5, 1}};
  json escaped = error_msg("java.lang.IllegalStateException", "ESCAPED", frames, {{"method", 2}, {"line", 8}});
  escaped["events"] = {{"thrown", 2, 8}, {"exit", 2}, {"exit", 1}};
  CHECK(s.trace(session, trace_msg(1, {{1, -1, 1, 0, 1}, {2, 0, 1, 2, 1}}, "complete", {escaped})));
  s.uncaught(session, {{"type", "uncaught"}, {"trace", 1}, {"thread", {{"id", 9}, {"name", "crasher"}}},
                       {"error", error_msg("java.lang.IllegalStateException", "UNCAUGHT", frames)}});
  CHECK(count(s, "SELECT COUNT(*) FROM rt_errors WHERE state = 'ESCAPED'") == 0);
  CHECK(count(s, "SELECT COUNT(*) FROM rt_errors WHERE state = 'UNCAUGHT'") == 1);
  // What the trace saw (throw site, events) is kept on the uncaught record, linked to the trace.
  CHECK(count(s, "SELECT throw_line FROM rt_errors WHERE state = 'UNCAUGHT'") == 8);
  CHECK(count(s, "SELECT json_array_length(events) FROM rt_errors WHERE state = 'UNCAUGHT'") == 3);
  CHECK(count(s, "SELECT COUNT(*) FROM rt_error_traces") == 1);
  // An uncaught exception no application frame saw (no trace) is recorded on its own.
  s.uncaught(session, {{"type", "uncaught"}, {"trace", nullptr}, {"thread", {{"id", 10}, {"name", "other"}}},
                       {"error", error_msg("java.lang.OutOfMemoryError", "UNCAUGHT", json::array())}});
  CHECK(count(s, "SELECT COUNT(*) FROM rt_errors WHERE state = 'UNCAUGHT'") == 2);
  for (int64_t id = 2; id <= 6; ++id) {
    json e = error_msg("E" + std::to_string(id), "CAUGHT", json::array());
    CHECK(s.trace(session, trace_msg(id, {{1, -1, 1, 0, 0}}, "complete", {e})));
  }
  s.commit();
  s.retain();
  CHECK(count(s, "SELECT COUNT(*) FROM rt_traces") == 3);
  CHECK(count(s, "SELECT MIN(agent_trace) FROM rt_traces") == 4);  // the newest three
  CHECK(count(s, "SELECT COUNT(*) FROM rt_errors") == 2);
  CHECK(count(s, "SELECT COUNT(*) FROM rt_error_traces WHERE error_id NOT IN (SELECT id FROM rt_errors)") == 0);
}

TEST_CASE("application packages from the index become agent include prefixes", "[dynamic]") {
  auto roots = dynamic::include_roots({"com.acme.legacy.web", "com.acme.legacy.dao", "com.acme.legacy.web",
                                       "org.apache.kafka.clients", "org.apache.kafka.common.utils", "kafka.server",
                                       "kafka.log", "", "single"});
  CHECK(roots == std::vector<std::string>{"com.acme.legacy", "kafka.log", "kafka.server", "org.apache.kafka", "single",
                                          "<default>"});
  CHECK(dynamic::include_roots({"com.example.shop", "com.example.shop.web", "com.example.shop.service"}) ==
        std::vector<std::string>{"com.example.shop"});
  CHECK(dynamic::include_roots({}).empty());
}

namespace {

// The dynamic-java fixture analyzed into a temporary data dir, plus its runtime store.
struct Observed {
  TempDir data;
  protocol::Engine engine;
  fs::path index;
  fs::path store;

  Observed() {
    auto add = call(engine, "add_project", {{"name", "shop"}, {"path", fixture("dynamic-java").string()}}, data.path());
    REQUIRE(add["result"].is_object());
    index = add["result"]["index_path"].get<std::string>();
    store = dynamic::runtime_store_path(data.path(), 1);
    fs::create_directories(store.parent_path());
  }

  // A line inside the static symbol, as the JVM's line table would report.
  int line(const std::string& qualified, const std::string& params = "*") {
    db::Database db(index, db::OpenMode::ReadOnly);
    auto st = db.prepare("SELECT start_line FROM symbols WHERE qualified_name = ? AND (? = '*' OR IFNULL(param_types, '') = ?)");
    st.bind(1, qualified).bind(2, params).bind(3, params);
    REQUIRE(st.step());
    return static_cast<int>(st.int64(0));
  }

  json q(const std::string& op, json args) {
    args["project"] = "shop";
    json r = call(engine, op, args, data.path());
    REQUIRE(r["status"] == "ok");
    return r["result"];
  }

  int64_t symbol(const std::string& qualified) {
    json r = q("search", {{"query", qualified}, {"kinds", {"symbol"}}, {"exact", true}});
    for (const auto& s : r["symbols"]) {
      if (s["qualified_name"] == qualified) return s["id"].get<int64_t>();
    }
    FAIL("no symbol " << qualified);
    return -1;
  }
};

}  // namespace

TEST_CASE("runtime methods are placed in the static index, with evidence for each observed call", "[dynamic]") {
  Observed o;
  {
    RuntimeStore s(o.store);
    int64_t session = s.open_session(hello("s-1"), json::object());
    s.begin();
    const std::string P = "com.example.shop.";
    s.method(session, method_def(1, P + "web.BaseHandler", "handle", "(Lcom/sun/net/httpserver/HttpExchange;)V", "BaseHandler.java",
                                 o.line(P + "web.BaseHandler.handle"), 0));
    s.method(session, method_def(2, P + "web.UserHandler", "doHandle", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
                                 "UserHandler.java", o.line(P + "web.UserHandler.doHandle"), 0));
    s.method(session, method_def(3, P + "service.UserService", "describe", "(J)Ljava/lang/String;", "UserService.java",
                                 o.line(P + "service.UserService.describe"), 0));
    s.method(session, method_def(4, P + "service.UserService", "format", "(Lcom/example/shop/User;)Ljava/lang/String;",
                                 "UserService.java", o.line(P + "service.UserService.format", "User"), 0));
    s.method(session, method_def(5, P + "service.UserService", "format", "(Lcom/example/shop/User;Z)Ljava/lang/String;",
                                 "UserService.java", o.line(P + "service.UserService.format", "User,boolean"), 0));
    s.method(session, method_def(6, P + "service.UserService", "lambda$describe$0", "(Ljava/lang/String;)V", "UserService.java",
                                 o.line(P + "service.UserService.describe"), 0));
    s.method(session, method_def(7, P + "Bootstrap", "init", "()V", "Bootstrap.java", o.line(P + "Bootstrap.init"), 0));
    s.method(session, method_def(8, P + "plugins.AuditPlugin", "start", "()V", "AuditPlugin.java", o.line(P + "plugins.AuditPlugin.start"), 0));
    s.method(session, method_def(9, P + "gen.Stub", "ping", "()V", "Stub.java", 5, 5));
    s.method(session, method_def(10, P + "web.UserHandler", "<init>", "()V", "UserHandler.java", 5, 5));
    s.method(session, method_def(11, P + "ScenarioClient$1", "run", "()V", "ScenarioClient.java", o.line(P + "ScenarioClient.run.run"), 0));
    s.method(session, method_def(12, P + "repo.PaymentRepository", "save", "(Ljava/lang/String;)V", "PaymentRepository.java",
                                 o.line(P + "repo.PaymentRepository.save"), 0));
    json frames = {{P + "repo.PaymentRepository", "save", "PaymentRepository.java", o.line(P + "repo.PaymentRepository.save") + 2, 1},
                   {P + "service.OrderService", "create", "OrderService.java", o.line(P + "service.OrderService.create") + 2, 1},
                   {"java.lang.Thread", "run", "Thread.java", 1583, 0}};
    CHECK(s.trace(session, trace_msg(1, {{1, -1, 1, 0, 0}, {2, 0, 1, 1, 0}, {3, 1, 1, 2, 0}, {4, 2, 1, 3, 0}, {5, 2, 1, 4, 0},
                                         {6, 2, 1, 5, 0}})));
    CHECK(s.trace(session, trace_msg(2, {{7, -1, 1, 0, 0}, {8, 0, 1, 1, 0}, {9, 1, 1, 2, 0}, {10, 0, 1, 3, 0}, {11, 0, 1, 4, 0}},
                                     "complete", {error_msg("java.sql.SQLException", "WRAPPED", frames, {{"method", 12}, {"line", 8}})})));
    s.commit();
  }

  json t = o.q("runtime_trace", {{"trace_id", 1}});
  const json& n = t["nodes"];
  REQUIRE(n.size() == 6);
  CHECK(n[0]["mapping"] == "exact");
  CHECK(n[1]["symbol"]["qualified_name"] == "com.example.shop.web.UserHandler.doHandle");
  CHECK(n[1]["evidence"] == "STATIC_INFERRED");  // BaseHandler.handle -> abstract doHandle -> this override
  CHECK(n[2]["evidence"] == "STATIC_CONFIRMED");
  // Overloads: each runtime descriptor lands on its own symbol.
  CHECK(n[3]["symbol"]["id"] != n[4]["symbol"]["id"]);
  CHECK(n[3]["symbol"]["line"] == o.line("com.example.shop.service.UserService.format", "User"));
  CHECK(n[4]["symbol"]["line"] == o.line("com.example.shop.service.UserService.format", "User,boolean"));
  // A lambda body belongs to its enclosing method, which the static model credits with its calls.
  CHECK(n[5]["mapping"] == "lambda");
  CHECK(n[5]["symbol"]["qualified_name"] == "com.example.shop.service.UserService.describe");

  json t2 = o.q("runtime_trace", {{"trace_id", 2}});
  const json& m = t2["nodes"];
  CHECK(m[1]["evidence"] == "RUNTIME_OBSERVED");    // reflection: no static call site
  CHECK(m[2]["evidence"] == "RUNTIME_UNRESOLVED");  // generated code outside the index
  CHECK(m[2]["mapping"] == "unresolved");
  CHECK(m[2]["symbol"].is_null());
  CHECK(m[2]["method"]["class"] == "com.example.shop.gen.Stub");
  CHECK(m[3]["mapping"] == "implicit");             // a constructor the source does not declare: its class
  CHECK(m[4]["symbol"]["qualified_name"] == "com.example.shop.ScenarioClient.run.run");  // anonymous class method

  // The Error Tracer keeps the stack as thrown: the deepest frame first; outside frames unmapped.
  json errors = o.q("runtime_errors", json::object());
  REQUIRE(errors["errors"].size() == 1);
  json e = o.q("runtime_error", {{"error_id", errors["errors"][0]["id"]}});
  REQUIRE(e["frames"].size() == 3);
  CHECK(e["frames"][0]["method"] == "save");
  CHECK(e["frames"][0]["symbol"]["qualified_name"] == "com.example.shop.repo.PaymentRepository.save");
  CHECK(e["frames"][1]["symbol"]["qualified_name"] == "com.example.shop.service.OrderService.create");
  CHECK(e["frames"][2]["mapping"] == "external");
  CHECK(e["throw_site"]["symbol"]["qualified_name"] == "com.example.shop.repo.PaymentRepository.save");
  CHECK(e["top_app_frame"]["method"] == "save");

  // Traversals: static edges carry evidence and runtime counts; runtime-only calls are added.
  json fwd = o.q("traverse", {{"symbol_id", o.symbol("com.example.shop.service.UserService.describe")}, {"max_depth", 1}});
  int observed_formats = 0;
  for (const auto& node : fwd["nodes"]) {
    if (node["edge"].is_null() || node["symbol"].is_null()) continue;
    if (node["symbol"]["name"] == "format") {
      CHECK(node["edge"]["evidence"] == "STATIC_CONFIRMED");
      observed_formats += node["edge"].value("observed", 0);
    }
  }
  CHECK(observed_formats == 2);
  json init = o.q("traverse", {{"symbol_id", o.symbol("com.example.shop.Bootstrap.init")}, {"max_depth", 2}});
  bool plugin = false;
  bool stub = false;
  for (const auto& node : init["nodes"]) {
    if (node["edge"].is_null()) continue;
    if (!node["symbol"].is_null() && node["symbol"]["qualified_name"] == "com.example.shop.plugins.AuditPlugin.start") {
      plugin = node["edge"]["evidence"] == "RUNTIME_OBSERVED" && node["edge"]["observed"] == 1 && node["edge"]["status"] == "observed";
    }
    if (node["edge"].value("evidence", std::string()) == "RUNTIME_UNRESOLVED") {
      stub = node["edge"]["target_text"].get<std::string>().find("com.example.shop.gen.Stub.ping") != std::string::npos;
    }
  }
  CHECK(plugin);
  CHECK(stub);
  // Without runtime evidence requested, the traversal is the static one.
  json plain = o.q("traverse", {{"symbol_id", o.symbol("com.example.shop.Bootstrap.init")}, {"max_depth", 2}, {"runtime", false}});
  for (const auto& node : plain["nodes"]) {
    if (!node["edge"].is_null()) CHECK_FALSE(node["edge"].contains("evidence"));
  }
  // The static index is not touched by any of this.
  db::Database idx(o.index, db::OpenMode::ReadOnly);
  CHECK(idx.query_int("SELECT COUNT(*) FROM sqlite_master WHERE name LIKE 'rt_%'").value_or(-1) == 0);
}

TEST_CASE("runtime queries on a project with nothing observed", "[dynamic]") {
  Observed o;
  CHECK(o.q("runtime_traces", json::object())["store"] == false);
  CHECK(o.q("runtime_errors", json::object())["errors"].empty());
  CHECK(o.q("runtime_sessions", json::object())["sessions"].empty());
  json r = call(o.engine, "runtime_trace", {{"project", "shop"}, {"trace_id", 1}}, o.data.path());
  CHECK(r["status"] == "not_found");
  json t = o.q("traverse", {{"symbol_id", o.symbol("com.example.shop.Bootstrap.init")}, {"max_depth", 1}});
  for (const auto& node : t["nodes"]) {
    if (!node["edge"].is_null()) CHECK_FALSE(node["edge"].contains("observed"));
  }
}
