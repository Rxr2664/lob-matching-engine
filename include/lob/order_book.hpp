#pragma once
// ----------------------------------------------------------------------------
// order_book.hpp -- the limit order book data structure.
//
// Price-time priority is represented directly in memory:
//
//   * PRICE priority: price levels live in a flat array indexed by
//     (price - min_price). Best-price lookup is O(1) via cached indices
//     (best_bid_/best_ask_); advancing to the next non-empty level after a
//     depletion is a bitmap scan using countl_zero/countr_zero -- one or two
//     64-bit word reads in practice, never a tree walk and never a heap
//     allocation (contrast std::map<Price, Level>).
//
//   * TIME priority: each level is an intrusive doubly-linked FIFO of Order
//     nodes. New resting orders append at the tail; matching consumes from
//     the head. Cancel is O(1) unlink from anywhere in the list.
//
// Memory layout choices (these came out of latency profiling, see
// docs/BENCHMARKING.md):
//   * Order is alignas(64): exactly one cache line per order, so touching a
//     maker during matching never drags in a neighboring order's line.
//   * PriceLevel is 32 bytes: two levels per line, deliberately NOT padded --
//     matching walks *adjacent* levels, so packing neighbors on one line is
//     free prefetch. (Levels are written by a single thread; there is no
//     cross-thread false-sharing concern inside the book.)
//   * The occupancy bitmaps are tiny (band/8 bytes) and hot, so they stay
//     resident in L1/L2 and make "find next best level" almost free.
//
// The book performs zero heap allocation after construction. All methods are
// single-threaded by design: exactly one matching thread owns the book, and
// concurrency is handled upstream by the SPSC rings.
// ----------------------------------------------------------------------------
#include <cstdint>
#include <cstddef>
#include <vector>
#include <cassert>
#include <bit>

#include "lob/common.hpp"

namespace lob {

// Intrusive order node. Exactly one cache line.
struct alignas(kCacheLine) Order {
  OrderId id{};
  Price price{};
  Qty leaves{};   // remaining open quantity
  Qty orig{};     // quantity at entry (or at last replace)
  Side side{};
  Tif tif{};
  std::uint8_t pad_[6]{};
  Order* prev{};  // FIFO links within the price level
  Order* next{};
};
static_assert(sizeof(Order) == 64, "one cache line per order node");

// One price level: FIFO of orders + aggregates. 32 bytes = 2 per cache line.
struct PriceLevel {
  Order* head{};
  Order* tail{};
  Qty total{};           // sum of leaves across the FIFO
  std::uint32_t count{}; // number of orders in the FIFO
  std::uint32_t pad_{};
};
static_assert(sizeof(PriceLevel) == 32);

class OrderBook {
 public:
  OrderBook(Price min_price, Price max_price)
      : min_px_(min_price), max_px_(max_price),
        band_(static_cast<std::size_t>(max_price - min_price + 1)),
        words_((band_ + 63) / 64),
        bid_levels_(band_), ask_levels_(band_),
        bid_bits_(words_, 0), ask_bits_(words_, 0) {
    assert(max_price > min_price);
  }

  // ---- geometry -----------------------------------------------------------
  bool in_band(Price p) const noexcept { return p >= min_px_ && p <= max_px_; }
  Price min_price() const noexcept { return min_px_; }
  Price max_price() const noexcept { return max_px_; }
  Price price_at(std::int64_t idx) const noexcept { return min_px_ + idx; }
  std::int64_t index_of(Price p) const noexcept { return p - min_px_; }

  // ---- best-price access --------------------------------------------------
  std::int64_t best_bid() const noexcept { return best_bid_; }  // level index or -1
  std::int64_t best_ask() const noexcept { return best_ask_; }

  Price best_bid_price() const noexcept {
    return best_bid_ < 0 ? kNoPrice : price_at(best_bid_);
  }
  Price best_ask_price() const noexcept {
    return best_ask_ < 0 ? kNoPrice : price_at(best_ask_);
  }

  PriceLevel& level(Side s, std::int64_t idx) noexcept {
    return s == Side::Buy ? bid_levels_[static_cast<std::size_t>(idx)]
                          : ask_levels_[static_cast<std::size_t>(idx)];
  }
  const PriceLevel& level(Side s, std::int64_t idx) const noexcept {
    return const_cast<OrderBook*>(this)->level(s, idx);
  }

  // nullptr when out of band or empty (tools/tests convenience).
  const PriceLevel* find_level(Side s, Price p) const noexcept {
    if (!in_band(p)) return nullptr;
    const PriceLevel& l = level(s, index_of(p));
    return l.count ? &l : nullptr;
  }

  // Best bid/ask snapshot for market-data publication.
  void top(Price& bid_px, Qty& bid_qty, Price& ask_px, Qty& ask_qty) const noexcept {
    if (best_bid_ < 0) { bid_px = kNoPrice; bid_qty = 0; }
    else { bid_px = price_at(best_bid_); bid_qty = bid_levels_[std::size_t(best_bid_)].total; }
    if (best_ask_ < 0) { ask_px = kNoPrice; ask_qty = 0; }
    else { ask_px = price_at(best_ask_); ask_qty = ask_levels_[std::size_t(best_ask_)].total; }
  }

  // ---- mutation -----------------------------------------------------------

  // Rest an order (append at the tail of its level = time priority).
  // Precondition: in_band(o->price); the engine has already matched away any
  // crossing quantity, so adding here never crosses the book.
  void add(Order* o) noexcept {
    const std::int64_t i = index_of(o->price);
    PriceLevel& lvl = level(o->side, i);
    o->prev = lvl.tail;
    o->next = nullptr;
    if (lvl.tail) lvl.tail->next = o;
    else lvl.head = o;
    lvl.tail = o;
    lvl.total += o->leaves;
    if (++lvl.count == 1) {
      if (o->side == Side::Buy) {
        set_bit(bid_bits_, i);
        if (i > best_bid_) best_bid_ = i;
      } else {
        set_bit(ask_bits_, i);
        if (best_ask_ < 0 || i < best_ask_) best_ask_ = i;
      }
    }
  }

  // Remove an order from anywhere in its level (cancel, full fill, replace).
  // Subtracts the order's *remaining* leaves from the level total.
  void unlink(Order* o) noexcept {
    const std::int64_t i = index_of(o->price);
    PriceLevel& lvl = level(o->side, i);
    if (o->prev) o->prev->next = o->next; else lvl.head = o->next;
    if (o->next) o->next->prev = o->prev; else lvl.tail = o->prev;
    o->prev = o->next = nullptr;
    lvl.total -= o->leaves;
    if (--lvl.count == 0) {
      if (o->side == Side::Buy) {
        clear_bit(bid_bits_, i);
        if (i == best_bid_) best_bid_ = prev_set(bid_bits_, i - 1);
      } else {
        clear_bit(ask_bits_, i);
        if (i == best_ask_) best_ask_ = next_set(ask_bits_, i + 1);
      }
    }
  }

  // Reduce a resting order's contribution to its level total by `delta`
  // (in-place replace path; caller adjusts o->leaves itself).
  void reduce(Order* o, Qty delta) noexcept {
    level(o->side, index_of(o->price)).total -= delta;
  }

  // Liquidity available to a taker of `taker_side` up to (and including)
  // price `bound`, capped at `need` (FOK pre-check). Walks best -> worse
  // using the occupancy bitmap; stops as soon as `need` is covered.
  Qty available(Side taker_side, Price bound, Qty need) const noexcept {
    Qty sum = 0;
    if (taker_side == Side::Buy) {
      for (std::int64_t i = best_ask_; i >= 0; i = next_set(ask_bits_, i + 1)) {
        if (price_at(i) > bound) break;
        sum += ask_levels_[std::size_t(i)].total;
        if (sum >= need) break;
      }
    } else {
      for (std::int64_t i = best_bid_; i >= 0; i = prev_set(bid_bits_, i - 1)) {
        if (price_at(i) < bound) break;
        sum += bid_levels_[std::size_t(i)].total;
        if (sum >= need) break;
      }
    }
    return sum;
  }

  // Visit levels best -> worse. f(Price, const PriceLevel&).
  template <typename F>
  void for_each_level(Side s, F&& f, std::size_t max_levels = SIZE_MAX) const {
    std::size_t n = 0;
    if (s == Side::Buy) {
      for (std::int64_t i = best_bid_; i >= 0 && n < max_levels;
           i = prev_set(bid_bits_, i - 1), ++n) {
        f(price_at(i), bid_levels_[std::size_t(i)]);
      }
    } else {
      for (std::int64_t i = best_ask_; i >= 0 && n < max_levels;
           i = next_set(ask_bits_, i + 1), ++n) {
        f(price_at(i), ask_levels_[std::size_t(i)]);
      }
    }
  }

 private:
  static void set_bit(std::vector<std::uint64_t>& bits, std::int64_t i) noexcept {
    bits[std::size_t(i) >> 6] |= (1ull << (unsigned(i) & 63u));
  }
  static void clear_bit(std::vector<std::uint64_t>& bits, std::int64_t i) noexcept {
    bits[std::size_t(i) >> 6] &= ~(1ull << (unsigned(i) & 63u));
  }

  // Highest set bit at index <= i, or -1. (Next best bid after a depletion.)
  std::int64_t prev_set(const std::vector<std::uint64_t>& bits,
                        std::int64_t i) const noexcept {
    if (i < 0) return -1;
    std::size_t w = std::size_t(i) >> 6;
    const unsigned r = unsigned(i) & 63u;
    std::uint64_t word =
        bits[w] & (r == 63 ? ~0ull : ((1ull << (r + 1)) - 1));
    for (;;) {
      if (word) {
        return std::int64_t((w << 6) + 63u -
                            unsigned(std::countl_zero(word)));
      }
      if (w == 0) return -1;
      word = bits[--w];
    }
  }

  // Lowest set bit at index >= i, or -1. (Next best ask after a depletion.)
  std::int64_t next_set(const std::vector<std::uint64_t>& bits,
                        std::int64_t i) const noexcept {
    if (i >= std::int64_t(band_)) return -1;
    std::size_t w = std::size_t(i) >> 6;
    std::uint64_t word = bits[w] & (~0ull << (unsigned(i) & 63u));
    for (;;) {
      if (word) {
        return std::int64_t((w << 6) + unsigned(std::countr_zero(word)));
      }
      if (++w == words_) return -1;
      word = bits[w];
    }
  }

  Price min_px_;
  Price max_px_;
  std::size_t band_;
  std::size_t words_;

  std::int64_t best_bid_ = -1;  // level index of best bid, -1 when empty
  std::int64_t best_ask_ = -1;

  std::vector<PriceLevel> bid_levels_;
  std::vector<PriceLevel> ask_levels_;
  std::vector<std::uint64_t> bid_bits_;  // occupancy bitmap: bit i set <=> level i non-empty
  std::vector<std::uint64_t> ask_bits_;
};

}  // namespace lob
