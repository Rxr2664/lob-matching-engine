#pragma once
// ----------------------------------------------------------------------------
// feed.hpp -- deterministic synthetic order-flow generator.
//
// Generates a mix of new orders (passive + aggressive around a random-walking
// mid), cancels and replaces. Fully deterministic for a given seed.
//
// Closed-loop tracking: the generator keeps a set of order ids it believes
// are open, so cancels/replaces target *real* resting orders instead of
// guessing. The bench harness feeds terminal events (full fill / cancel /
// reject) back through a third SPSC ring, and the producer calls
// on_order_dead() to drop those ids -- exactly how a real trading client
// tracks its own open orders off the exchange's execution reports. This
// keeps the resting-book population in a realistic steady state instead of
// growing without bound.
//
// The generator itself is allocation-free after construction (dense vector +
// FlatMap, both pre-sized) so it never becomes the bottleneck being measured.
// ----------------------------------------------------------------------------
#include <cstdint>
#include <vector>

#include "lob/common.hpp"
#include "lob/messages.hpp"
#include "lob/flat_map.hpp"

namespace lob {

struct FeedConfig {
  std::uint64_t seed = 42;
  Price min_price = 1;
  Price max_price = 200000;
  Price start_mid = 100000;
  // op mix, percent (remainder = replaces)
  std::uint32_t new_pct = 56;
  std::uint32_t cancel_pct = 34;
  // of new orders, percent
  std::uint32_t aggressive_pct = 32;  // priced to cross the touch
  std::uint32_t market_pct = 3;
  std::uint32_t ioc_pct = 6;
  std::uint32_t fok_pct = 1;
  Qty max_qty = 200;
  std::size_t max_tracked = 1u << 20;  // capacity of the open-order tracker
};

class FeedGenerator {
 public:
  explicit FeedGenerator(const FeedConfig& cfg)
      : cfg_(cfg), pos_(cfg.max_tracked), rng_(cfg.seed ? cfg.seed : 1),
        mid_(cfg.start_mid) {
    live_.reserve(cfg.max_tracked);
    if (mid_ < cfg_.min_price + kMidMargin) mid_ = cfg_.min_price + kMidMargin;
    if (mid_ > cfg_.max_price - kMidMargin) mid_ = cfg_.max_price - kMidMargin;
  }

  InboundMsg next() {
    const std::uint32_t roll = static_cast<std::uint32_t>(rnd() % 100);
    if (roll < cfg_.new_pct || live_.empty()) return gen_new();
    if (roll < cfg_.new_pct + cfg_.cancel_pct) return gen_cancel();
    return gen_replace();
  }

  // Execution-report feedback: this order is no longer open.
  void on_order_dead(OrderId id) {
    std::uint32_t* p = pos_.find(id);
    if (p == nullptr) return;  // already dropped locally (e.g. we canceled it)
    forget_at(*p, id);
  }

  std::uint64_t issued() const { return next_id_ - 1; }
  std::size_t tracked_open() const { return live_.size(); }

 private:
  static constexpr Price kMidMargin = 64;

  struct LiveOrder {
    OrderId id;
    Price price;
    Qty qty;
  };

  std::uint64_t rnd() {  // xorshift64*
    std::uint64_t x = rng_;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng_ = x;
    return x * 0x2545F4914F6CDD1Dull;
  }

  Price clamp_px(Price p) const {
    if (p < cfg_.min_price) return cfg_.min_price;
    if (p > cfg_.max_price) return cfg_.max_price;
    return p;
  }

  void walk_mid() {
    mid_ += static_cast<Price>(rnd() % 3) - 1;  // -1, 0, +1 tick
    if (mid_ < cfg_.min_price + kMidMargin) mid_ = cfg_.min_price + kMidMargin;
    if (mid_ > cfg_.max_price - kMidMargin) mid_ = cfg_.max_price - kMidMargin;
  }

  InboundMsg gen_new() {
    walk_mid();
    InboundMsg m{};
    m.type = MsgType::NewOrder;
    m.id = next_id_++;
    m.side = (rnd() & 1) ? Side::Buy : Side::Sell;
    m.qty = 1 + (rnd() % cfg_.max_qty);

    const std::uint32_t troll = static_cast<std::uint32_t>(rnd() % 100);
    if (troll < cfg_.market_pct) {
      m.ord_type = OrdType::Market;
      m.tif = Tif::IOC;  // market flow never rests
      m.price = 0;
      return m;
    }
    m.ord_type = OrdType::Limit;
    if (troll < cfg_.market_pct + cfg_.ioc_pct) m.tif = Tif::IOC;
    else if (troll < cfg_.market_pct + cfg_.ioc_pct + cfg_.fok_pct) m.tif = Tif::FOK;
    else m.tif = Tif::GTC;

    const Price off = static_cast<Price>(rnd() % 12);
    const bool aggressive = (rnd() % 100) < cfg_.aggressive_pct;
    if (m.side == Side::Buy) {
      m.price = clamp_px(aggressive ? mid_ + off : mid_ - 1 - off);
    } else {
      m.price = clamp_px(aggressive ? mid_ - off : mid_ + 1 + off);
    }

    if (m.tif == Tif::GTC) track(m.id, m.price, m.qty);
    return m;
  }

  InboundMsg gen_cancel() {
    const std::size_t idx = static_cast<std::size_t>(rnd() % live_.size());
    const OrderId id = live_[idx].id;
    forget_at(static_cast<std::uint32_t>(idx), id);
    InboundMsg m{};
    m.type = MsgType::Cancel;
    m.id = id;
    m.side = Side::Buy;  // ignored by the engine for cancels it can resolve
    return m;
  }

  InboundMsg gen_replace() {
    const std::size_t idx = static_cast<std::size_t>(rnd() % live_.size());
    LiveOrder& lo = live_[idx];
    InboundMsg m{};
    m.type = MsgType::Replace;
    m.id = lo.id;
    if (rnd() & 1) {
      // size-down at the same price: exercises the keep-priority path
      m.price = lo.price;
      m.qty = 1 + (rnd() % lo.qty);
    } else {
      walk_mid();
      const Price off = static_cast<Price>(rnd() % 12);
      m.price = clamp_px((rnd() & 1) ? mid_ - 1 - off : mid_ + 1 + off);
      m.qty = 1 + (rnd() % cfg_.max_qty);
    }
    lo.price = m.price;
    lo.qty = m.qty;
    return m;
  }

  void track(OrderId id, Price px, Qty q) {
    if (live_.size() >= cfg_.max_tracked) return;  // stop tracking, never grow
    if (!pos_.insert(id, static_cast<std::uint32_t>(live_.size()))) return;
    live_.push_back(LiveOrder{id, px, q});
  }

  // O(1) swap-remove keeping the position index consistent.
  void forget_at(std::uint32_t idx, OrderId id) {
    const LiveOrder last = live_.back();
    live_[idx] = last;
    live_.pop_back();
    pos_.erase(id);
    if (last.id != id) {
      std::uint32_t* p = pos_.find(last.id);
      if (p != nullptr) *p = idx;
    }
  }

  FeedConfig cfg_;
  std::vector<LiveOrder> live_;
  FlatMap<std::uint32_t> pos_;  // id -> index into live_
  std::uint64_t rng_;
  OrderId next_id_ = 1;
  Price mid_;
};

}  // namespace lob
