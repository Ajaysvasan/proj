#pragma once

#include <optional>
#include <string_view>

namespace proj {

enum class Language { None, JavaScript, TypeScript, Tsx, Python, Java, C, Cpp };

// Chooses a language from the file extension. Returns Language::None for
// files without a supported parser.
Language language_for_path(std::string_view rel_path);

// Stable lower-case identifier stored in the index ("javascript", "tsx", ...).
const char* language_name(Language lang);

std::optional<Language> language_from_name(std::string_view name);

}  // namespace proj
