// Differential fuzz test.
//
// A deliberately naive reference implementation of the exact same matching
// semantics is written here with std::map + std::deque (slow, obvious,
// easy to audit). We then throw 150,000 randomized operations -- news across
// all order types/TIFs, cancels, replaces, out-of-band prices, zero
// quantities, duplicate ids -- at BOTH implementations inside a narrow price
// band (which forces constant crossing, level depletion and deep walks), and
// demand that:
//   1. every emitted event matches field-for-field (type, id, counter,
//      price, qty, leaves, reason, flags, seq), and
//   2. periodically, the full book state matches: every level's price,
//      aggregate qty, order count, and the exact FIFO order of order ids.
//
// If the optimized engine (bitmaps, intrusive lists, pools, open addressing)
// diverges from the readable spec in any reachable way, this test finds it.
#include <cstdint>
#include <deque>
#include <map>
#include <unordered_map>
#include <vector>

#include "harness.hpp"
#include "lob/matching_engine.hpp"

using namespace lob;

namespace {

struct VecSink {
  std::vector<OutboundEvent> events;
  void operator()(const OutboundEvent& e) { events.push_back(e); }
};

// ---------------------------------------------------------------------------
// Reference implementation (the "spec").
// ---------------------------------------------------------------------------
class ReferenceBook {
 public:
  ReferenceBook(Price min_px, Price max_px, std::vector<OutboundEvent>& out)
      : min_px_(min_px), max_px_(max_px), out_(out) {}

  void process(const InboundMsg& m) {
    switch (m.type) {
      case MsgType::NewOrder: on_new(m); break;
      case MsgType::Cancel: on_cancel(m); break;
      case MsgType::Replace: on_replace(m); break;
      default: reject(m, RejectReason::BadMessage); break;
    }
  }

  struct RefOrder {
    OrderId id;
    Price price;
    Qty leaves;
    Qty orig;
    Side side;
  };

  using BidMap = std::map<Price, std::deque<RefOrder>, std::greater<Price>>;
  using AskMap = std::map<Price, std::deque<RefOrder>, std::less<Price>>;
  const BidMap& bids() const { return bids_; }
  const AskMap& asks() const { return asks_; }

 private:
  bool in_band(Price p) const { return p >= min_px_ && p <= max_px_; }

  void emit(EventType t, Side s, RejectReason r, std::uint8_t flags,
            OrderId id, OrderId counter, Price px, Qty qty, Qty leaves) {
    OutboundEvent e{};
    e.type = t; e.side = s; e.reason = r; e.flags = flags;
    e.seq = ++seq_; e.id = id; e.counter = counter;
    e.price = px; e.qty = qty; e.leaves = leaves; e.ts_ns = 0;
    out_.push_back(e);
  }

  void reject(const InboundMsg& m, RejectReason r) {
    emit(EventType::Rejected, m.side, r, 0, m.id, 0, m.price, m.qty, 0);
  }

  Qty available(Side taker, Price bound, Qty need) const {
    Qty sum = 0;
    if (taker == Side::Buy) {
      for (const auto& [px, dq] : asks_) {
        if (px > bound) break;
        for (const RefOrder& o : dq) sum += o.leaves;
        if (sum >= need) break;
      }
    } else {
      for (const auto& [px, dq] : bids_) {
        if (px < bound) break;
        for (const RefOrder& o : dq) sum += o.leaves;
        if (sum >= need) break;
      }
    }
    return sum;
  }

  template <typename Book>
  void match_one_side(Book& book, Side maker_side, Side taker_side,
                      OrderId taker_id, Price bound, Qty& leaves) {
    while (leaves != 0 && !book.empty()) {
      auto it = book.begin();
      const Price px = it->first;
      if (taker_side == Side::Buy ? (px > bound) : (px < bound)) return;
      RefOrder& maker = it->second.front();
      const Qty q = std::min(leaves, maker.leaves);
      maker.leaves -= q;
      leaves -= q;
      emit(EventType::Fill, maker_side, RejectReason::None, 0, maker.id,
           taker_id, px, q, maker.leaves);
      emit(EventType::Fill, taker_side, RejectReason::None, kFlagTaker,
           taker_id, maker.id, px, q, leaves);
      if (maker.leaves == 0) {
        index_.erase(maker.id);
        it->second.pop_front();
        if (it->second.empty()) book.erase(it);
      }
    }
  }

  void match(Side taker_side, Price bound, OrderId taker_id, Qty& leaves) {
    if (taker_side == Side::Buy) {
      match_one_side(asks_, Side::Sell, Side::Buy, taker_id, bound, leaves);
    } else {
      match_one_side(bids_, Side::Buy, Side::Sell, taker_id, bound, leaves);
    }
  }

  void rest(const RefOrder& o) {
    if (o.side == Side::Buy) bids_[o.price].push_back(o);
    else asks_[o.price].push_back(o);
    index_[o.id] = {o.side};
  }

  void on_new(const InboundMsg& m) {
    if (m.id == 0) { reject(m, RejectReason::BadMessage); return; }
    if (m.qty == 0) { reject(m, RejectReason::BadQty); return; }
    const bool is_limit = (m.ord_type == OrdType::Limit);
    if (is_limit && !in_band(m.price)) { reject(m, RejectReason::BadPrice); return; }
    if (index_.count(m.id)) { reject(m, RejectReason::DuplicateId); return; }
    const Price bound =
        is_limit ? m.price : (m.side == Side::Buy ? max_px_ : min_px_);
    if (m.tif == Tif::FOK && available(m.side, bound, m.qty) < m.qty) {
      emit(EventType::Accepted, m.side, RejectReason::None, 0, m.id, 0,
           m.price, m.qty, m.qty);
      emit(EventType::Canceled, m.side, RejectReason::NoLiquidity, 0, m.id, 0,
           m.price, m.qty, m.qty);
      return;
    }
    emit(EventType::Accepted, m.side, RejectReason::None, 0, m.id, 0, m.price,
         m.qty, m.qty);
    Qty leaves = m.qty;
    match(m.side, bound, m.id, leaves);
    if (leaves == 0) return;
    if (is_limit && m.tif == Tif::GTC) {
      rest(RefOrder{m.id, m.price, leaves, m.qty, m.side});
    } else {
      emit(EventType::Canceled, m.side, RejectReason::NoLiquidity, 0, m.id, 0,
           m.price, m.qty, leaves);
    }
  }

  struct Loc {
    Side side;
  };

  RefOrder* locate(OrderId id, Price* out_px = nullptr) {
    auto it = index_.find(id);
    if (it == index_.end()) return nullptr;
    if (it->second.side == Side::Buy) {
      for (auto& [px, dq] : bids_) {
        for (RefOrder& o : dq) {
          if (o.id == id) { if (out_px) *out_px = px; return &o; }
        }
      }
    } else {
      for (auto& [px, dq] : asks_) {
        for (RefOrder& o : dq) {
          if (o.id == id) { if (out_px) *out_px = px; return &o; }
        }
      }
    }
    return nullptr;
  }

  void remove_from_book(OrderId id, Side side, Price px) {
    auto scrub = [&](auto& book) {
      auto it = book.find(px);
      auto& dq = it->second;
      for (auto oit = dq.begin(); oit != dq.end(); ++oit) {
        if (oit->id == id) { dq.erase(oit); break; }
      }
      if (dq.empty()) book.erase(it);
    };
    if (side == Side::Buy) scrub(bids_);
    else scrub(asks_);
  }

  void on_cancel(const InboundMsg& m) {
    if (m.id == 0) { reject(m, RejectReason::BadMessage); return; }
    RefOrder* o = locate(m.id);
    if (!o) { reject(m, RejectReason::UnknownOrder); return; }
    emit(EventType::Canceled, o->side, RejectReason::None, 0, o->id, 0,
         o->price, o->orig, o->leaves);
    const Side s = o->side;
    const Price px = o->price;
    index_.erase(m.id);
    remove_from_book(m.id, s, px);
  }

  void on_replace(const InboundMsg& m) {
    if (m.id == 0) { reject(m, RejectReason::BadMessage); return; }
    if (m.qty == 0) { reject(m, RejectReason::BadQty); return; }
    if (!in_band(m.price)) { reject(m, RejectReason::BadPrice); return; }
    RefOrder* o = locate(m.id);
    if (!o) { reject(m, RejectReason::UnknownOrder); return; }

    if (m.price == o->price && m.qty <= o->leaves) {
      o->leaves = m.qty;
      o->orig = m.qty;
      emit(EventType::Replaced, o->side, RejectReason::None, 0, o->id, 0,
           o->price, o->orig, o->leaves);
      return;
    }
    const Side s = o->side;
    const Price old_px = o->price;
    remove_from_book(m.id, s, old_px);
    index_.erase(m.id);
    emit(EventType::Replaced, s, RejectReason::None, 0, m.id, 0, m.price,
         m.qty, m.qty);
    Qty leaves = m.qty;
    match(s, m.price, m.id, leaves);
    if (leaves == 0) return;
    rest(RefOrder{m.id, m.price, leaves, m.qty, s});
  }

  Price min_px_;
  Price max_px_;
  BidMap bids_;
  AskMap asks_;
  std::unordered_map<OrderId, Loc> index_;  // open-order ids
  std::vector<OutboundEvent>& out_;
  Seq seq_ = 0;
};

bool events_equal(const OutboundEvent& a, const OutboundEvent& b) {
  return a.type == b.type && a.side == b.side && a.reason == b.reason &&
         a.flags == b.flags && a.seq == b.seq && a.id == b.id &&
         a.counter == b.counter && a.price == b.price && a.qty == b.qty &&
         a.leaves == b.leaves;
}

}  // namespace

TEST(fuzz_engine_matches_reference_spec) {
  constexpr Price kMin = 1, kMax = 120;  // tight band => constant crossing
  constexpr int kOps = 150000;

  EngineConfig cfg;
  cfg.min_price = kMin;
  cfg.max_price = kMax;
  cfg.max_open_orders = 1u << 17;  // ample: BOOK_FULL must never fire here
  cfg.publish_bbo = false;
  cfg.stamp_events = false;

  VecSink esink;
  MatchingEngine<VecSink> eng(cfg, esink);
  std::vector<OutboundEvent> rout;
  ReferenceBook ref(kMin, kMax, rout);

  std::uint64_t rng = 0xC0FFEE123456789ull;
  auto rnd = [&] {
    rng ^= rng >> 12; rng ^= rng << 25; rng ^= rng >> 27;
    return rng * 0x2545F4914F6CDD1Dull;
  };

  std::vector<OrderId> maybe_open;  // ids we ever rested (some already gone)
  OrderId next_id = 1;
  std::uint64_t event_mismatches = 0;
  std::uint64_t book_mismatches = 0;
  std::size_t cursor = 0;  // compared prefix of the event streams

  auto compare_book = [&] {
    // engine -> flattened level snapshot
    struct Snap { Price px; Qty total; std::uint32_t count; std::vector<OrderId> fifo; };
    for (Side s : {Side::Buy, Side::Sell}) {
      std::vector<Snap> es;
      eng.book().for_each_level(s, [&](Price p, const PriceLevel& l) {
        Snap sn{p, l.total, l.count, {}};
        for (const Order* o = l.head; o; o = o->next) sn.fifo.push_back(o->id);
        es.push_back(std::move(sn));
      });
      std::vector<Snap> rs;
      auto flatten = [&](const auto& book) {
        for (const auto& [px, dq] : book) {
          Snap sn{px, 0, static_cast<std::uint32_t>(dq.size()), {}};
          for (const auto& o : dq) { sn.total += o.leaves; sn.fifo.push_back(o.id); }
          rs.push_back(std::move(sn));
        }
      };
      if (s == Side::Buy) flatten(ref.bids()); else flatten(ref.asks());
      if (es.size() != rs.size()) { ++book_mismatches; continue; }
      for (std::size_t i = 0; i < es.size(); ++i) {
        if (es[i].px != rs[i].px || es[i].total != rs[i].total ||
            es[i].count != rs[i].count || es[i].fifo != rs[i].fifo) {
          ++book_mismatches;
        }
      }
    }
  };

  for (int op = 0; op < kOps; ++op) {
    InboundMsg m{};
    const std::uint32_t what = static_cast<std::uint32_t>(rnd() % 100);
    if (what < 55 || maybe_open.empty()) {
      m.type = MsgType::NewOrder;
      // 3% duplicate-id probes, otherwise fresh
      if (what < 3 && !maybe_open.empty()) {
        m.id = maybe_open[rnd() % maybe_open.size()];
      } else {
        m.id = next_id++;
      }
      m.side = (rnd() & 1) ? Side::Buy : Side::Sell;
      const std::uint32_t t = static_cast<std::uint32_t>(rnd() % 100);
      m.ord_type = t < 8 ? OrdType::Market : OrdType::Limit;
      const std::uint32_t tf = static_cast<std::uint32_t>(rnd() % 100);
      m.tif = tf < 12 ? Tif::IOC : (tf < 20 ? Tif::FOK : Tif::GTC);
      // mostly in band, ~4% out of band, ~2% zero qty
      m.price = static_cast<Price>(rnd() % (kMax + 10)) - 4;
      m.qty = (rnd() % 50 == 0) ? 0 : 1 + (rnd() % 40);
      if (m.ord_type == OrdType::Limit && m.tif == Tif::GTC && m.qty &&
          m.id >= next_id - 1) {
        maybe_open.push_back(m.id);
      }
    } else if (what < 80) {
      m.type = MsgType::Cancel;
      // mostly plausible targets, sometimes garbage
      m.id = (rnd() % 5 == 0) ? (next_id + 1000 + rnd() % 50)
                              : maybe_open[rnd() % maybe_open.size()];
      m.side = (rnd() & 1) ? Side::Buy : Side::Sell;
    } else {
      m.type = MsgType::Replace;
      m.id = (rnd() % 6 == 0) ? (next_id + 1000 + rnd() % 50)
                              : maybe_open[rnd() % maybe_open.size()];
      m.price = static_cast<Price>(rnd() % (kMax + 6)) - 2;
      m.qty = (rnd() % 40 == 0) ? 0 : 1 + (rnd() % 40);
      m.side = (rnd() & 1) ? Side::Buy : Side::Sell;
    }

    eng.process(m);
    ref.process(m);

    if (esink.events.size() != rout.size()) {
      ++event_mismatches;
      break;  // streams desynced; further comparison is noise
    }
    for (; cursor < rout.size(); ++cursor) {
      if (!events_equal(esink.events[cursor], rout[cursor])) {
        ++event_mismatches;
        std::printf(
            "    first divergence at event %zu (op %d): engine{t=%d id=%llu "
            "px=%lld q=%llu lv=%llu} ref{t=%d id=%llu px=%lld q=%llu lv=%llu}\n",
            cursor, op, int(esink.events[cursor].type),
            (unsigned long long)esink.events[cursor].id,
            (long long)esink.events[cursor].price,
            (unsigned long long)esink.events[cursor].qty,
            (unsigned long long)esink.events[cursor].leaves,
            int(rout[cursor].type), (unsigned long long)rout[cursor].id,
            (long long)rout[cursor].price,
            (unsigned long long)rout[cursor].qty,
            (unsigned long long)rout[cursor].leaves);
        break;
      }
    }
    if (event_mismatches) break;
    if (op % 500 == 0) compare_book();
  }
  compare_book();

  CHECK_EQ(event_mismatches, std::uint64_t(0));
  CHECK_EQ(book_mismatches, std::uint64_t(0));
  CHECK(esink.events.size() > 100000);  // sanity: the fuzz actually did work
}
