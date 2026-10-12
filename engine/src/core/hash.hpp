#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace proj {

// Streaming 64-bit FNV-1a. Used as a content fingerprint for change detection
// (together with the file size), not for any security purpose.
class Fnv1a64 {
 public:
  void update(std::string_view data) {
    for (unsigned char c : data) {
      state_ ^= c;
      state_ *= 0x100000001b3ULL;
    }
  }
  uint64_t value() const { return state_; }
  std::string hex() const;

 private:
  uint64_t state_ = 0xcbf29ce484222325ULL;
};

}  // namespace proj
