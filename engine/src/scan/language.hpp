#pragma once

#include <optional>
#include <string_view>

namespace proj {

enum class Language { None, JavaScript, TypeScript, Tsx, Python, Java, C, Cpp, Xml, Yaml };

// Chooses a language from the file extension. Returns Language::None for
// files without a supported parser.
Language language_for_path(std::string_view rel_path);

// Stable lower-case identifier stored in the index ("javascript", "tsx", ...).
const char* language_name(Language lang);

std::optional<Language> language_from_name(std::string_view name);

// Configuration languages (XML, YAML) are parsed for routes, handler classes
// and framework wiring. Their syntax errors and size limits never make an
// analysis partial: many repositories contain templated or data files in them.
bool is_config_language(Language lang);
bool is_config_language_name(std::string_view name);

}  // namespace proj
