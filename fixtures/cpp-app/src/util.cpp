#include "util.h"

#include <cstdio>

void print_user(const store::User& user) {
  std::printf("%d %s\n", user.id, user.name.c_str());
}

void log_error(const char* message) {
  std::fprintf(stderr, "error: %s\n", message);
}
