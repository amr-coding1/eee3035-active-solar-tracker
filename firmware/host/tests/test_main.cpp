#include "test_framework.h"

namespace ast_test {
std::vector<TestCase>& registry() {
  static std::vector<TestCase> r;
  return r;
}
int g_checks = 0;
int g_failed_checks = 0;
static bool g_current_failed = false;

void fail(const char* file, int line, const std::string& msg) {
  ++g_failed_checks;
  g_current_failed = true;
  printf("    FAIL %s:%d: %s\n", file, line, msg.c_str());
}
}  // namespace ast_test

// Usage: ast_tests [-v] [name-filter]
int main(int argc, char** argv) {
  using namespace ast_test;
  bool verbose = false;
  const char* filter = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "-v") == 0) verbose = true;
    else filter = argv[i];
  }
  int run = 0, failed = 0;
  for (const TestCase& t : registry()) {
    if (filter != nullptr && strstr(t.name, filter) == nullptr) continue;
    g_current_failed = false;
    ++run;
    t.fn();
    if (g_current_failed) {
      ++failed;
      printf("[FAIL] %s\n", t.name);
    } else if (verbose) {
      printf("[ ok ] %s\n", t.name);
    }
  }
  printf("%d tests, %d checks, %d tests failed (%d checks failed)\n", run, g_checks, failed, g_failed_checks);
  return failed == 0 && run > 0 ? 0 : 1;
}
