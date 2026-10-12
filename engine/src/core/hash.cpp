#include "core/hash.hpp"

#include <cstdio>

namespace proj {

std::string Fnv1a64::hex() const {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(state_));
  return buf;
}

}  // namespace proj
