#include "scan/language.hpp"

#include <array>
#include <string>
#include <utility>

#include "core/paths.hpp"

namespace proj {
namespace {

constexpr std::array<std::pair<const char*, Language>, 24> kExtensions = {{
    {".js", Language::JavaScript},  {".mjs", Language::JavaScript}, {".cjs", Language::JavaScript},
    {".jsx", Language::JavaScript}, {".ts", Language::TypeScript},  {".mts", Language::TypeScript},
    {".cts", Language::TypeScript}, {".tsx", Language::Tsx},        {".py", Language::Python},
    {".pyi", Language::Python},     {".java", Language::Java},      {".c", Language::C},
    // Headers are parsed with the C++ grammar, which accepts most C headers.
    {".h", Language::Cpp},          {".cc", Language::Cpp},         {".cpp", Language::Cpp},
    {".cxx", Language::Cpp},        {".c++", Language::Cpp},        {".hpp", Language::Cpp},
    {".hh", Language::Cpp},         {".hxx", Language::Cpp},        {".h++", Language::Cpp},
    {".ipp", Language::Cpp},        {".inl", Language::Cpp},        {".tpp", Language::Cpp},
}};

}  // namespace

Language language_for_path(std::string_view rel_path) {
  std::string ext = paths::extension(rel_path);
  if (ext.empty()) return Language::None;
  for (const auto& [e, lang] : kExtensions) {
    if (ext == e) return lang;
  }
  return Language::None;
}

const char* language_name(Language lang) {
  switch (lang) {
    case Language::JavaScript: return "javascript";
    case Language::TypeScript: return "typescript";
    case Language::Tsx: return "tsx";
    case Language::Python: return "python";
    case Language::Java: return "java";
    case Language::C: return "c";
    case Language::Cpp: return "cpp";
    case Language::None: break;
  }
  return "none";
}

std::optional<Language> language_from_name(std::string_view name) {
  for (Language l : {Language::JavaScript, Language::TypeScript, Language::Tsx, Language::Python,
                     Language::Java, Language::C, Language::Cpp}) {
    if (name == language_name(l)) return l;
  }
  return std::nullopt;
}

}  // namespace proj
