// Leak checks without external tools: glibc's mallinfo2() reports every byte
// currently allocated through malloc (C++ new, SQLite and Tree-sitter all use
// it). After a warm-up cycle (one-time library initialization), repeating a
// complete engine lifecycle must return the allocator to the same baseline,
// and repeated queries on a long-lived engine must not accumulate memory.
//
// glibc's per-thread cache (tcache) keeps up to 7 freed chunks per size class,
// and mallinfo2() counts them as in use, so with it enabled the totals creep up
// for several cycles before levelling off. The counts are exact only with the
// cache disabled; CTest runs these tests with
// GLIBC_TUNABLES=glibc.malloc.tcache_count=0 (see tests/CMakeLists.txt).
#include <cstdlib>

#include <stdexcept>

#include <tree_sitter/api.h>

#include "dynamic/runtime_store.hpp"
#include "dynamic/service.hpp"
#include "helpers.hpp"
#include "parse/ts_util.hpp"
#include "query/query.hpp"

#if defined(__GLIBC__)
#include <malloc.h>
#endif

extern "C" const TSLanguage* tree_sitter_javascript();

using namespace proj;
using proj::test::call;
using proj::test::fixture;
using proj::test::TempDir;
using proj::test::write_file;
namespace fs = std::filesystem;

#if defined(__GLIBC__)
namespace {

bool exact_accounting() {
  const char* t = std::getenv("GLIBC_TUNABLES");
  return t && std::string(t).find("glibc.malloc.tcache_count=0") != std::string::npos;
}

#define REQUIRE_EXACT_ACCOUNTING()                                                              \
  if (!exact_accounting()) SKIP("run with GLIBC_TUNABLES=glibc.malloc.tcache_count=0 (ctest does)")

size_t bytes_in_use() {
  malloc_trim(0);
  struct mallinfo2 mi = mallinfo2();
  return mi.uordblks + mi.hblkhd;  // small-block arena + mmapped blocks
}

// One complete lifecycle: analyze, modify, refresh, run every query type, destroy.
// Returns the number of failed operations (assertions happen outside measurement).
int engine_lifecycle(const fs::path& work) {
  int failures = 0;
  TempDir data;
  fs::path src = work / "src";
  std::error_code ec;
  fs::remove_all(src, ec);
  fs::copy(fixture("express-api"), src, fs::copy_options::recursive);
  {
    protocol::Engine engine;
    auto add = call(engine, "add_project", {{"name", "p"}, {"path", src.string()}}, data.path());
    if (!add["result"].is_object()) return 1;
    write_file(src / "src/utils/extra.js", "function extra() {\n  return 1;\n}\n\nmodule.exports = { extra };\n");
    if (call(engine, "refresh_project", {{"project", "p"}}, data.path())["status"] != "partial") ++failures;
    auto found = call(engine, "search", {{"project", "p"}, {"query", "getUser"}}, data.path());
    int64_t sym = found["result"]["symbols"][0]["id"].get<int64_t>();
    auto route = call(engine, "search", {{"project", "p"}, {"query", "GET /api/users/1"}, {"kinds", {"route"}}}, data.path());
    int64_t route_id = route["result"]["routes"][0]["id"].get<int64_t>();
    for (const auto& [op, args] : std::vector<std::pair<std::string, json>>{
             {"symbol", {{"symbol_id", sym}}},
             {"edges", {{"symbol_id", sym}, {"direction", "in"}}},
             {"traverse", {{"symbol_id", sym}, {"max_depth", 6}}},
             {"traverse", {{"symbol_id", sym}, {"direction", "backward"}}},
             {"route_flow", {{"route_id", route_id}}},
             {"exceptions", {{"symbol_id", sym}}},
             {"file", {{"path", "src/app.js"}}},
             {"file_deps", {{"file_id", 1}}},
             {"entry_points", json::object()},
             {"routes", json::object()},
             {"check_index", json::object()},
             {"snippet", {{"file_id", 1}, {"line", 3}}},
             {"list_runs", json::object()},
         }) {
      json a = args;
      a["project"] = "p";
      auto r = call(engine, op, a, data.path());
      if (r["status"] != "ok") ++failures;
    }
  }
  return failures;
}

}  // namespace

TEST_CASE("a complete engine lifecycle releases all memory it allocates", "[leak]") {
  REQUIRE_EXACT_ACCOUNTING();
  TempDir work;
  REQUIRE(engine_lifecycle(work.path()) == 0);  // warm-up: one-time library initialization
  size_t baseline = bytes_in_use();
  int failures = 0;
  for (int i = 0; i < 5; ++i) failures += engine_lifecycle(work.path());
  size_t after = bytes_in_use();
  CHECK(failures == 0);
  INFO("baseline " << baseline << " bytes, after 5 lifecycles " << after << " bytes");
  // Allocator bookkeeping differs by a few hundred bytes at most; a leak in
  // any analyzed file, query or statement would grow by kilobytes per cycle.
  CHECK(after <= baseline + 4096);
}

TEST_CASE("repeated queries on a long-lived engine do not accumulate memory", "[leak]") {
  REQUIRE_EXACT_ACCOUNTING();
  TempDir data;
  protocol::Engine engine;
  auto add = call(engine, "add_project", {{"name", "p"}, {"path", fixture("express-api").string()}}, data.path());
  REQUIRE(add["result"].is_object());
  auto found = call(engine, "search", {{"project", "p"}, {"query", "getUser"}}, data.path());
  int64_t sym = found["result"]["symbols"][0]["id"].get<int64_t>();
  auto run_queries = [&](int n) {
    int bad = 0;
    for (int i = 0; i < n; ++i) {
      bad += call(engine, "search", {{"project", "p"}, {"query", i % 2 ? "find" : "format"}}, data.path())["status"] != "ok";
      bad += call(engine, "traverse", {{"project", "p"}, {"symbol_id", sym}, {"max_depth", 6}}, data.path())["status"] != "ok";
      bad += call(engine, "exceptions", {{"project", "p"}, {"symbol_id", sym}}, data.path())["status"] != "ok";
    }
    return bad;
  };
  REQUIRE(run_queries(200) == 0);  // warm caches (SQLite page cache, statement caches)
  size_t baseline = bytes_in_use();
  int bad = run_queries(2000);
  size_t after = bytes_in_use();
  CHECK(bad == 0);
  INFO("baseline " << baseline << " bytes, after 6000 queries " << after << " bytes");
  CHECK(after <= baseline + 4096);
}

TEST_CASE("runtime ingest and runtime queries release their memory", "[leak]") {
  REQUIRE_EXACT_ACCOUNTING();
  TempDir data;
  protocol::Engine engine;
  auto add = call(engine, "add_project", {{"name", "p"}, {"path", fixture("dynamic-java").string()}}, data.path());
  REQUIRE(add["result"].is_object());
  fs::path store = dynamic::runtime_store_path(data.path(), 1);
  fs::create_directories(store.parent_path());
  int64_t next = 1;
  // One service-like batch: open the store, record traces with errors, close it.
  auto ingest = [&](int traces) {
    dynamic::RuntimeStore s(store, dynamic::Retention{200, 50, 10});
    int64_t session = s.open_session({{"session", "s-" + std::to_string(next)}, {"mode", "startup"}, {"since_start", true}}, json::object());
    s.begin();
    s.method(session, {{"id", 1}, {"class", "com.example.shop.Bootstrap"}, {"name", "init"}, {"desc", "()V"},
                       {"source", "Bootstrap.java"}, {"first_line", 9}, {"last_line", 12}});
    s.method(session, {{"id", 2}, {"class", "com.example.shop.Config"}, {"name", "load"}, {"desc", "()Lcom/example/shop/Config;"},
                       {"source", "Config.java"}, {"first_line", 11}, {"last_line", 11}});
    for (int i = 0; i < traces; ++i, ++next) {
      json err = {{"exception", "java.io.IOException"}, {"message", "m" + std::to_string(next % 7)}, {"state", "CAUGHT"},
                  {"throw", {{"method", 2}, {"line", 11}}}, {"frames", {{"com.example.shop.Config", "load", "Config.java", 11, 1}}},
                  {"events", {{"thrown", 2, 11}, {"caught", 1, 10}}}};
      s.trace(session, {{"id", next}, {"status", "complete"}, {"thread", {{"id", 1}, {"name", "t"}}}, {"start_ms", next},
                        {"duration_us", 5}, {"nodes", {{1, -1, 1, 0, 0}, {2, 0, 2, 1, 0}}}, {"errors", {err}}});
    }
    s.commit();
    s.retain();
    s.close_session(session, "ended", "the JVM exited");
  };
  auto found = call(engine, "search", {{"project", "p"}, {"query", "Bootstrap.init"}}, data.path());
  int64_t sym = found["result"]["symbols"][0]["id"].get<int64_t>();
  auto queries = [&](int n) {
    int bad = 0;
    for (int i = 0; i < n; ++i) {
      bad += call(engine, "runtime_traces", {{"project", "p"}, {"limit", 50}}, data.path())["status"] != "ok";
      auto traces = call(engine, "runtime_traces", {{"project", "p"}, {"limit", 1}}, data.path());
      int64_t trace = traces["result"]["traces"][0]["id"].get<int64_t>();
      bad += call(engine, "runtime_trace", {{"project", "p"}, {"trace_id", trace}}, data.path())["status"] != "ok";
      auto errors = call(engine, "runtime_errors", {{"project", "p"}}, data.path());
      bad += errors["status"] != "ok";
      bad += call(engine, "runtime_error", {{"project", "p"}, {"error_id", errors["result"]["errors"][0]["id"]}}, data.path())["status"] != "ok";
      bad += call(engine, "traverse", {{"project", "p"}, {"symbol_id", sym}, {"max_depth", 3}}, data.path())["status"] != "ok";
    }
    return bad;
  };
  ingest(50);
  REQUIRE(queries(20) == 0);  // warm caches
  size_t baseline = bytes_in_use();
  for (int i = 0; i < 5; ++i) ingest(50);
  int bad = queries(200);
  size_t after = bytes_in_use();
  CHECK(bad == 0);
  INFO("baseline " << baseline << " bytes, after 250 traces and 1200 runtime queries " << after << " bytes");
  CHECK(after <= baseline + 4096);
}

TEST_CASE("parsing and extraction release syntax trees and cursors", "[leak]") {
  REQUIRE_EXACT_ACCOUNTING();
  ParserManager pm;
  std::string code = "class A extends B {\n  m(x) { try { return this.f(x).g(); } catch (e) { throw new E(e); } }\n}\n";
  for (int i = 0; i < 50; ++i) pm.parse(Language::TypeScript, "a.ts", code);
  size_t baseline = bytes_in_use();
  for (int i = 0; i < 2000; ++i) {
    pm.parse(Language::TypeScript, "a.ts", code);
    pm.parse(Language::Python, "a.py", "def f(x):\n    raise ValueError(x)\n");
    pm.parse(Language::Java, "A.java", "class A { void m() { new B().c(); } }\n");
    pm.parse(Language::Cpp, "a.cpp", "int main() { try { f(); } catch (...) {} }\n");
    pm.parse(Language::JavaScript, "bad.js", "function broken( {\n");
  }
  size_t after = bytes_in_use();
  INFO("baseline " << baseline << " bytes, after 10000 parses " << after << " bytes");
  CHECK(after <= baseline + 4096);
}

TEST_CASE("a tree walk aborted by an exception releases its cursor", "[leak]") {
  REQUIRE_EXACT_ACCOUNTING();
  TSParser* parser = ts_parser_new();
  ts_parser_set_language(parser, tree_sitter_javascript());
  std::string code = "function a() { return b(c(d(1))); }\n";
  TSTree* tree = ts_parser_parse_string(parser, nullptr, code.data(), static_cast<uint32_t>(code.size()));
  TSNode root = ts_tree_root_node(tree);
  // An extractor that throws part-way through the tree (as on malformed input).
  auto aborted_walk = [&] {
    int seen = 0;
    try {
      ts::walk(root, [&](TSNode) {
        if (++seen == 6) throw std::runtime_error("extractor failed");
        return true;
      }, [](TSNode) {});
    } catch (const std::runtime_error&) {
      return 1;
    }
    return 0;
  };
  for (int i = 0; i < 50; ++i) aborted_walk();
  size_t baseline = bytes_in_use();
  int thrown = 0;
  for (int i = 0; i < 2000; ++i) thrown += aborted_walk();
  size_t after = bytes_in_use();
  ts_tree_delete(tree);
  ts_parser_delete(parser);
  CHECK(thrown == 2000);
  INFO("baseline " << baseline << " bytes, after 2000 aborted walks " << after << " bytes");
  CHECK(after <= baseline + 4096);
}
#else
TEST_CASE("memory accounting requires glibc", "[leak]") { SKIP("mallinfo2() is glibc-specific"); }
#endif
