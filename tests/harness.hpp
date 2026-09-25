#pragma once
// Minimal zero-dependency test harness: TEST(name) registers a case,
// CHECK/CHECK_EQ record failures with file:line, main() in test_main.cpp
// runs everything and returns non-zero if anything failed.
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

struct TestCase {
  const char* name;
  void (*fn)();
};

std::vector<TestCase>& test_registry();
extern int g_failures;

struct TestRegistrar {
  TestRegistrar(const char* name, void (*fn)()) {
    test_registry().push_back({name, fn});
  }
};

#define TEST(name)                                    \
  static void test_fn_##name();                       \
  static TestRegistrar reg_##name(#name, &test_fn_##name); \
  static void test_fn_##name()

#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      ++g_failures;                                                     \
      std::printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
    }                                                                   \
  } while (0)

template <typename A, typename B>
void check_eq_impl(const A& a, const B& b, const char* sa, const char* sb,
                   const char* file, int line) {
  if (!(a == b)) {
    ++g_failures;
    std::ostringstream os;
    os << "    FAIL " << file << ":" << line << ": " << sa << " == " << sb
       << "  (" << a << " vs " << b << ")";
    std::printf("%s\n", os.str().c_str());
  }
}

#define CHECK_EQ(a, b) check_eq_impl((a), (b), #a, #b, __FILE__, __LINE__)
