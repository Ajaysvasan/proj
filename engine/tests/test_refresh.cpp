// `refresh_project`: incremental re-indexing must produce exactly the index a
// full analysis of the same source tree would produce.
#include <fstream>
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

std::string read(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void replace_in(const fs::path& p, const std::string& from, const std::string& to) {
  std::string s = read(p);
  auto pos = s.find(from);
  REQUIRE(pos != std::string::npos);
  s.replace(pos, from.size(), to);
  write_file(p, s);
}

// A working copy of a fixture with one data directory for refreshes and a
// fresh one per full analysis used as the reference.
struct Scenario {
  TempDir dir;
  fs::path src;
  fs::path data;
  protocol::Engine engine;
  json first;

  explicit Scenario(const std::string& name) {
    src = dir.path() / "src tree";  // a space in the path on purpose
    fs::copy(fixture(name), src, fs::copy_options::recursive);
    data = dir.path() / "data";
    first = call(engine, "add_project", {{"name", "p"}, {"path", src.string()}}, data);
    REQUIRE(first["result"].is_object());
  }

  // fraction 1.0 forces the selective path (no fallback to full re-resolution).
  json refresh(double fraction = 1.0) {
    return call(engine, "refresh_project", {{"project", "p"}, {"max_affected_fraction", fraction}}, data);
  }

  fs::path index() const { return first["result"]["index_path"].get<std::string>(); }

  // Asserts the refreshed index equals a full analysis of the current tree.
  void check_equivalent() {
    TempDir ref;
    protocol::Engine other;
    auto full = call(other, "add_project", {{"name", "p"}, {"path", src.string()}}, ref.path());
    REQUIRE(full["result"].is_object());
    db::Database a(index(), db::OpenMode::ReadOnly);
    db::Database b(full["result"]["index_path"].get<std::string>(), db::OpenMode::ReadOnly);
    auto da = proj::test::canonical_dump(a);
    auto db_ = proj::test::canonical_dump(b);
    std::vector<std::string> only_refresh, only_full;
    std::set_difference(da.begin(), da.end(), db_.begin(), db_.end(), std::back_inserter(only_refresh));
    std::set_difference(db_.begin(), db_.end(), da.begin(), da.end(), std::back_inserter(only_full));
    for (size_t i = 0; i < only_refresh.size() && i < 10; ++i) UNSCOPED_INFO("only in refreshed: " << only_refresh[i]);
    for (size_t i = 0; i < only_full.size() && i < 10; ++i) UNSCOPED_INFO("only in full:      " << only_full[i]);
    CHECK(only_refresh.empty());
    CHECK(only_full.empty());
    CHECK(IndexReader(index()).check()["ok"] == true);
  }
};

}  // namespace

TEST_CASE("refresh without changes re-parses nothing", "[refresh]") {
  Scenario sc("express-api");
  auto r = sc.refresh();
  REQUIRE(r["status"] == "partial");  // the fixture keeps its syntax error
  const auto& rs = r["result"]["stats"]["refresh"];
  CHECK(rs["changed_files"] == 0);
  CHECK(rs["added_files"] == 0);
  CHECK(rs["removed_files"] == 0);
  CHECK(rs["unchanged_files"] == 26);
  CHECK(rs["affected_files"] == 0);
  CHECK(r["result"]["mode"] == "refresh");
  sc.check_equivalent();
}

TEST_CASE("refresh: modified bodies, renamed and duplicated names (express)", "[refresh]") {
  Scenario sc("express-api");
  // Body change in a called function.
  replace_in(sc.src / "src/repositories/userRepository.js", "return db.query('SELECT * FROM users WHERE id = $1', [id]);",
             "const rows = db.query('SELECT * FROM users WHERE id = $1', [id]);\n    return rows;");
  auto r1 = sc.refresh();
  CHECK(r1["result"]["stats"]["refresh"]["changed_files"] == 1);
  sc.check_equivalent();

  // Rename: callers of the old name become unresolved, nothing keeps pointing at it.
  replace_in(sc.src / "src/utils/text.js", "function format(message)", "function formatText(message)");
  replace_in(sc.src / "src/utils/text.js", "module.exports = { format };", "module.exports = { formatText };");
  sc.refresh();
  sc.check_equivalent();

  // A second function with an existing name in a new file must not capture
  // calls that resolve to the original by import.
  write_file(sc.src / "src/utils/date2.js", "function format(d) {\n  return String(d);\n}\n\nmodule.exports = { format };\n");
  write_file(sc.src / "src/report.js",
             "const { format } = require('./utils/date2');\nconst date = require('./utils/date');\n\n"
             "function report(d) {\n  return format(d) + date.format(d);\n}\n\nmodule.exports = { report };\n");
  auto r3 = sc.refresh();
  CHECK(r3["result"]["stats"]["refresh"]["added_files"] == 2);
  sc.check_equivalent();
  {
    db::Database db(sc.index(), db::OpenMode::ReadOnly);
    auto st = db.prepare(
        "SELECT e.target_text, tf.path FROM edges e JOIN symbols s ON s.id = e.src_symbol_id JOIN symbols t ON "
        "t.id = e.dst_symbol_id JOIN files tf ON tf.id = t.file_id WHERE s.name = 'report' ORDER BY e.col");
    std::map<std::string, std::string> targets;
    while (st.step()) targets[st.text(0)] = st.text(1);
    CHECK(targets["format"] == "src/utils/date2.js");
    CHECK(targets["date.format"] == "src/utils/date.js");
    // The original caller of date.format is unaffected.
    auto fu = db.prepare(
        "SELECT tf.path FROM edges e JOIN symbols s ON s.id = e.src_symbol_id JOIN symbols t ON t.id = e.dst_symbol_id "
        "JOIN files tf ON tf.id = t.file_id WHERE s.name = 'formatUser' AND e.target_name = 'format'");
    REQUIRE(fu.step());
    CHECK(fu.text(0) == "src/utils/date.js");
  }

  // Remove a file: importers' dependencies become unresolved, no dangling rows remain.
  fs::remove(sc.src / "src/utils/date.js");
  auto r4 = sc.refresh();
  CHECK(r4["result"]["stats"]["refresh"]["removed_files"] == 1);
  sc.check_equivalent();
}

TEST_CASE("refresh: routes, mounts, inheritance and exclusions (express)", "[refresh]") {
  Scenario sc("express-api");
  replace_in(sc.src / "src/routes/users.js", "module.exports = router;",
             "router.patch('/:id', authenticate, userController.getUser);\n\nmodule.exports = router;");
  replace_in(sc.src / "src/app.js", "app.use('/api/orders', orderRoutes);", "app.use('/api/v2/orders', orderRoutes);");
  sc.refresh();
  sc.check_equivalent();
  IndexReader idx(sc.index());
  auto found = idx.search("PATCH /api/users/7", {"route"}, false, 5)["routes"];
  REQUIRE(found.size() == 1);
  CHECK(found[0]["handler_name"] == "getUser");
  auto orders = idx.search("GET /api/v2/orders", {"route"}, false, 5)["routes"];
  REQUIRE(orders.size() >= 1);
  CHECK(orders[0]["full_path"] == "/api/v2/orders");
  CHECK(idx.search("GET /api/orders", {"route"}, true, 5)["routes"].empty());  // old prefix is gone

  replace_in(sc.src / "src/errors.js", "class NotFoundError extends AppError {", "class NotFoundError extends Error {");
  sc.refresh();
  sc.check_equivalent();

  write_file(sc.src / ".projignore", "src/legacy.js\n");
  auto r = sc.refresh();
  CHECK(r["result"]["stats"]["refresh"]["changed_files"].get<int64_t>() >= 1);
  sc.check_equivalent();

  // Fix the syntax error: the analysis status becomes success.
  write_file(sc.src / "src/broken.js", "function broken() {\n  return 42;\n}\n");
  auto fixed = sc.refresh();
  CHECK(fixed["result"]["status"] == "success");
  sc.check_equivalent();
}

TEST_CASE("refresh: python modules and methods", "[refresh][python]") {
  Scenario sc("python-app");
  replace_in(sc.src / "app/repositories/user_repository.py", "    def find(self, user_id):", "    def find_one(self, user_id):");
  sc.refresh();
  sc.check_equivalent();
  replace_in(sc.src / "app/services/user_service.py", "user = self.repo.find(user_id)", "user = self.repo.find_one(user_id)");
  write_file(sc.src / "app/services/audit.py", "class UserService:\n    def get_user(self, user_id):\n        return None\n");
  sc.refresh();
  sc.check_equivalent();
  fs::remove(sc.src / "app/errors.py");
  sc.refresh();
  sc.check_equivalent();
}

TEST_CASE("refresh: java interfaces, overloads and same-package classes", "[refresh][java]") {
  Scenario sc("java-spring");
  auto svc = sc.src / "src/main/java/com/example/demo/service";
  replace_in(svc / "UserServiceImpl.java", "public class UserServiceImpl implements UserService {",
             "public class UserServiceImpl {");
  sc.refresh();
  sc.check_equivalent();
  replace_in(svc / "UserServiceImpl.java", "    private void validate(User user) {\n        validate(user, false);\n    }",
             "    private void validate(User user) {\n        validate(user, false, 1);\n    }\n\n"
             "    private void validate(User user, boolean strict, int level) {\n    }");
  write_file(svc / "Helpers.java",
             "package com.example.demo.service;\n\nclass Helpers {\n    static void log(String m) {}\n}\n");
  replace_in(svc / "UserServiceImpl.java", "        validate(user);\n", "        validate(user);\n        Helpers.log(\"x\");\n");
  sc.refresh();
  sc.check_equivalent();
}

TEST_CASE("refresh: C++ header and out-of-line definitions", "[refresh][cpp]") {
  Scenario sc("cpp-app");
  replace_in(sc.src / "include/store/user_store.h", "  void add(const User& user);", "  void add(const User& user);\n  void clear();");
  replace_in(sc.src / "src/user_store.cpp", "}  // namespace store", "void UserStore::clear() { users_.clear(); }\n\n}  // namespace store");
  replace_in(sc.src / "src/main.cpp", "  return 0;\n}", "  users.clear();\n  return 0;\n}");
  sc.refresh();
  sc.check_equivalent();
  fs::remove(sc.src / "src/util.h");
  sc.refresh();
  sc.check_equivalent();
}

TEST_CASE("refresh: randomized edits match a full analysis", "[refresh][random]") {
  // Selective re-resolution only (fraction 1.0) and with the default fallback.
  double fraction = GENERATE(1.0, 0.4);
  Scenario sc("express-api");
  std::mt19937 rng(20261011);
  int selective_rounds = 0;
  auto source_files = [&] {
    std::vector<fs::path> files;
    for (auto& e : fs::recursive_directory_iterator(sc.src / "src")) {
      if (e.is_regular_file() && (e.path().extension() == ".js" || e.path().extension() == ".ts")) files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    return files;
  };
  const std::vector<std::string> names = {"format", "findById", "getUser", "authenticate", "query", "loadConfig",
                                          "validateEmail", "ping", "pong", "Store", "UserService"};
  for (int round = 0; round < 40; ++round) {
    auto files = source_files();
    REQUIRE(!files.empty());
    int op = static_cast<int>(rng() % 5);
    auto pick = files[rng() % files.size()];
    std::string name = names[rng() % names.size()];
    switch (op) {
      case 0: {  // copy a file under a new name (duplicate definitions)
        fs::copy_file(pick, pick.parent_path() / ("copy" + std::to_string(round) + pick.extension().string()));
        break;
      }
      case 1: {  // delete a file
        fs::remove(pick);
        break;
      }
      case 2: {  // rename one identifier inside one file
        std::string s = read(pick);
        s = std::regex_replace(s, std::regex("\\b" + name + "\\b"), name + "Renamed");
        write_file(pick, s);
        break;
      }
      case 3: {  // add a function that calls an existing name
        std::string s = read(pick);
        s += "\nfunction added" + std::to_string(round) + "(x) {\n  return " + name + "(x);\n}\n";
        write_file(pick, s);
        break;
      }
      case 4: {  // add a new module defining an existing name and importing another file
        auto other = files[rng() % files.size()];
        auto rel = fs::relative(other, sc.src / "src").replace_extension().generic_string();
        write_file(sc.src / "src" / ("new" + std::to_string(round) + ".js"),
                   "const dep = require('./" + rel + "');\n\nfunction " + name + "(x) {\n  return dep." + name +
                       "(x);\n}\n\nmodule.exports = { " + name + " };\n");
        break;
      }
    }
    INFO("round " << round << " op " << op << " on " << pick.filename() << " name " << name << " fraction " << fraction);
    auto r = sc.refresh(fraction);
    REQUIRE(r["result"].is_object());
    if (!r["result"]["stats"]["refresh"]["full_reresolve"].get<bool>()) ++selective_rounds;
    sc.check_equivalent();
  }
  if (fraction == 1.0) CHECK(selective_rounds == 40);
}

TEST_CASE("refresh errors: unknown project, missing directory, missing index", "[refresh]") {
  TempDir data, src;
  write_file(src.path() / "a.js", "function a() {}\n");
  protocol::Engine engine;
  CHECK(call(engine, "refresh_project", {{"project", "ghost"}}, data.path())["error"]["code"] == "unknown_project");
  auto added = call(engine, "add_project", {{"name", "p"}, {"path", src.path().string()}}, data.path());
  REQUIRE(added["status"] == "ok");
  fs::path index = added["result"]["index_path"].get<std::string>();
  fs::remove(index);
  CHECK(call(engine, "refresh_project", {{"project", "p"}}, data.path())["error"]["code"] == "index_missing");
}
