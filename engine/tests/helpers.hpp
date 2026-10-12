#pragma once

#include <catch_amalgamated.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

#include "core/json.hpp"
#include "parse/facts.hpp"
#include "parse/parser.hpp"
#include "protocol/protocol.hpp"
#include "scan/language.hpp"

namespace proj::test {

namespace fs = std::filesystem;

// A private temporary directory removed on destruction. It only ever removes
// the directory it created itself (under the system temp directory).
class TempDir {
 public:
  TempDir() {
    static std::atomic<int> counter{0};
    path_ = fs::temp_directory_path() /
            ("proj-engine-test-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
    fs::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ec;
    if (path_.filename().string().rfind("proj-engine-test-", 0) == 0) {
      // Restore permissions changed by tests so cleanup can proceed.
      for (auto it = fs::recursive_directory_iterator(path_, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        fs::permissions(it->path(), fs::perms::owner_all, fs::perm_options::add, ec);
      }
      fs::remove_all(path_, ec);
    }
  }
  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

inline fs::path fixture(const std::string& name) { return fs::path(PROJ_FIXTURES_DIR) / name; }

inline void write_file(const fs::path& p, const std::string& content) {
  fs::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary);
  out << content;
}

inline ParseOutcome parse(Language lang, const std::string& code, const std::string& path = "src/x") {
  ParserManager pm;
  return pm.parse(lang, path, code);
}

inline json call(protocol::Engine& engine, const std::string& op, json args, const fs::path& data_dir) {
  json req = {{"protocol", 1}, {"id", 1}, {"op", op}, {"args", std::move(args)},
              {"options", {{"data_dir", data_dir.string()}, {"log_level", "error"}}}};
  return engine.handle(req);
}

// Finds the first symbol with this qualified name, or nullptr.
inline const SymbolFact* find_symbol(const FileFacts& f, const std::string& qualified) {
  for (const auto& s : f.symbols) {
    if (s.qualified_name == qualified) return &s;
  }
  return nullptr;
}

inline const CallFact* find_call(const FileFacts& f, const std::string& name) {
  for (const auto& c : f.calls) {
    if (c.name == name) return &c;
  }
  return nullptr;
}

}  // namespace proj::test
