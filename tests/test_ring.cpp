// SPSC ring: single-thread semantics, wraparound, batch pop, and a
// cross-thread stress test that verifies strict FIFO order with no loss or
// duplication across millions of handoffs.
#include <cstdint>
#include <thread>

#include "harness.hpp"
#include "lob/spsc_ring.hpp"

using namespace lob;

TEST(ring_basic_fifo_and_full_empty) {
  SpscRing<std::uint64_t> r(8);
  std::uint64_t v = 0;
  CHECK(!r.try_pop(v));
  for (std::uint64_t i = 0; i < 8; ++i) CHECK(r.try_push(i));
  CHECK(!r.try_push(99));  // full at capacity
  for (std::uint64_t i = 0; i < 8; ++i) {
    CHECK(r.try_pop(v));
    CHECK_EQ(v, i);
  }
  CHECK(!r.try_pop(v));
}

TEST(ring_wraparound_many_times) {
  SpscRing<std::uint64_t> r(4);
  std::uint64_t v = 0;
  for (std::uint64_t i = 0; i < 1000; ++i) {
    CHECK(r.try_push(i));
    CHECK(r.try_pop(v));
    CHECK_EQ(v, i);
  }
  CHECK(r.empty());
}

TEST(ring_batch_pop) {
  SpscRing<std::uint64_t> r(16);
  for (std::uint64_t i = 0; i < 10; ++i) r.try_push(i);
  std::uint64_t out[16];
  const std::size_t n = r.try_pop_batch(out, 16);
  CHECK_EQ(n, std::size_t(10));
  for (std::uint64_t i = 0; i < 10; ++i) CHECK_EQ(out[i], i);
  CHECK_EQ(r.try_pop_batch(out, 16), std::size_t(0));
}

TEST(ring_cross_thread_fifo_integrity) {
  // Producer pushes 0..N-1; consumer must observe exactly that sequence.
  constexpr std::uint64_t N = 2'000'000;
  SpscRing<std::uint64_t> r(1024);
  std::uint64_t bad_order = 0;

  std::thread consumer([&] {
    std::uint64_t expect = 0;
    std::uint64_t batch[64];
    while (expect < N) {
      const std::size_t n = r.try_pop_batch(batch, 64);
      if (n == 0) {
        std::this_thread::yield();  // keeps this test honest on 1-core boxes
        continue;
      }
      for (std::size_t i = 0; i < n; ++i) {
        if (batch[i] != expect) ++bad_order;
        ++expect;
      }
    }
  });

  for (std::uint64_t i = 0; i < N; ++i) {
    while (!r.try_push(i)) std::this_thread::yield();
  }
  consumer.join();
  CHECK_EQ(bad_order, std::uint64_t(0));
  CHECK(r.empty());
}
