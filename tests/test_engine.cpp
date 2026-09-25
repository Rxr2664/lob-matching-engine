// Scripted matching scenarios with exact expected event sequences.
// Every economically meaningful behaviour has a named test here; the
// randomized differential test in test_fuzz.cpp covers the combinatorics.
#include <vector>

#include "harness.hpp"
#include "lob/matching_engine.hpp"

using namespace lob;

namespace {

struct VecSink {
  std::vector<OutboundEvent> events;
  void operator()(const OutboundEvent& e) { events.push_back(e); }
};

using Engine = MatchingEngine<VecSink>;

EngineConfig test_cfg(std::size_t max_open = 1024, bool bbo = false) {
  EngineConfig c;
  c.min_price = 1;
  c.max_price = 1000;
  c.max_open_orders = max_open;
  c.publish_bbo = bbo;
  c.stamp_events = false;  // deterministic events
  return c;
}

InboundMsg mk_new(OrderId id, Side s, Price px, Qty q, Tif tif = Tif::GTC,
                  OrdType t = OrdType::Limit) {
  InboundMsg m{};
  m.type = MsgType::NewOrder;
  m.id = id;
  m.side = s;
  m.ord_type = t;
  m.tif = tif;
  m.price = px;
  m.qty = q;
  return m;
}

InboundMsg mk_cancel(OrderId id) {
  InboundMsg m{};
  m.type = MsgType::Cancel;
  m.id = id;
  return m;
}

InboundMsg mk_replace(OrderId id, Price px, Qty q) {
  InboundMsg m{};
  m.type = MsgType::Replace;
  m.id = id;
  m.price = px;
  m.qty = q;
  return m;
}

void expect(const OutboundEvent& e, EventType t, OrderId id, OrderId counter,
            Price px, Qty qty, Qty leaves,
            RejectReason r = RejectReason::None, std::uint8_t flags = 0) {
  CHECK_EQ(int(e.type), int(t));
  CHECK_EQ(e.id, id);
  CHECK_EQ(e.counter, counter);
  CHECK_EQ(e.price, px);
  CHECK_EQ(e.qty, qty);
  CHECK_EQ(e.leaves, leaves);
  CHECK_EQ(int(e.reason), int(r));
  CHECK_EQ(int(e.flags), int(flags));
}

}  // namespace

TEST(engine_price_priority_better_price_fills_first) {
  VecSink sink;
  Engine eng(test_cfg(), sink);
  eng.process(mk_new(1, Side::Sell, 105, 10));
  eng.process(mk_new(2, Side::Sell, 105, 5));
  eng.process(mk_new(3, Side::Sell, 104, 7));  // better ask, arrived later
  sink.events.clear();

  eng.process(mk_new(4, Side::Buy, 105, 15));
  const auto& ev = sink.events;
  CHECK_EQ(ev.size(), std::size_t(5));
  expect(ev[0], EventType::Accepted, 4, 0, 105, 15, 15);
  // best price first: 104 (id 3), then time priority at 105: id 1 before id 2
  expect(ev[1], EventType::Fill, 3, 4, 104, 7, 0);
  expect(ev[2], EventType::Fill, 4, 3, 104, 7, 8, RejectReason::None, kFlagTaker);
  expect(ev[3], EventType::Fill, 1, 4, 105, 8, 2);
  expect(ev[4], EventType::Fill, 4, 1, 105, 8, 0, RejectReason::None, kFlagTaker);

  // book state: 105 level holds id1(leaves 2) then id2(5), FIFO intact
  const PriceLevel* l = eng.book().find_level(Side::Sell, 105);
  CHECK(l != nullptr);
  CHECK_EQ(l->total, Qty(7));
  CHECK_EQ(l->count, 2u);
  CHECK_EQ(l->head->id, OrderId(1));
  CHECK_EQ(l->head->leaves, Qty(2));
  CHECK_EQ(l->tail->id, OrderId(2));
  CHECK_EQ(eng.book().best_ask_price(), Price(105));
}

TEST(engine_time_priority_fifo_within_level) {
  VecSink sink;
  Engine eng(test_cfg(), sink);
  eng.process(mk_new(1, Side::Sell, 105, 2));
  eng.process(mk_new(2, Side::Sell, 105, 5));
  sink.events.clear();
  eng.process(mk_new(3, Side::Buy, 105, 4));
  const auto& ev = sink.events;
  CHECK_EQ(ev.size(), std::size_t(5));
  expect(ev[1], EventType::Fill, 1, 3, 105, 2, 0);           // oldest first
  expect(ev[3], EventType::Fill, 2, 3, 105, 2, 3);           // then next in line
  expect(ev[4], EventType::Fill, 3, 2, 105, 2, 0, RejectReason::None, kFlagTaker);
}

TEST(engine_partial_fill_rests_remainder_at_limit) {
  VecSink sink;
  Engine eng(test_cfg(), sink);
  eng.process(mk_new(1, Side::Sell, 105, 4));
  sink.events.clear();
  eng.process(mk_new(2, Side::Buy, 106, 10));  // crosses, fills 4, rests 6 @106
  const auto& ev = sink.events;
  CHECK_EQ(ev.size(), std::size_t(3));
  expect(ev[0], EventType::Accepted, 2, 0, 106, 10, 10);
  expect(ev[1], EventType::Fill, 1, 2, 105, 4, 0);           // maker's price
  expect(ev[2], EventType::Fill, 2, 1, 105, 4, 6, RejectReason::None, kFlagTaker);
  CHECK_EQ(eng.book().best_bid_price(), Price(106));
  const PriceLevel* l = eng.book().find_level(Side::Buy, 106);
  CHECK(l && l->total == 6 && l->count == 1);
  CHECK_EQ(eng.open_orders(), std::size_t(1));
}

TEST(engine_ioc_unfilled_remainder_canceled) {
  VecSink sink;
  Engine eng(test_cfg(), sink);
  eng.process(mk_new(1, Side::Sell, 105, 3));
  sink.events.clear();
  eng.process(mk_new(2, Side::Buy, 105, 8, Tif::IOC));
  const auto& ev = sink.events;
  CHECK_EQ(ev.size(), std::size_t(4));
  expect(ev[0], EventType::Accepted, 2, 0, 105, 8, 8);
  expect(ev[1], EventType::Fill, 1, 2, 105, 3, 0);
  expect(ev[2], EventType::Fill, 2, 1, 105, 3, 5, RejectReason::None, kFlagTaker);
  expect(ev[3], EventType::Canceled, 2, 0, 105, 8, 5, RejectReason::NoLiquidity);
  CHECK_EQ(eng.open_orders(), std::size_t(0));  // IOC never rests
}

TEST(engine_ioc_no_cross_cancels_in_full) {
  VecSink sink;
  Engine eng(test_cfg(), sink);
  eng.process(mk_new(1, Side::Sell, 105, 3));
  sink.events.clear();
  eng.process(mk_new(2, Side::Buy, 104, 8, Tif::IOC));  // does not cross
  const auto& ev = sink.events;
  CHECK_EQ(ev.size(), std::size_t(2));
  expect(ev[0], EventType::Accepted, 2, 0, 104, 8, 8);
  expect(ev[1], EventType::Canceled, 2, 0, 104, 8, 8, RejectReason::NoLiquidity);
}

TEST(engine_fok_kills_without_touching_book_when_short) {
  VecSink sink;
  Engine eng(test_cfg(), sink);
  eng.process(mk_new(1, Side::Sell, 105, 3));
  eng.process(mk_new(2, Side::Sell, 106, 2));
  sink.events.clear();
  eng.process(mk_new(3, Side::Buy, 105, 4, Tif::FOK));  // only 3 available <=105
  const auto& ev = sink.events;
  CHECK_EQ(ev.size(), std::size_t(2));
  expect(ev[0], EventType::Accepted, 3, 0, 105, 4, 4);
  expect(ev[1], EventType::Canceled, 3, 0, 105, 4, 4, RejectReason::NoLiquidity);
  // book untouched -- not a single fill printed
  const PriceLevel* l = eng.book().find_level(Side::Sell, 105);
  CHECK(l && l->total == 3);
}

TEST(engine_fok_executes_fully_across_levels_when_available) {
  VecSink sink;
  Engine eng(test_cfg(), sink);
  eng.process(mk_new(1, Side::Sell, 105, 3));
  eng.process(mk_new(2, Side::Sell, 106, 2));
  sink.events.clear();
  eng.process(mk_new(3, Side::Buy, 106, 5, Tif::FOK));
  const auto& ev = sink.events;
  CHECK_EQ(ev.size(), std::size_t(5));
  expect(ev[1], EventType::Fill, 1, 3, 105, 3, 0);
  expect(ev[3], EventType::Fill, 2, 3, 106, 2, 0);
  expect(ev[4], EventType::Fill, 3, 2, 106, 2, 0, RejectReason::None, kFlagTaker);
  CHECK(eng.book().best_ask_price() == kNoPrice);
}

TEST(engine_market_order_walks_book_and_cancels_remainder) {
  VecSink sink;
  Engine eng(test_cfg(), sink);
  eng.process(mk_new(1, Side::Buy, 100, 3));
  eng.process(mk_new(2, Side::Buy, 99, 4));
  sink.events.clear();
  InboundMsg mkt = mk_new(3, Side::Sell, 0, 10, Tif::IOC, OrdType::Market);
  eng.process(mkt);
  const auto& ev = sink.events;
  CHECK_EQ(ev.size(), std::size_t(6));
  expect(ev[0], EventType::Accepted, 3, 0, 0, 10, 10);
  expect(ev[1], EventType::Fill, 1, 3, 100, 3, 0);  // best bid first
  expect(ev[3], EventType::Fill, 2, 3, 99, 4, 0);   // walks down
  expect(ev[5], EventType::Canceled, 3, 0, 0, 10, 3, RejectReason::NoLiquidity);
  CHECK(eng.book().best_bid_price() == kNoPrice);
}

TEST(engine_cancel_then_cancel_again_rejects) {
  VecSink sink;
  Engine eng(test_cfg(), sink);
  eng.process(mk_new(1, Side::Buy, 100, 5));
  sink.events.clear();
  eng.process(mk_cancel(1));
  eng.process(mk_cancel(1));
  const auto& ev = sink.events;
  CHECK_EQ(ev.size(), std::size_t(2));
  expect(ev[0], EventType::Canceled, 1, 0, 100, 5, 5);
  expect(ev[1], EventType::Rejected, 1, 0, 0, 0, 0, RejectReason::UnknownOrder);
  CHECK_EQ(eng.open_orders(), std::size_t(0));
  CHECK(eng.book().best_bid_price() == kNoPrice);
}

TEST(engine_validation_rejects) {
  VecSink sink;
  Engine eng(test_cfg(), sink);
  eng.process(mk_new(1, Side::Buy, 100, 0));            // zero qty
  eng.process(mk_new(2, Side::Buy, 5000, 5));           // out of band
  eng.process(mk_new(0, Side::Buy, 100, 5));            // id 0
  eng.process(mk_new(3, Side::Buy, 100, 5));            // ok, rests
  eng.process(mk_new(3, Side::Sell, 101, 5));           // duplicate id
  const auto& ev = sink.events;
  CHECK_EQ(ev.size(), std::size_t(5));
  expect(ev[0], EventType::Rejected, 1, 0, 100, 0, 0, RejectReason::BadQty);
  expect(ev[1], EventType::Rejected, 2, 0, 5000, 5, 0, RejectReason::BadPrice);
  expect(ev[2], EventType::Rejected, 0, 0, 100, 5, 0, RejectReason::BadMessage);
  expect(ev[3], EventType::Accepted, 3, 0, 100, 5, 5);
  expect(ev[4], EventType::Rejected, 3, 0, 101, 5, 0, RejectReason::DuplicateId);
}

TEST(engine_replace_size_down_same_price_keeps_priority) {
  VecSink sink;
  Engine eng(test_cfg(), sink);
  eng.process(mk_new(1, Side::Buy, 100, 10));
  eng.process(mk_new(2, Side::Buy, 100, 5));  // behind id 1
  sink.events.clear();
  eng.process(mk_replace(1, 100, 4));         // size down in place
  eng.process(mk_new(3, Side::Sell, 100, 6));
  const auto& ev = sink.events;
  CHECK_EQ(ev.size(), std::size_t(6));
  expect(ev[0], EventType::Replaced, 1, 0, 100, 4, 4);
  expect(ev[1], EventType::Accepted, 3, 0, 100, 6, 6);
  expect(ev[2], EventType::Fill, 1, 3, 100, 4, 0);  // id 1 kept its slot
  expect(ev[4], EventType::Fill, 2, 3, 100, 2, 3);  // then id 2
}

TEST(engine_replace_size_up_loses_priority) {
  VecSink sink;
  Engine eng(test_cfg(), sink);
  eng.process(mk_new(1, Side::Buy, 100, 10));
  eng.process(mk_new(2, Side::Buy, 100, 5));
  sink.events.clear();
  eng.process(mk_replace(1, 100, 15));  // size up: re-enters behind id 2
  eng.process(mk_new(3, Side::Sell, 100, 6));
  const auto& ev = sink.events;
  expect(ev[0], EventType::Replaced, 1, 0, 100, 15, 15);
  expect(ev[2], EventType::Fill, 2, 3, 100, 5, 0);   // id 2 now first
  expect(ev[4], EventType::Fill, 1, 3, 100, 1, 14);  // id 1 moved to the back
  const PriceLevel* l = eng.book().find_level(Side::Buy, 100);
  CHECK(l && l->head->id == 1 && l->head->leaves == 14);
}

TEST(engine_replace_price_change_can_trade_immediately) {
  VecSink sink;
  Engine eng(test_cfg(), sink);
  eng.process(mk_new(1, Side::Sell, 105, 4));
  eng.process(mk_new(2, Side::Buy, 100, 9));
  sink.events.clear();
  eng.process(mk_replace(2, 105, 9));  // reprice up: crosses the ask
  const auto& ev = sink.events;
  CHECK_EQ(ev.size(), std::size_t(3));
  expect(ev[0], EventType::Replaced, 2, 0, 105, 9, 9);
  expect(ev[1], EventType::Fill, 1, 2, 105, 4, 0);
  expect(ev[2], EventType::Fill, 2, 1, 105, 4, 5, RejectReason::None, kFlagTaker);
  const PriceLevel* l = eng.book().find_level(Side::Buy, 105);
  CHECK(l && l->head->id == 2 && l->head->leaves == 5);
  eng.process(mk_replace(99, 100, 1));  // unknown target
  expect(sink.events.back(), EventType::Rejected, 99, 0, 100, 1, 0,
         RejectReason::UnknownOrder);
}

TEST(engine_pool_exhaustion_cancels_instead_of_allocating) {
  VecSink sink;
  Engine eng(test_cfg(/*max_open=*/2), sink);
  eng.process(mk_new(1, Side::Buy, 100, 5));
  eng.process(mk_new(2, Side::Buy, 99, 5));
  sink.events.clear();
  eng.process(mk_new(3, Side::Buy, 98, 5));  // pool full: BOOK_FULL cut
  const auto& ev = sink.events;
  CHECK_EQ(ev.size(), std::size_t(2));
  expect(ev[0], EventType::Accepted, 3, 0, 98, 5, 5);
  expect(ev[1], EventType::Canceled, 3, 0, 98, 5, 5, RejectReason::BookFull);
  eng.process(mk_cancel(1));
  sink.events.clear();
  eng.process(mk_new(4, Side::Buy, 98, 5));  // capacity freed: rests fine
  CHECK_EQ(sink.events.size(), std::size_t(1));
  expect(sink.events[0], EventType::Accepted, 4, 0, 98, 5, 5);
  CHECK_EQ(eng.open_orders(), std::size_t(2));
}

TEST(engine_best_price_advances_across_gaps_after_depletion) {
  VecSink sink;
  Engine eng(test_cfg(), sink);
  // asks at 105 and 300 (a >64-tick gap crosses bitmap word boundaries)
  eng.process(mk_new(1, Side::Sell, 105, 3));
  eng.process(mk_new(2, Side::Sell, 300, 3));
  eng.process(mk_new(3, Side::Buy, 105, 3));  // depletes 105
  CHECK_EQ(eng.book().best_ask_price(), Price(300));
  eng.process(mk_new(4, Side::Buy, 300, 3));  // depletes 300
  CHECK(eng.book().best_ask_price() == kNoPrice);
  // bids at 200 and 10
  eng.process(mk_new(5, Side::Buy, 200, 3));
  eng.process(mk_new(6, Side::Buy, 10, 3));
  eng.process(mk_new(7, Side::Sell, 200, 3));
  CHECK_EQ(eng.book().best_bid_price(), Price(10));
}

TEST(engine_bbo_events_track_top_of_book) {
  VecSink sink;
  Engine eng(test_cfg(1024, /*bbo=*/true), sink);
  eng.process(mk_new(1, Side::Sell, 105, 10));
  eng.process(mk_new(2, Side::Buy, 100, 4));
  eng.process(mk_new(3, Side::Buy, 100, 2));   // bid qty changes
  eng.process(mk_cancel(2));                   // bid qty changes again
  eng.process(mk_new(4, Side::Buy, 90, 1));    // below best: NO bbo event

  std::vector<OutboundEvent> tops;
  for (const auto& e : sink.events) {
    if (e.type == EventType::BookTop) tops.push_back(e);
  }
  CHECK_EQ(tops.size(), std::size_t(4));
  CHECK(tops[0].price == kNoPrice && book_top_ask_price(tops[0]) == 105 &&
        tops[0].leaves == 10);
  CHECK(tops[1].price == 100 && tops[1].qty == 4);
  CHECK(tops[2].price == 100 && tops[2].qty == 6);
  CHECK(tops[3].price == 100 && tops[3].qty == 2);
}
