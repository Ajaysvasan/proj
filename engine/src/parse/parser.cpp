#include "parse/parser.hpp"

#include <tree_sitter/api.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <stdexcept>

#include "core/paths.hpp"
#include "parse/extractors.hpp"
#include "parse/ts_util.hpp"

extern "C" {
const TSLanguage* tree_sitter_javascript();
const TSLanguage* tree_sitter_typescript();
const TSLanguage* tree_sitter_tsx();
const TSLanguage* tree_sitter_python();
const TSLanguage* tree_sitter_java();
const TSLanguage* tree_sitter_c();
const TSLanguage* tree_sitter_cpp();
}

namespace proj {
namespace {

const TSLanguage* grammar_for(Language lang) {
  switch (lang) {
    case Language::JavaScript: return tree_sitter_javascript();
    case Language::TypeScript: return tree_sitter_typescript();
    case Language::Tsx: return tree_sitter_tsx();
    case Language::Python: return tree_sitter_python();
    case Language::Java: return tree_sitter_java();
    case Language::C: return tree_sitter_c();
    case Language::Cpp: return tree_sitter_cpp();
    case Language::None: break;
  }
  return nullptr;
}

ExtractFn extractor_for(Language lang) {
  switch (lang) {
    case Language::JavaScript:
    case Language::TypeScript:
    case Language::Tsx: return extract_javascript;
    case Language::Python: return extract_python;
    case Language::Java: return extract_java;
    case Language::C:
    case Language::Cpp: return extract_cpp;
    case Language::None: break;
  }
  return nullptr;
}

struct Deadline {
  std::chrono::steady_clock::time_point until;
};

bool progress_callback(TSParseState* state) {
  auto* d = static_cast<Deadline*>(state->payload);
  return std::chrono::steady_clock::now() > d->until;  // true cancels the parse
}

struct TreeDeleter {
  void operator()(TSTree* t) const { ts_tree_delete(t); }
};

void count_errors(TSNode root, int& count, uint32_t& first_line) {
  constexpr int kMaxCounted = 1000;
  ts::walk(
      root,
      [&](TSNode n) {
        if (count >= kMaxCounted || !ts_node_has_error(n)) return false;
        auto record = [&](TSNode e) {
          uint32_t line = ts_node_start_point(e).row + 1;
          if (count == 0 || line < first_line) first_line = line;
          ++count;
        };
        if (ts_node_is_error(n) || ts_node_is_missing(n)) {
          record(n);
          return false;
        }
        // MISSING tokens are usually anonymous, which the walk does not visit.
        uint32_t children = ts_node_child_count(n);
        for (uint32_t i = 0; i < children && count < kMaxCounted; ++i) {
          TSNode c = ts_node_child(n, i);
          if (!ts_node_is_named(c) && ts_node_is_missing(c)) record(c);
        }
        return true;
      },
      [](TSNode) {});
  // Defensive fallback: the root reports an error the walk could not attribute.
  if (count == 0 && ts_node_has_error(root)) {
    count = 1;
    first_line = ts_node_start_point(root).row + 1;
  }
}

uint32_t count_lines(std::string_view s) {
  if (s.empty()) return 0;
  auto n = static_cast<uint32_t>(std::count(s.begin(), s.end(), '\n'));
  return s.back() == '\n' ? n : n + 1;
}

}  // namespace

int extractor_version(Language lang) {
  switch (lang) {
    case Language::JavaScript:
    case Language::TypeScript:
    case Language::Tsx: return 3;
    case Language::Python: return 3;
    case Language::Java: return 3;
    case Language::C:
    case Language::Cpp: return 3;
    case Language::None: break;
  }
  return 0;
}

void add_module_symbol(const ExtractInput& in, FileFacts& out) {
  SymbolFact m;
  m.name = paths::stem(in.rel_path);
  m.qualified_name = std::string(in.rel_path);
  m.kind = "module";
  m.span = ts::span(in.root);
  m.span.start = {1, 1};
  out.symbols.push_back(std::move(m));
}

ParserManager::ParserManager(double timeout_ms) : parser_(ts_parser_new()), timeout_ms_(timeout_ms) {
  if (!parser_) throw std::runtime_error("failed to create a Tree-sitter parser");
}

ParserManager::~ParserManager() { ts_parser_delete(parser_); }

ParseOutcome ParserManager::parse(Language lang, std::string_view rel_path, std::string_view source) {
  ParseOutcome out;
  out.line_count = count_lines(source);
  const TSLanguage* grammar = grammar_for(lang);
  ExtractFn extract = extractor_for(lang);
  if (!grammar || !extract) {
    out.status = "parse_failed";
    out.message = "no parser is available for this language";
    return out;
  }
  if (!ts_parser_set_language(parser_, grammar)) {
    out.status = "parse_failed";
    out.message = "grammar ABI version is incompatible with the Tree-sitter runtime";
    return out;
  }
  ts_parser_reset(parser_);

  Deadline deadline{std::chrono::steady_clock::now() +
                    std::chrono::microseconds(static_cast<int64_t>(timeout_ms_ * 1000.0))};
  TSParseOptions options{&deadline, progress_callback};
  TSInput input{};
  struct Payload {
    std::string_view src;
  } payload{source};
  input.payload = &payload;
  input.encoding = TSInputEncodingUTF8;
  input.read = [](void* p, uint32_t byte, TSPoint, uint32_t* bytes_read) -> const char* {
    auto* pl = static_cast<Payload*>(p);
    if (byte >= pl->src.size()) {
      *bytes_read = 0;
      return "";
    }
    *bytes_read = static_cast<uint32_t>(pl->src.size() - byte);
    return pl->src.data() + byte;
  };
  std::unique_ptr<TSTree, TreeDeleter> tree(ts_parser_parse_with_options(parser_, nullptr, input, options));
  if (!tree) {
    ts_parser_reset(parser_);
    out.status = "parse_failed";
    out.message = "parser stopped after the " + std::to_string(static_cast<int64_t>(timeout_ms_)) +
                  " ms time limit";
    return out;
  }

  TSNode root = ts_tree_root_node(tree.get());
  if (ts_node_has_error(root)) count_errors(root, out.error_count, out.first_error_line);

  ExtractInput in{root, source, rel_path, lang};
  try {
    extract(in, out.facts);
  } catch (const std::exception& e) {
    out.facts = FileFacts{};
    out.status = "parse_failed";
    out.message = std::string("extractor failed: ") + e.what();
    return out;
  }
  out.status = out.error_count > 0 ? "syntax_errors" : "parsed";
  return out;
}

std::string ParserManager::dump(Language lang, std::string_view source) {
  const TSLanguage* grammar = grammar_for(lang);
  if (!grammar || !ts_parser_set_language(parser_, grammar)) return "";
  std::unique_ptr<TSTree, TreeDeleter> tree(
      ts_parser_parse_string(parser_, nullptr, source.data(), static_cast<uint32_t>(source.size())));
  if (!tree) return "";
  std::unique_ptr<char, decltype(&std::free)> s(ts_node_string(ts_tree_root_node(tree.get())), &std::free);
  return std::string(s.get());
}

}  // namespace proj
