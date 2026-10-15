// Resolution of chained calls, typed receivers, static imports, inherited
// external methods and constructor-injected fields.
#include <map>

#include "db/sqlite.hpp"
#include "helpers.hpp"

using namespace proj;
using proj::test::call;
using proj::test::TempDir;
using proj::test::write_file;
namespace fs = std::filesystem;

namespace {

struct Analyzed {
  TempDir data;
  json result;
  // (caller qualified name, call text) -> (status, target qualified name, note)
  std::map<std::pair<std::string, std::string>, std::tuple<std::string, std::string, std::string>> edges;
};

std::unique_ptr<Analyzed> analyze(const fs::path& root) {
  auto a = std::make_unique<Analyzed>();
  protocol::Engine engine;
  a->result = call(engine, "add_project", {{"name", "p"}, {"path", root.string()}}, a->data.path());
  REQUIRE(a->result["result"].is_object());
  db::Database db(a->result["result"]["index_path"].get<std::string>(), db::OpenMode::ReadOnly);
  auto st = db.prepare(
      "SELECT s.qualified_name, e.target_text, e.status, IFNULL(t.qualified_name, ''), IFNULL(e.note, '') FROM edges e "
      "JOIN symbols s ON s.id = e.src_symbol_id LEFT JOIN symbols t ON t.id = e.dst_symbol_id "
      "WHERE e.kind IN ('calls','instantiates')");
  while (st.step()) a->edges[{st.text(0), st.text(1)}] = {st.text(2), st.text(3), st.text(4)};
  return a;
}

std::string status_of(const Analyzed& a, const std::string& caller, const std::string& text) {
  auto it = a.edges.find({caller, text});
  if (it == a.edges.end()) return "<no edge>";
  return std::get<0>(it->second);
}

std::string target_of(const Analyzed& a, const std::string& caller, const std::string& text) {
  auto it = a.edges.find({caller, text});
  return it == a.edges.end() ? "<no edge>" : std::get<1>(it->second);
}

std::string note_of(const Analyzed& a, const std::string& caller, const std::string& text) {
  auto it = a.edges.find({caller, text});
  return it == a.edges.end() ? "<no edge>" : std::get<2>(it->second);
}

}  // namespace

TEST_CASE("Java: chains, casts, new, static imports, inherited external methods", "[resolution][java]") {
  TempDir src;
  auto d = src.path() / "src/a";
  write_file(d / "Repo.java", "package a;\npublic class Repo {\n  public User find(long id) { return null; }\n}\n");
  write_file(d / "User.java",
             "package a;\npublic class User {\n  public String name() { return \"\"; }\n  public User self() { return this; }\n}\n");
  write_file(d / "Util.java", "package a;\npublic class Util {\n  public static void helper() {}\n}\n");
  write_file(d / "Service.java",
             "package a;\n\nimport static a.Util.helper;\nimport static org.junit.Assert.*;\nimport java.util.HashMap;\n\n"
             "public class Service extends HashMap<String, String> {\n"
             "  private final Repo repo = new Repo();\n"
             "  void run(Object obj) {\n"
             "    repo.find(1).name();\n"
             "    new Repo().find(2);\n"
             "    ((User) obj).self();\n"
             "    helper();\n"
             "    assertEquals(1, 1);\n"
             "    put(\"k\", \"v\");\n"
             "    repo.find(1).name().trim();\n"
             "    var u = repo.find(3);\n"
             "    u.name();\n"
             "  }\n}\n");
  auto a = analyze(src.path());
  const std::string run = "a.Service.run";
  CHECK(target_of(*a, run, "repo.find(1).name") == "a.User.name");
  CHECK(note_of(*a, run, "repo.find(1).name") == "via return type User");
  CHECK(target_of(*a, run, "new Repo().find") == "a.Repo.find");
  CHECK(target_of(*a, run, "((User) obj).self") == "a.User.self");
  CHECK(target_of(*a, run, "helper") == "a.Util.helper");
  CHECK(status_of(*a, run, "assertEquals") == "external");
  CHECK(note_of(*a, run, "assertEquals").find("org.junit.Assert.*") != std::string::npos);
  // Both an external base class and a static wildcard import could supply these:
  // the edge is external and the note names both possibilities.
  CHECK(status_of(*a, run, "put") == "external");
  CHECK(note_of(*a, run, "put").find("inherited from an external base class or from static import org.junit.Assert.*") !=
        std::string::npos);
  CHECK(status_of(*a, run, "repo.find(1).name().trim") == "external");
  // var u = repo.find(3): typed from find's declared return type.
  CHECK(target_of(*a, run, "u.name") == "a.User.name");
}

TEST_CASE("Python: injected fields, return annotations and built-in types", "[resolution][python]") {
  TempDir src;
  write_file(src.path() / "models.py",
             "class User:\n    def name(self) -> str:\n        return ''\n\n\n"
             "class Repo:\n    def find(self, id) -> 'User':\n        return User()\n");
  write_file(src.path() / "service.py",
             "from models import Repo\n\n\nclass Service:\n    def __init__(self, repo: Repo):\n        self.repo = repo\n\n"
             "    def run(self):\n        self.repo.find(1).name().upper()\n");
  auto a = analyze(src.path());
  const std::string run = "service.Service.run";
  CHECK(target_of(*a, run, "self.repo.find") == "models.Repo.find");
  CHECK(target_of(*a, run, "self.repo.find(1).name") == "models.User.name");
  CHECK(status_of(*a, run, "self.repo.find(1).name().upper") == "external");
  CHECK(note_of(*a, run, "self.repo.find(1).name().upper") == "models.User.name returns str (Python built-in type)");
}

TEST_CASE("Values returned by calls: external values and declared return types", "[resolution][python]") {
  TempDir src;
  write_file(src.path() / "factory.py",
             "class Client:\n    def send(self, msg):\n        return msg\n\n\n"
             "def make_client() -> Client:\n    return Client()\n\n\ndef untyped():\n    return Client()\n");
  write_file(src.path() / "app.py",
             "import logging\nfrom factory import make_client, untyped\n\nlogger = logging.getLogger(__name__)\n\n\n"
             "class Worker:\n    def __init__(self):\n        self.client = make_client()\n\n"
             "    def run(self):\n        logger.info('start')\n        self.client.send('x')\n"
             "        other = untyped()\n        other.send('y')\n");
  auto a = analyze(src.path());
  const std::string run = "app.Worker.run";
  CHECK(status_of(*a, run, "logger.info") == "external");
  CHECK(note_of(*a, run, "logger.info") == "receiver type @call:logging.getLogger (value returned by external logging.getLogger)");
  CHECK(target_of(*a, run, "self.client.send") == "factory.Client.send");
  CHECK(status_of(*a, run, "other.send") == "unresolved");  // untyped() declares no return type
  CHECK(note_of(*a, run, "other.send") == "receiver 'other' holds the value returned by untyped, whose type is not known");
}

TEST_CASE("JavaScript and Java: values returned by calls", "[resolution][ts][java]") {
  TempDir src;
  write_file(src.path() / "db.ts",
             "import { Pool } from 'pg';\n\nexport class Repo {\n  find(id: number): number { return id; }\n}\n\n"
             "export async function connect(): Promise<Repo> { return new Repo(); }\n\n"
             "export async function load() {\n  const repo = await connect();\n  repo.find(1);\n"
             "  const pool = new Pool();\n  const client = await pool.connect();\n  client.query('x');\n}\n");
  write_file(src.path() / "src/a/Svc.java",
             "package a;\n\npublic class Svc {\n  public Svc self() { return this; }\n  public void go() {}\n"
             "  void run() {\n    var s = self();\n    s.go();\n  }\n}\n");
  auto a = analyze(src.path());
  CHECK(target_of(*a, "load", "repo.find") == "Repo.find");
  CHECK(status_of(*a, "load", "client.query") == "external");
  CHECK(target_of(*a, "a.Svc.run", "s.go") == "a.Svc.go");
}

TEST_CASE("TypeScript: await unwraps Promise<T>; new and as receivers", "[resolution][ts]") {
  TempDir src;
  write_file(src.path() / "repo.ts",
             "export class User {\n  name(): string { return ''; }\n}\n\n"
             "export class Repo {\n  async find(id: number): Promise<User> { return new User(); }\n}\n");
  write_file(src.path() / "svc.ts",
             "import { Repo, User } from './repo';\n\nexport async function run(r: Repo, x: unknown) {\n"
             "  (await r.find(1)).name();\n  new Repo().find(2);\n  (x as User).name();\n  r.find(3).then(() => 1);\n}\n");
  auto a = analyze(src.path());
  CHECK(target_of(*a, "run", "(await r.find(1)).name") == "User.name");
  CHECK(target_of(*a, "run", "new Repo().find") == "Repo.find");
  CHECK(target_of(*a, "run", "(x as User).name") == "User.name");
  CHECK(status_of(*a, "run", "r.find(3).then") == "external");  // Promise is a JS built-in
}

TEST_CASE("C++: chained calls through declared return types", "[resolution][cpp]") {
  TempDir src;
  write_file(src.path() / "a.h", "struct B {\n  int size() const;\n};\n\nstruct A {\n  B get() const;\n};\n");
  write_file(src.path() / "a.cpp",
             "#include \"a.h\"\n\nint B::size() const { return 0; }\nB A::get() const { return B(); }\n\n"
             "int main() {\n  A a;\n  return a.get().size();\n}\n");
  auto a = analyze(src.path());
  CHECK(target_of(*a, "main", "a.get") == "A::get");
  CHECK(target_of(*a, "main", "a.get().size") == "B::size");
}

TEST_CASE("Java: method references resolve like the calls they stand for", "[resolution][java]") {
  TempDir src;
  auto d = src.path() / "src/a";
  write_file(d / "Util.java", "package a;\npublic class Util {\n  public static void print(Object o) {}\n}\n");
  write_file(d / "Worker.java", "package a;\npublic class Worker {\n  public void work() {}\n}\n");
  write_file(d / "Repo.java", "package a;\npublic class Repo {\n  public Repo() {}\n}\n");
  write_file(d / "Jobs.java",
             "package a;\n\nimport java.util.List;\nimport java.util.concurrent.Executor;\n\n"
             "public class Jobs {\n"
             "  void start(Executor pool, Worker w, List<Object> items) {\n"
             "    pool.execute(this::drain);\n"
             "    items.forEach(Util::print);\n"
             "    pool.execute(w::work);\n"
             "    items.forEach(this::handle);\n"
             "    items.stream().map(String::valueOf);\n"
             "    pool.execute(unknown::run);\n"
             "    java.util.function.Supplier<Repo> s = Repo::new;\n"
             "  }\n"
             "  void drain() {}\n"
             "  void handle(Object o) {}\n"
             "  void handle(Object o, int n) {}\n"
             "}\n");
  auto a = analyze(src.path());
  const std::string start = "a.Jobs.start";
  CHECK(target_of(*a, start, "this::drain") == "a.Jobs.drain");
  CHECK(status_of(*a, start, "this::drain") == "resolved");
  CHECK(target_of(*a, start, "Util::print") == "a.Util.print");
  CHECK(target_of(*a, start, "w::work") == "a.Worker.work");
  CHECK(target_of(*a, start, "Repo::new") == "a.Repo.Repo");
  // The argument count is not visible: both overloads remain candidates.
  CHECK(status_of(*a, start, "this::handle") == "ambiguous");
  CHECK(status_of(*a, start, "String::valueOf") == "external");
  CHECK(status_of(*a, start, "unknown::run") == "unresolved");
}
