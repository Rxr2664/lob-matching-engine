// Test runner. Also replaces global operator new/delete with counting
// versions so tests/test_no_alloc.cpp can PROVE the hot path performs zero
// heap allocations (rather than just claiming it in a comment).
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>
#if defined(_WIN32)
#include <malloc.h>
#endif

#include "harness.hpp"

std::atomic<unsigned long long> g_alloc_count{0};
int g_failures = 0;

std::vector<TestCase>& test_registry() {
  static std::vector<TestCase> r;
  return r;
}

// ---- counting global allocator ---------------------------------------------
// Plain and aligned variants both counted; each new pairs with a matching
// delete so the whole binary uses a consistent malloc/free discipline.
// MSVCRT/UCRT (MinGW's C runtime) has no std::aligned_alloc; _aligned_malloc/
// _aligned_free is the Windows equivalent, and unlike malloc/free the two
// aligned/unaligned families are not interchangeable, so they stay paired
// per platform rather than both funneling through std::free.
namespace {
void* aligned_alloc_compat(std::size_t align, std::size_t size) {
#if defined(_WIN32)
  return _aligned_malloc(size, align);
#else
  return std::aligned_alloc(align, size);
#endif
}
void aligned_free_compat(void* p) noexcept {
#if defined(_WIN32)
  _aligned_free(p);
#else
  std::free(p);
#endif
}
}  // namespace

void* operator new(std::size_t n) {
  g_alloc_count.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void* operator new(std::size_t n, std::align_val_t al) {
  g_alloc_count.fetch_add(1, std::memory_order_relaxed);
  const std::size_t a = static_cast<std::size_t>(al);
  const std::size_t sz = (n + a - 1) / a * a;
  if (void* p = aligned_alloc_compat(a, sz ? sz : a)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n, std::align_val_t al) {
  return ::operator new(n, al);
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { aligned_free_compat(p); }
void operator delete[](void* p, std::align_val_t) noexcept { aligned_free_compat(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { aligned_free_compat(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { aligned_free_compat(p); }

int main() {
  int ran = 0;
  for (const TestCase& t : test_registry()) {
    std::printf("[ RUN ] %s\n", t.name);
    const int before = g_failures;
    t.fn();
    std::printf("[ %s ] %s\n", g_failures == before ? " OK " : "FAIL", t.name);
    ++ran;
  }
  std::printf("\n%d test(s) ran, %d failure(s)\n", ran, g_failures);
  return g_failures == 0 ? 0 : 1;
}
