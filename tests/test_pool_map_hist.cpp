// FixedPool and FlatMap unit tests. The FlatMap stress test is
// differential: 200k random insert/find/erase ops checked against
// std::unordered_map, which exercises the backward-shift deletion path hard.
#include <cstdint>
#include <unordered_map>

#include "harness.hpp"
#include "lob/flat_map.hpp"
#include "lob/order_book.hpp"
#include "lob/pool.hpp"

using namespace lob;

TEST(pool_alloc_exhaust_release) {
  FixedPool<Order> p(4);
  Order* a[4];
  for (int i = 0; i < 4; ++i) {
    a[i] = p.alloc();
    CHECK(a[i] != nullptr);
  }
  CHECK_EQ(p.in_use(), std::size_t(4));
  CHECK(p.alloc() == nullptr);  // exhausted, no heap fallback
  p.release(a[2]);
  Order* b = p.alloc();
  CHECK_EQ(b, a[2]);  // LIFO reuse for cache warmth
  for (int i = 0; i < 4; ++i) p.release(a[i]);
  CHECK_EQ(p.in_use(), std::size_t(0));
}

TEST(flatmap_basic) {
  FlatMap<int> m(8);
  CHECK(m.insert(10, 1));
  CHECK(m.insert(20, 2));
  CHECK(!m.insert(10, 3));  // duplicate refused
  CHECK_EQ(*m.find(10), 1);
  CHECK_EQ(*m.find(20), 2);
  CHECK(m.find(30) == nullptr);
  CHECK(m.erase(10));
  CHECK(!m.erase(10));
  CHECK(m.find(10) == nullptr);
  CHECK_EQ(*m.find(20), 2);
  CHECK_EQ(m.size(), std::size_t(1));
}

TEST(flatmap_capacity_limit) {
  FlatMap<int> m(4);
  for (std::uint64_t k = 1; k <= 4; ++k) CHECK(m.insert(k, int(k)));
  CHECK(!m.insert(5, 5));  // at capacity
  CHECK(m.erase(2));
  CHECK(m.insert(5, 5));   // room again
}

TEST(flatmap_differential_stress_vs_unordered_map) {
  FlatMap<std::uint64_t> m(4096);
  std::unordered_map<std::uint64_t, std::uint64_t> ref;
  std::uint64_t rng = 0x123456789abcdefull;
  auto rnd = [&] {
    rng ^= rng >> 12; rng ^= rng << 25; rng ^= rng >> 27;
    return rng * 0x2545F4914F6CDD1Dull;
  };
  std::uint64_t mismatches = 0;
  for (int i = 0; i < 200000; ++i) {
    const std::uint64_t key = 1 + (rnd() % 3000);  // heavy key reuse
    switch (rnd() % 3) {
      case 0: {
        const std::uint64_t val = rnd();
        const bool a = m.insert(key, val);
        const bool b = ref.emplace(key, val).second;
        if (a != b) ++mismatches;
        break;
      }
      case 1: {
        const std::uint64_t* p = m.find(key);
        auto it = ref.find(key);
        if ((p == nullptr) != (it == ref.end())) ++mismatches;
        else if (p && *p != it->second) ++mismatches;
        break;
      }
      case 2: {
        const bool a = m.erase(key);
        const bool b = ref.erase(key) > 0;
        if (a != b) ++mismatches;
        break;
      }
    }
    if (m.size() != ref.size()) ++mismatches;
  }
  CHECK_EQ(mismatches, std::uint64_t(0));
  // final full sweep
  for (const auto& [k, v] : ref) {
    const std::uint64_t* p = m.find(k);
    CHECK(p != nullptr && *p == v);
  }
}
