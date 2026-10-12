#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// Path helpers. Index paths are stored project-relative with '/' separators so
// an index is independent of the host path syntax.
namespace proj::paths {

namespace fs = std::filesystem;

// $PROJ_HOME if set, otherwise the platform user-data directory:
//   Linux:  $XDG_DATA_HOME/proj, or ~/.local/share/proj
//   macOS:  ~/Library/Application Support/proj
// Throws std::runtime_error if no home directory can be determined.
fs::path default_data_dir();

// Lexically normalizes a '/'-separated relative path ("a/./b/../c" -> "a/c").
// Returns nullopt for absolute paths or paths that escape upward.
std::optional<std::string> normalize_relative(std::string_view rel);

// Joins a project-relative directory with a relative reference and
// normalizes the result. Returns nullopt if the result leaves the root.
std::optional<std::string> join_relative(std::string_view dir, std::string_view rel);

// "a/b/c.js" -> "a/b"; "c.js" -> "".
std::string parent_dir(std::string_view rel);
// "a/b/c.js" -> "c.js".
std::string base_name(std::string_view rel);
// Lower-cased final extension including the dot ("x.D.TS" -> ".ts"), or "".
std::string extension(std::string_view name);
// File name without its final extension ("c.test.js" -> "c.test").
std::string stem(std::string_view name);

// If `path` lies inside `root` (both should already be canonical), returns the
// '/'-separated relative path ("" for the root itself). Otherwise nullopt.
std::optional<std::string> relative_to_root(const fs::path& root, const fs::path& path);

}  // namespace proj::paths
