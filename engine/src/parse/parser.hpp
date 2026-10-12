#pragma once

#include <string>
#include <string_view>

#include "parse/facts.hpp"
#include "scan/language.hpp"

struct TSParser;

namespace proj {

struct ParseOutcome {
  // parsed: no syntax errors; syntax_errors: tree contains ERROR/MISSING
  // nodes (facts are best-effort); parse_failed: no usable tree or the
  // extractor failed.
  std::string status;
  FileFacts facts;
  int error_count = 0;
  uint32_t first_error_line = 0;
  uint32_t line_count = 0;
  std::string message;
};

// Owns a single reusable Tree-sitter parser. Each call parses one file,
// extracts facts and deletes the syntax tree before returning.
class ParserManager {
 public:
  explicit ParserManager(double timeout_ms = 10000.0);
  ~ParserManager();
  ParserManager(const ParserManager&) = delete;
  ParserManager& operator=(const ParserManager&) = delete;

  ParseOutcome parse(Language lang, std::string_view rel_path, std::string_view source);

  // Development aid: S-expression of the syntax tree (proj-engine --dump-ast).
  std::string dump(Language lang, std::string_view source);

 private:
  TSParser* parser_;
  double timeout_ms_;
};

}  // namespace proj
