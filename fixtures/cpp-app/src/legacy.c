#include <stdio.h>

static int helper(int x) { return x * 2; }

int compute(int value) {
  return helper(value);
}
