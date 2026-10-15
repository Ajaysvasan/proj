#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// Process utilities for the dynamic service (Linux and macOS).
namespace proj::dynamic::platform {

bool process_alive(int64_t pid);
// Owned by the user running this process (the only processes the service may attach to).
bool owned_by_me(int64_t pid);
// Command line with arguments separated by spaces, or nullopt if unreadable.
std::optional<std::string> command_line(int64_t pid);
// JVMs of this user that publish performance data (hsperfdata_<user>; absent with -XX:-UsePerfData).
std::vector<int64_t> user_jvms();

struct Child {
  int pid = -1;
  int out_fd = -1;  // stdout and stderr, non-blocking, close-on-exec
};
// Starts argv[0] (looked up on PATH) with stdin from /dev/null.
std::optional<Child> spawn_capture(const std::vector<std::string>& argv, std::string* error);

// Commit of a Git checkout (HEAD resolved through refs), or "" when not a Git checkout.
std::string git_revision(const std::filesystem::path& root);

}  // namespace proj::dynamic::platform
