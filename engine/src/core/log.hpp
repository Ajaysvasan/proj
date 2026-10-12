#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// Structured engine logging.
//
// Diagnostics go to stderr (never stdout, which carries protocol JSON) as
//   <timestamp> <LEVEL> [<component>] <message>
// INFO and above are additionally appended as JSON lines to a size-rotated
// application log file when one is configured.
namespace proj::log {

enum class Level { Debug = 0, Info = 1, Warn = 2, Error = 3 };

std::optional<Level> parse_level(std::string_view name);
const char* level_name(Level level);

// stderr_threshold: minimum level written to stderr.
// file: application log path (rotated at ~1 MiB, 3 backups), or nullopt.
void configure(Level stderr_threshold, std::optional<std::filesystem::path> file);

void write(Level level, std::string_view component, std::string_view message);

inline void debug(std::string_view c, std::string_view m) { write(Level::Debug, c, m); }
inline void info(std::string_view c, std::string_view m) { write(Level::Info, c, m); }
inline void warn(std::string_view c, std::string_view m) { write(Level::Warn, c, m); }
inline void error(std::string_view c, std::string_view m) { write(Level::Error, c, m); }

// Rotates `file` when it exceeds max_bytes: file -> file.1 -> ... -> file.<keep>.
void rotate_if_needed(const std::filesystem::path& file, std::uintmax_t max_bytes, int keep);

}  // namespace proj::log
