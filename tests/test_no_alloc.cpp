// Allocation-free hot path: PROVEN, not asserted.
//
// test_main.cpp replaces global operator new/new[] (plain + aligned) with
// counting versions. Here we pre-generate 200k messages of realistic mixed
// flow, snapshot the global allocation counter, push all of them through
// engine.process(), and require the counter delta to be EXACTLY zero.
//
// Any accidental std::map node, vector growth, rehash, or string in the
// matching path turns this test red immediately.
#include <atomic>
#include <vector>

#include "harness.hpp"
#include "lob/feed.hpp"
#include "lob/matching_engine.hpp"

using namespace lob;

extern std::atomic<unsigned long long> g_alloc_count;

namespace {
struct DiscardSink {
  std::uint64_t seen = 0;
  void operator()(const OutboundEvent&) { ++seen; }  // no storage, no allocs
};
}  // namespace

TEST(hot_path_performs_zero_heap_allocations) {
  constexpr std::uint64_t kMsgs = 200000;

  EngineConfig cfg;
  cfg.min_price = 1;
  cfg.max_price = 5000;
  cfg.max_open_orders = 1u << 16;
  cfg.publish_bbo = true;    // include the market-data path in the proof
  cfg.stamp_events = false;  // clock calls are irrelevant to allocation

  DiscardSink sink;
  MatchingEngine<DiscardSink> eng(cfg, sink);

  FeedConfig fcfg;
  fcfg.seed = 7;
  fcfg.min_price = 1;
  fcfg.max_price = 5000;
  fcfg.start_mid = 2500;
  fcfg.max_tracked = 1u << 16;
  FeedGenerator feed(fcfg);

  // Pre-generate the whole stream so generation cost/allocation (the vector
  // below) is outside the measured window.
  std::vector<InboundMsg> msgs;
  msgs.reserve(kMsgs);
  for (std::uint64_t i = 0; i < kMsgs; ++i) msgs.push_back(feed.next());

  const unsigned long long before = g_alloc_count.load();
  for (const InboundMsg& m : msgs) eng.process(m);
  const unsigned long long after = g_alloc_count.load();

  CHECK_EQ(after - before, 0ull);
  CHECK_EQ(eng.stats().msgs, kMsgs);
  CHECK(eng.stats().trades > 1000);       // the run actually crossed
  CHECK(eng.open_orders() > 100);         // and actually rested orders
  CHECK(sink.seen > kMsgs);               // and actually emitted events
}

TEST(feed_generator_is_allocation_free_after_warm_start) {
  FeedConfig fcfg;
  fcfg.seed = 11;
  fcfg.max_tracked = 1u << 12;
  FeedGenerator feed(fcfg);
  for (int i = 0; i < 1000; ++i) (void)feed.next();  // reach steady state

  const unsigned long long before = g_alloc_count.load();
  for (int i = 0; i < 100000; ++i) (void)feed.next();
  const unsigned long long after = g_alloc_count.load();
  CHECK_EQ(after - before, 0ull);
}
