// Paths, ignore rules, language selection and the scanner.
#include <cstdlib>
#include <map>
#include <set>

#include <unistd.h>

#include "core/hash.hpp"
#include "core/paths.hpp"
#include "helpers.hpp"
#include "index/registry.hpp"
#include "scan/ignore.hpp"
#include "scan/language.hpp"
#include "scan/scanner.hpp"

using namespace proj;
using proj::test::TempDir;
using proj::test::write_file;

TEST_CASE("relative paths are normalized lexically and cannot escape the root", "[paths]") {
  CHECK(paths::normalize_relative("a/./b/../c.js") == "a/c.js");
  CHECK(paths::normalize_relative("a//b/") == "a/b");
  CHECK(paths::normalize_relative("") == "");
  CHECK_FALSE(paths::normalize_relative("../x").has_value());
  CHECK_FALSE(paths::normalize_relative("a/../../x").has_value());
  CHECK_FALSE(paths::normalize_relative("/etc/passwd").has_value());
  CHECK(paths::join_relative("src/routes", "../controllers/user") == "src/controllers/user");
  CHECK(paths::join_relative("", "./a") == "a");
  CHECK_FALSE(paths::join_relative("src", "../../outside").has_value());
}

TEST_CASE("path components", "[paths]") {
  CHECK(paths::parent_dir("a/b/c.js") == "a/b");
  CHECK(paths::parent_dir("c.js") == "");
  CHECK(paths::base_name("a/b/c.test.js") == "c.test.js");
  CHECK(paths::extension("a/B.TS") == ".ts");
  CHECK(paths::extension("Makefile") == "");
  CHECK(paths::extension(".gitignore") == "");
  CHECK(paths::stem("a/c.test.js") == "c.test");
}

TEST_CASE("relative_to_root only accepts paths inside the root", "[paths]") {
  CHECK(paths::relative_to_root("/p/root", "/p/root/src/a.js") == "src/a.js");
  CHECK(paths::relative_to_root("/p/root", "/p/root") == "");
  CHECK_FALSE(paths::relative_to_root("/p/root", "/p/rootx/a.js").has_value());
  CHECK_FALSE(paths::relative_to_root("/p/root", "/p/other/a.js").has_value());
  CHECK(paths::relative_to_root("/p/dir with space", "/p/dir with space/é.js") == "é.js");
}

TEST_CASE("PROJ_HOME overrides the data directory", "[paths]") {
  const char* old = std::getenv("PROJ_HOME");
  std::string saved = old ? old : "";
  setenv("PROJ_HOME", "/tmp/custom-proj-home", 1);
  CHECK(paths::default_data_dir() == std::filesystem::path("/tmp/custom-proj-home"));
  if (old) setenv("PROJ_HOME", saved.c_str(), 1); else unsetenv("PROJ_HOME");
  CHECK_FALSE(paths::default_data_dir().empty());
}

TEST_CASE("glob matching follows gitignore wildcard rules", "[ignore]") {
  CHECK(glob_match("*.js", "app.js"));
  CHECK_FALSE(glob_match("*.js", "src/app.js"));
  CHECK(glob_match("src/*.js", "src/app.js"));
  CHECK(glob_match("**/test", "a/b/test"));
  CHECK(glob_match("**/test", "test"));
  CHECK(glob_match("a/**/b", "a/b"));
  CHECK(glob_match("a/**/b", "a/x/y/b"));
  CHECK(glob_match("logs/**", "logs/a/b.txt"));
  CHECK_FALSE(glob_match("logs/**", "logs"));
  CHECK(glob_match("file?.txt", "file1.txt"));
  CHECK_FALSE(glob_match("file?.txt", "file/.txt"));
  CHECK(glob_match("[abc].c", "b.c"));
  CHECK_FALSE(glob_match("[!abc].c", "b.c"));
  CHECK(glob_match("[a-c]x", "bx"));
  CHECK(glob_match("\\*literal", "*literal"));
  CHECK(glob_match("cmake-build-*", "cmake-build-debug"));
}

TEST_CASE("ignore rules: negation, directory-only and anchored patterns", "[ignore]") {
  IgnoreRules rules;
  rules.add_text("# comment\n\n*.log\n!keep.log\nbuild/\n/root-only.txt\ndocs/*.md\n", "", ".gitignore");
  CHECK(rules.match("debug.log", false));
  CHECK(rules.match("deep/debug.log", false));
  CHECK_FALSE(rules.match("keep.log", false));
  CHECK(rules.match("build", true));
  CHECK_FALSE(rules.match("build", false));  // a file named build is not a directory
  CHECK(rules.match("root-only.txt", false));
  CHECK_FALSE(rules.match("sub/root-only.txt", false));
  CHECK(rules.match("docs/a.md", false));
  CHECK_FALSE(rules.match("other/docs/a.md", false));
  CHECK_FALSE(rules.match("src/app.js", false));
}

TEST_CASE("nested ignore files only apply below their directory", "[ignore]") {
  IgnoreRules rules;
  rules.add_text("*.tmp\n", "pkg", "pkg/.gitignore");
  CHECK(rules.match("pkg/a.tmp", false));
  CHECK(rules.match("pkg/sub/a.tmp", false));
  CHECK_FALSE(rules.match("a.tmp", false));
  CHECK_FALSE(rules.match("pkgx/a.tmp", false));
  size_t before = rules.size();
  rules.add_pattern("*.x", "", "test");
  rules.truncate(before);
  CHECK_FALSE(rules.match("a.x", false));
}

TEST_CASE("languages are chosen by extension", "[language]") {
  CHECK(language_for_path("a/b.js") == Language::JavaScript);
  CHECK(language_for_path("a/b.mjs") == Language::JavaScript);
  CHECK(language_for_path("a/b.ts") == Language::TypeScript);
  CHECK(language_for_path("a/b.d.ts") == Language::TypeScript);
  CHECK(language_for_path("a/b.tsx") == Language::Tsx);
  CHECK(language_for_path("a/b.py") == Language::Python);
  CHECK(language_for_path("A.java") == Language::Java);
  CHECK(language_for_path("x.c") == Language::C);
  CHECK(language_for_path("x.h") == Language::Cpp);
  CHECK(language_for_path("x.hpp") == Language::Cpp);
  CHECK(language_for_path("x.CPP") == Language::Cpp);
  CHECK(language_for_path("README.md") == Language::None);
  CHECK(language_for_path("deploy.rb") == Language::None);
  CHECK(language_from_name("tsx") == Language::Tsx);
  CHECK_FALSE(language_from_name("ruby").has_value());
}

TEST_CASE("fingerprints change with content", "[hash]") {
  Fnv1a64 a, b, c;
  a.update("hello");
  b.update("hello");
  c.update("hellp");
  CHECK(a.hex() == b.hex());
  CHECK(a.hex() != c.hex());
  CHECK(a.hex().size() == 16);
}

namespace {
std::map<std::string, std::string> scan_statuses(const std::filesystem::path& root, ScanOptions opts = {}) {
  DiagnosticSink diags(100);
  Scanner scanner(root, opts, diags);
  std::map<std::string, std::string> out;
  scanner.scan([&](const ScanEntry& e) { out[e.rel_path] = e.status; });
  return out;
}
}  // namespace

TEST_CASE("scanner applies default exclusions and .gitignore on the express fixture", "[scanner]") {
  auto files = scan_statuses(proj::test::fixture("express-api"));
  CHECK(files.at("src/app.js") == "pending");
  CHECK(files.at("src/routes/orders.ts") == "pending");
  CHECK(files.at("scripts/deploy.rb") == "unsupported");
  CHECK(files.at("debug.log") == "excluded");  // *.log in .gitignore
  CHECK(files.count("generated/bundle.js") == 0);    // generated/ in .gitignore
  CHECK(files.count("dist/app.js") == 0);            // default exclusion
  CHECK(files.count("node_modules/express/index.js") == 0);
  // Deterministic ordering: a second scan visits the same files.
  CHECK(files == scan_statuses(proj::test::fixture("express-api")));
}

TEST_CASE("default exclusions are configurable", "[scanner]") {
  ScanOptions opts;
  opts.include_defaults = {"dist"};
  auto files = scan_statuses(proj::test::fixture("express-api"), opts);
  CHECK(files.at("dist/app.js") == "pending");
  ScanOptions no_git;
  no_git.use_gitignore = false;
  auto all = scan_statuses(proj::test::fixture("express-api"), no_git);
  CHECK(all.at("generated/bundle.js") == "pending");
  ScanOptions extra;
  extra.extra_excludes = {"src/cycle/"};
  auto fewer = scan_statuses(proj::test::fixture("express-api"), extra);
  CHECK(fewer.count("src/cycle/a.js") == 0);
}

TEST_CASE("ignore files of an enclosing git repository apply to a subdirectory project", "[scanner]") {
  TempDir tmp;
  auto repo = tmp.path() / "repo";
  std::filesystem::create_directories(repo / ".git" / "info");
  write_file(repo / ".gitignore", "backend/third_party/\n*.log\n");
  write_file(repo / ".git" / "info" / "exclude", "secret.py\n");
  write_file(repo / "backend" / ".gitignore", "!keep.log\n");
  write_file(repo / "backend" / "app.py", "x = 1\n");
  write_file(repo / "backend" / "debug.log", "x\n");
  write_file(repo / "backend" / "keep.log", "x\n");
  write_file(repo / "backend" / "secret.py", "x = 1\n");
  write_file(repo / "backend" / "third_party" / "lib.cpp", "int x;\n");
  auto files = scan_statuses(repo / "backend");
  CHECK(files.at("app.py") == "pending");
  CHECK(files.at("debug.log") == "excluded");        // *.log from the repository root
  CHECK(files.at("keep.log") == "unsupported");      // re-included by backend/.gitignore
  CHECK(files.at("secret.py") == "excluded");        // .git/info/exclude of the enclosing repository
  CHECK(files.count("third_party/lib.cpp") == 0);    // anchored root rule backend/third_party/
  ScanOptions no_git;
  no_git.use_gitignore = false;
  CHECK(scan_statuses(repo / "backend", no_git).at("third_party/lib.cpp") == "pending");
}

TEST_CASE(".projignore adds project-specific exclusions", "[scanner]") {
  TempDir tmp;
  write_file(tmp.path() / "keep.js", "1;\n");
  write_file(tmp.path() / "legacy/old.js", "1;\n");
  write_file(tmp.path() / "gen.pb.js", "1;\n");
  write_file(tmp.path() / ".projignore", "# project rules\nlegacy/\n*.pb.js\n");
  auto files = scan_statuses(tmp.path());
  CHECK(files.at("keep.js") == "pending");
  CHECK(files.count("legacy/old.js") == 0);
  CHECK(files.at("gen.pb.js") == "excluded");
}

TEST_CASE("scanner does not follow symlinks and reports size limits", "[scanner]") {
  TempDir tmp;
  auto root = tmp.path() / "project";
  write_file(root / "src/a.js", "function a() {}\n");
  write_file(root / "big.js", std::string(4096, 'x'));
  TempDir outside;
  write_file(outside.path() / "secret.js", "function secret() {}\n");
  std::filesystem::create_directory_symlink(outside.path(), root / "linked_dir");
  std::filesystem::create_symlink(outside.path() / "secret.js", root / "linked.js");

  ScanOptions opts;
  opts.max_file_size = 1024;
  DiagnosticSink diags(100);
  Scanner scanner(root, opts, diags);
  std::map<std::string, std::string> files;
  scanner.scan([&](const ScanEntry& e) { files[e.rel_path] = e.status; });
  CHECK(files.at("src/a.js") == "pending");
  CHECK(files.at("big.js") == "too_large");
  CHECK(files.at("linked.js") == "symlink");
  CHECK(files.count("linked_dir/secret.js") == 0);
  CHECK(scanner.stats().dirs_symlink == 1);
  bool reported = false;
  for (const auto& d : diags.kept()) reported |= d.code == "file_too_large" && d.path == "big.js";
  CHECK(reported);
}

TEST_CASE("unreadable directories are reported, not fatal", "[scanner]") {
  if (geteuid() == 0) SKIP("running as root: permissions are not enforced");
  TempDir tmp;
  auto root = tmp.path() / "project";
  write_file(root / "ok.js", "1;\n");
  write_file(root / "locked/inner.js", "1;\n");
  std::filesystem::permissions(root / "locked", std::filesystem::perms::none);
  DiagnosticSink diags(100);
  Scanner scanner(root, ScanOptions{}, diags);
  std::set<std::string> seen;
  scanner.scan([&](const ScanEntry& e) { seen.insert(e.rel_path); });
  std::filesystem::permissions(root / "locked", std::filesystem::perms::owner_all);
  CHECK(seen.count("ok.js") == 1);
  CHECK(seen.count("locked/inner.js") == 0);
  CHECK(scanner.stats().dirs_unreadable == 1);
}

TEST_CASE("file limit stops scanning and is reported", "[scanner]") {
  TempDir tmp;
  for (int i = 0; i < 10; ++i) write_file(tmp.path() / ("f" + std::to_string(i) + ".js"), "1;\n");
  ScanOptions opts;
  opts.max_files = 4;
  DiagnosticSink diags(100);
  Scanner scanner(tmp.path(), opts, diags);
  int count = 0;
  scanner.scan([&](const ScanEntry&) { ++count; });
  CHECK(count == 4);
  CHECK(scanner.stats().file_limit_reached);
}

TEST_CASE("diagnostic sink keeps memory bounded but counts everything", "[registry]") {
  DiagnosticSink sink(8);
  for (int i = 0; i < 100; ++i) sink.add({"debug", "config", "x", "", 0, "minor"});
  for (int i = 0; i < 20; ++i) sink.add({"warn", "source", "y", "", 0, "warning"});
  CHECK(sink.total() == 120);
  CHECK(sink.count_level("warn") == 20);
  CHECK(sink.kept().size() == 8 + 2);  // 8 warnings, 8/4 minor entries
  CHECK(sink.dropped() == 110);
}
