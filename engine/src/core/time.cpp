#include "core/time.hpp"

#include <ctime>
#include <cstdio>

namespace proj {

std::string iso_now() {
  using namespace std::chrono;
  auto now = system_clock::now();
  auto secs = time_point_cast<seconds>(now);
  auto ms = duration_cast<milliseconds>(now - secs).count();
  std::time_t t = system_clock::to_time_t(secs);
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900,
                tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms));
  return buf;
}

}  // namespace proj
