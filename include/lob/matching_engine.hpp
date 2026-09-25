#pragma once
// ----------------------------------------------------------------------------
// matching_engine.hpp -- validation + price-time priority matching.
//
// The engine is single-threaded and owns the book, the order pool and the
// open-order index. It is templated on the event Sink so event emission
// inlines completely (the bench sink is a push into the outbound SPSC ring;
// the test sink is a vector append; the replay sink is a pretty-printer).
//
// Invariants maintained (and enforced by the differential fuzz test against
// an independent reference implementation):
//   * The book never crosses: incoming flow is matched before it rests.
//   * Fills always execute at the MAKER's price (price improvement to taker).
//   * Within a level, makers fill strictly in arrival order (time priority).
//   * Every message produces >= 1 event, and event `seq` is gap-free.
//   * After construction, process() performs zero heap allocation
//     (proved by tests/test_no_alloc.cpp).
//
// Message semantics are specified precisely in docs/ARCHITECTURE.md.
// ----------------------------------------------------------------------------
#include <cstdint>
#include <algorithm>

#include "lob/common.hpp"
#include "lob/messages.hpp"
#include "lob/order_book.hpp"
#include "lob/pool.hpp"
#include "lob/flat_map.hpp"

namespace lob {

struct EngineConfig {
  Price min_price = 1;
  Price max_price = 200000;
  std::size_t max_open_orders = 1u << 20;  // pool + index capacity
  bool publish_bbo = true;    // emit BookTop events when best bid/offer changes
  bool stamp_events = true;   // stamp ts_ns on outbound events (off => 0)
};

struct EngineStats {
  std::uint64_t msgs = 0;         // messages processed
  std::uint64_t accepted = 0;     // new orders that passed validation
  std::uint64_t rejected = 0;     // validation failures
  std::uint64_t user_cancels = 0; // explicit cancels honored
  std::uint64_t expired = 0;      // IOC/FOK/market remainders + BOOK_FULL cuts
  std::uint64_t replaced = 0;     // replace operations honored
  std::uint64_t trades = 0;       // individual match executions
  std::uint64_t traded_qty = 0;   // total quantity crossed
  std::uint64_t bbo_updates = 0;  // BookTop events emitted
};

template <typename Sink>
class MatchingEngine {
 public:
  MatchingEngine(const EngineConfig& cfg, Sink& sink)
      : cfg_(cfg),
        book_(cfg.min_price, cfg.max_price),
        pool_(cfg.max_open_orders),
        orders_(cfg.max_open_orders),
        sink_(sink) {}

  MatchingEngine(const MatchingEngine&) = delete;
  MatchingEngine& operator=(const MatchingEngine&) = delete;

  // Hot-path entry point: exactly one call per inbound message.
  void process(const InboundMsg& m) {
    ++stats_.msgs;
    switch (m.type) {
      case MsgType::NewOrder: on_new(m); break;
      case MsgType::Cancel:   on_cancel(m); break;
      case MsgType::Replace:  on_replace(m); break;
      default:                reject(m, RejectReason::BadMessage); break;
    }
    if (cfg_.publish_bbo) publish_bbo();
  }

  const EngineStats& stats() const noexcept { return stats_; }
  const OrderBook& book() const noexcept { return book_; }
  std::size_t open_orders() const noexcept { return orders_.size(); }
  const EngineConfig& config() const noexcept { return cfg_; }

 private:
  // ---- new orders ---------------------------------------------------------
  void on_new(const InboundMsg& m) {
    if (m.id == 0) { reject(m, RejectReason::BadMessage); return; }
    if (m.qty == 0) { reject(m, RejectReason::BadQty); return; }
    const bool is_limit = (m.ord_type == OrdType::Limit);
    if (is_limit && !book_.in_band(m.price)) {
      reject(m, RejectReason::BadPrice);
      return;
    }
    if (orders_.find(m.id) != nullptr) {
      reject(m, RejectReason::DuplicateId);
      return;
    }

    const Price bound = is_limit
        ? m.price
        : (m.side == Side::Buy ? book_.max_price() : book_.min_price());

    // FOK: all-or-none. Pre-scan crossable liquidity; if insufficient,
    // cancel the whole order without printing a single fill.
    if (m.tif == Tif::FOK) {
      const Qty avail = book_.available(m.side, bound, m.qty);
      if (avail < m.qty) {
        ++stats_.accepted;
        emit(EventType::Accepted, m.side, RejectReason::None, 0, m.id, 0,
             m.price, m.qty, m.qty);
        ++stats_.expired;
        emit(EventType::Canceled, m.side, RejectReason::NoLiquidity, 0, m.id,
             0, m.price, m.qty, m.qty);
        return;
      }
    }

    ++stats_.accepted;
    emit(EventType::Accepted, m.side, RejectReason::None, 0, m.id, 0, m.price,
         m.qty, m.qty);

    Qty leaves = m.qty;
    match(m.side, bound, m.id, leaves);
    if (leaves == 0) return;

    if (is_limit && m.tif == Tif::GTC) {
      rest_remainder(m, leaves);
    } else {
      // IOC remainder, or market flow that exhausted the opposite side.
      ++stats_.expired;
      emit(EventType::Canceled, m.side, RejectReason::NoLiquidity, 0, m.id, 0,
           m.price, m.qty, leaves);
    }
  }

  void rest_remainder(const InboundMsg& m, Qty leaves) {
    Order* o = pool_.alloc();
    if (o == nullptr) {
      ++stats_.expired;
      emit(EventType::Canceled, m.side, RejectReason::BookFull, 0, m.id, 0,
           m.price, m.qty, leaves);
      return;
    }
    o->id = m.id;
    o->price = m.price;
    o->leaves = leaves;
    o->orig = m.qty;
    o->side = m.side;
    o->tif = m.tif;
    o->prev = o->next = nullptr;
    if (!orders_.insert(m.id, o)) {  // only possible failure: index full
      pool_.release(o);
      ++stats_.expired;
      emit(EventType::Canceled, m.side, RejectReason::BookFull, 0, m.id, 0,
           m.price, m.qty, leaves);
      return;
    }
    book_.add(o);
  }

  // ---- matching core ------------------------------------------------------
  // Consume liquidity from the opposite side while it crosses `bound`.
  // The per-iteration work is: one bitmap-backed best lookup, one head
  // dereference, arithmetic, two event emissions -- no allocation, no locks.
  void match(Side taker_side, Price bound, OrderId taker_id, Qty& leaves) {
    if (taker_side == Side::Buy) {
      while (leaves != 0) {
        const std::int64_t bi = book_.best_ask();
        if (bi < 0) return;
        const Price px = book_.price_at(bi);
        if (px > bound) return;
        fill_against(Side::Sell, bi, px, taker_side, taker_id, leaves);
      }
    } else {
      while (leaves != 0) {
        const std::int64_t bi = book_.best_bid();
        if (bi < 0) return;
        const Price px = book_.price_at(bi);
        if (px < bound) return;
        fill_against(Side::Buy, bi, px, taker_side, taker_id, leaves);
      }
    }
  }

  void fill_against(Side maker_side, std::int64_t level_idx, Price px,
                    Side taker_side, OrderId taker_id, Qty& leaves) {
    PriceLevel& lvl = book_.level(maker_side, level_idx);
    Order* maker = lvl.head;  // strict FIFO: always the oldest order
    const Qty q = std::min(leaves, maker->leaves);
    maker->leaves -= q;
    lvl.total -= q;
    leaves -= q;
    ++stats_.trades;
    stats_.traded_qty += q;
    emit(EventType::Fill, maker_side, RejectReason::None, 0, maker->id,
         taker_id, px, q, maker->leaves);
    emit(EventType::Fill, taker_side, RejectReason::None, kFlagTaker, taker_id,
         maker->id, px, q, leaves);
    if (maker->leaves == 0) {
      const OrderId mid = maker->id;
      book_.unlink(maker);  // removes head; advances best level if depleted
      orders_.erase(mid);
      pool_.release(maker);
    }
  }

  // ---- cancel -------------------------------------------------------------
  void on_cancel(const InboundMsg& m) {
    if (m.id == 0) { reject(m, RejectReason::BadMessage); return; }
    Order** slot = orders_.find(m.id);
    if (slot == nullptr) { reject(m, RejectReason::UnknownOrder); return; }
    Order* o = *slot;
    ++stats_.user_cancels;
    emit(EventType::Canceled, o->side, RejectReason::None, 0, o->id, 0,
         o->price, o->orig, o->leaves);
    book_.unlink(o);
    orders_.erase(m.id);
    pool_.release(o);
  }

  // ---- cancel/replace -----------------------------------------------------
  // qty is the new *remaining* quantity. Same price + qty <= current leaves
  // reduces in place and KEEPS time priority; any price change or size
  // increase re-enters the order (loses priority) and may trade immediately.
  void on_replace(const InboundMsg& m) {
    if (m.id == 0) { reject(m, RejectReason::BadMessage); return; }
    if (m.qty == 0) { reject(m, RejectReason::BadQty); return; }
    if (!book_.in_band(m.price)) { reject(m, RejectReason::BadPrice); return; }
    Order** slot = orders_.find(m.id);
    if (slot == nullptr) { reject(m, RejectReason::UnknownOrder); return; }
    Order* o = *slot;
    ++stats_.replaced;

    if (m.price == o->price && m.qty <= o->leaves) {
      const Qty delta = o->leaves - m.qty;
      book_.reduce(o, delta);
      o->leaves = m.qty;
      o->orig = m.qty;
      emit(EventType::Replaced, o->side, RejectReason::None, 0, o->id, 0,
           o->price, o->orig, o->leaves);
      return;
    }

    book_.unlink(o);
    o->price = m.price;
    o->leaves = m.qty;
    o->orig = m.qty;
    emit(EventType::Replaced, o->side, RejectReason::None, 0, o->id, 0,
         o->price, o->orig, o->leaves);
    Qty leaves = o->leaves;
    match(o->side, o->price, o->id, leaves);
    o->leaves = leaves;
    if (leaves == 0) {
      orders_.erase(o->id);
      pool_.release(o);
      return;
    }
    book_.add(o);  // tail of the new level: time priority lost by design
  }

  // ---- event emission -----------------------------------------------------
  void reject(const InboundMsg& m, RejectReason r) {
    ++stats_.rejected;
    emit(EventType::Rejected, m.side, r, 0, m.id, 0, m.price, m.qty, 0);
  }

  void emit(EventType t, Side s, RejectReason r, std::uint8_t flags,
            OrderId id, OrderId counter, Price px, Qty qty, Qty leaves) {
    OutboundEvent e{};
    e.type = t;
    e.side = s;
    e.reason = r;
    e.flags = flags;
    e.seq = ++event_seq_;
    e.id = id;
    e.counter = counter;
    e.price = px;
    e.qty = qty;
    e.leaves = leaves;
    e.ts_ns = cfg_.stamp_events ? now_ns() : 0;
    sink_(e);
  }

  void publish_bbo() {
    Price bp, ap;
    Qty bq, aq;
    book_.top(bp, bq, ap, aq);
    if (bp == last_bid_px_ && bq == last_bid_qty_ && ap == last_ask_px_ &&
        aq == last_ask_qty_) {
      return;
    }
    last_bid_px_ = bp;
    last_bid_qty_ = bq;
    last_ask_px_ = ap;
    last_ask_qty_ = aq;
    ++stats_.bbo_updates;
    OutboundEvent e{};
    e.type = EventType::BookTop;
    e.seq = ++event_seq_;
    e.price = bp;
    e.qty = bq;
    e.counter = static_cast<std::uint64_t>(ap);
    e.leaves = aq;
    e.ts_ns = cfg_.stamp_events ? now_ns() : 0;
    sink_(e);
  }

  EngineConfig cfg_;
  OrderBook book_;
  FixedPool<Order> pool_;
  FlatMap<Order*> orders_;  // open-order index: id -> node
  Sink& sink_;
  EngineStats stats_{};
  Seq event_seq_ = 0;

  Price last_bid_px_ = kNoPrice + 1;  // force first BookTop to publish
  Qty last_bid_qty_ = 0;
  Price last_ask_px_ = kNoPrice + 1;
  Qty last_ask_qty_ = 0;
};

}  // namespace lob
