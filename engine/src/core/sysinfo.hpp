#pragma once

#include <cstdint>
#include <optional>

namespace proj {

// Peak resident set size of the current process in KiB, if the platform
// reports it. Returns nullopt rather than guessing.
std::optional<int64_t> peak_rss_kib();

// True if a process with this id currently exists (POSIX kill(pid, 0)).
bool process_alive(int64_t pid);

int64_t current_pid();

}  // namespace proj
