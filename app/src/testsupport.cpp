#include "testsupport.h"

#include <cstdio>

int& TestFailures() {
  static int failed = 0;
  return failed;
}

void Expect(bool cond, const char* name) {
  if (cond) {
    std::printf("PASS %s\n", name);
  } else {
    std::printf("FAIL %s\n", name);
    ++TestFailures();
  }
}
