// Tiny self-written unit-test framework (no downloads): TEST() registers a
// function, CHECK*() record failures and carry on, main() is in test_main.cpp.
#pragma once
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <sstream>
#include <string>
#include <vector>

namespace ast_test {

struct TestCase {
  const char* name;
  void (*fn)();
};
std::vector<TestCase>& registry();
extern int g_checks;
extern int g_failed_checks;
void fail(const char* file, int line, const std::string& msg);

struct Registrar {
  Registrar(const char* name, void (*fn)()) { registry().push_back(TestCase{name, fn}); }
};

template <typename T>
std::string show(const T& v) {
  std::ostringstream o;
  o << v;
  return o.str();
}
inline std::string show(unsigned char v) { return std::to_string(static_cast<unsigned>(v)); }
inline std::string show(signed char v) { return std::to_string(static_cast<int>(v)); }
inline std::string show(bool v) { return v ? "true" : "false"; }

}  // namespace ast_test

#define TEST(name)                                                   \
  static void name();                                                \
  static ast_test::Registrar registrar_##name(#name, name);         \
  static void name()

#define CHECK(cond)                                                  \
  do {                                                               \
    ++ast_test::g_checks;                                            \
    if (!(cond)) ast_test::fail(__FILE__, __LINE__, "CHECK(" #cond ")"); \
  } while (0)

#define CHECK_EQ(a, b)                                               \
  do {                                                               \
    ++ast_test::g_checks;                                            \
    const auto va_ = (a);                                            \
    const auto vb_ = (b);                                            \
    if (!(va_ == vb_))                                               \
      ast_test::fail(__FILE__, __LINE__, std::string("CHECK_EQ(" #a ", " #b ") got ") + \
                                             ast_test::show(va_) + " vs " + ast_test::show(vb_)); \
  } while (0)

#define CHECK_STR(a, b)                                              \
  do {                                                               \
    ++ast_test::g_checks;                                            \
    const std::string va_ = (a);                                     \
    const std::string vb_ = (b);                                     \
    if (va_ != vb_)                                                  \
      ast_test::fail(__FILE__, __LINE__, std::string("CHECK_STR(" #a ")\n     got: ") + va_ + "\n  wanted: " + vb_); \
  } while (0)

#define CHECK_NEAR(a, b, tol)                                        \
  do {                                                               \
    ++ast_test::g_checks;                                            \
    const double va_ = static_cast<double>(a);                       \
    const double vb_ = static_cast<double>(b);                       \
    if (!(fabs(va_ - vb_) <= (tol)))                                 \
      ast_test::fail(__FILE__, __LINE__, std::string("CHECK_NEAR(" #a ", " #b ") got ") + \
                                             std::to_string(va_) + " vs " + std::to_string(vb_)); \
  } while (0)
