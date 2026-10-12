#pragma once

#include <chrono>
#include <string>

namespace proj {

// UTC timestamp in ISO-8601 form with millisecond precision.
std::string iso_now();

// Measures elapsed time with a monotonic clock.
class Stopwatch {
 public:
  Stopwatch() : start_(std::chrono::steady_clock::now()) {}
  double elapsed_ms() const {
    auto d = std::chrono::steady_clock::now() - start_;
    return std::chrono::duration<double, std::milli>(d).count();
  }

 private:
  std::chrono::steady_clock::time_point start_;
};

}  // namespace proj
