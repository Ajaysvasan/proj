#pragma once

#include <tree_sitter/api.h>

#include <string_view>

#include "parse/facts.hpp"
#include "scan/language.hpp"

// Language extractors. Each one walks a syntax tree once and fills the shared
// FileFacts representation, keeping language-specific rules local to its own
// file. They never retain the tree.
namespace proj {

struct ExtractInput {
  TSNode root;
  std::string_view source;
  std::string_view rel_path;
  Language language;
};

using ExtractFn = void (*)(const ExtractInput&, FileFacts&);

void extract_javascript(const ExtractInput& in, FileFacts& out);  // JavaScript, TypeScript, TSX
void extract_python(const ExtractInput& in, FileFacts& out);
void extract_java(const ExtractInput& in, FileFacts& out);
void extract_cpp(const ExtractInput& in, FileFacts& out);         // C and C++

// Bumped whenever an extractor's output changes, so a future incremental
// refresh can tell which files must be re-extracted.
int extractor_version(Language lang);

// Creates the module symbol (symbols[0]) that owns top-level code.
void add_module_symbol(const ExtractInput& in, FileFacts& out);

}  // namespace proj
