#include "core/sysinfo.hpp"

#include <cerrno>
#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>

namespace proj {

std::optional<int64_t> peak_rss_kib() {
  struct rusage usage {};
  if (getrusage(RUSAGE_SELF, &usage) != 0) return std::nullopt;
#if defined(__APPLE__)
  // macOS reports ru_maxrss in bytes.
  return static_cast<int64_t>(usage.ru_maxrss) / 1024;
#elif defined(__linux__)
  // Linux reports ru_maxrss in KiB.
  return static_cast<int64_t>(usage.ru_maxrss);
#else
  return std::nullopt;
#endif
}

bool process_alive(int64_t pid) {
  if (pid <= 0) return false;
  if (kill(static_cast<pid_t>(pid), 0) == 0) return true;
  return errno == EPERM;  // exists but owned by someone else
}

int64_t current_pid() { return static_cast<int64_t>(getpid()); }

}  // namespace proj
