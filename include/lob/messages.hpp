#pragma once
// ----------------------------------------------------------------------------
// messages.hpp -- fixed-size POD messages exchanged between threads.
//
// Both message types are trivially copyable and padded to exactly one cache
// line (64 bytes). Each ring slot therefore occupies its own line, so the
// producer and consumer never write to the same line for different slots
// (no false sharing between adjacent slots).
// ----------------------------------------------------------------------------
#include <cstdint>
#include <type_traits>

#include "lob/common.hpp"

namespace lob {

enum class MsgType : std::uint8_t {
  NewOrder = 1,
  Cancel   = 2,
  Replace  = 3,
  Shutdown = 4,  // harness control message, never reaches the matching logic
};

// Inbound message: feed thread -> matching thread.
//
//   NewOrder : id, side, ord_type, tif, price (limit only), qty
//   Cancel   : id = target order
//   Replace  : id = target order, price/qty = new values.
//              Semantics are documented in docs/ARCHITECTURE.md: qty is the
//              new *remaining* quantity; same-price + qty<=leaves keeps
//              time priority, anything else re-enters the book.
struct alignas(kCacheLine) InboundMsg {
  MsgType  type{};
  Side     side{};
  OrdType  ord_type{};
  Tif      tif{};
  std::uint32_t pad_{};
  OrderId  id{};
  Price    price{};
  Qty      qty{};
  std::uint64_t ts_enqueue_ns{};  // stamped by the producer immediately before push
};
static_assert(std::is_trivially_copyable_v<InboundMsg>);
static_assert(sizeof(InboundMsg) == 64, "one cache line per ring slot");

enum class EventType : std::uint8_t {
  Accepted = 1,  // order passed validation and entered the matching path
  Rejected = 2,  // order refused (reason field set), nothing else happened
  Fill     = 3,  // one execution; emitted once for the maker and once for the taker
  Canceled = 4,  // remaining quantity removed (user cancel or IOC/FOK/market remainder)
  Replaced = 5,  // cancel/replace applied
  BookTop  = 6,  // best-bid/offer changed (see field mapping below)
};

inline const char* to_string(EventType t) noexcept {
  switch (t) {
    case EventType::Accepted: return "ACCEPTED";
    case EventType::Rejected: return "REJECTED";
    case EventType::Fill:     return "FILL";
    case EventType::Canceled: return "CANCELED";
    case EventType::Replaced: return "REPLACED";
    case EventType::BookTop:  return "BOOK_TOP";
  }
  return "?";
}

inline constexpr std::uint8_t kFlagTaker = 0x01;  // set on the taker-side Fill event

// Outbound event: matching thread -> event/market-data thread.
//
// Field usage by event type (unused fields are zero):
//   Accepted : id, side, price (as submitted; 0 for market), qty=orig, leaves=orig
//   Rejected : id, side, reason, price, qty as submitted, leaves=0
//   Fill     : id = order this event is for, counter = opposite order,
//              price = execution (maker) price, qty = fill size,
//              leaves = remaining for `id`, flags bit0 = taker
//   Canceled : user cancel  -> id, side, price/qty from the resting order,
//                              leaves = qty removed, reason=NONE
//              remainder    -> fields from the inbound msg, reason=NO_LIQUIDITY
//                              or BOOK_FULL
//   Replaced : id, side, price = new price, qty = leaves = new remaining qty
//   BookTop  : price = best bid px (kNoPrice if none), qty = bid qty,
//              counter = best ask px bit-cast to u64 (kNoPrice if none),
//              leaves = ask qty
struct alignas(kCacheLine) OutboundEvent {
  EventType     type{};
  Side          side{};
  RejectReason  reason{};
  std::uint8_t  flags{};
  std::uint32_t pad_{};
  Seq           seq{};      // gap-free per-engine sequence number
  OrderId       id{};
  OrderId       counter{};
  Price         price{};
  Qty           qty{};
  Qty           leaves{};
  std::uint64_t ts_ns{};    // engine timestamp (0 when stamping is disabled)
};
static_assert(std::is_trivially_copyable_v<OutboundEvent>);
static_assert(sizeof(OutboundEvent) == 64, "one cache line per ring slot");

// Helper for reading the ask price out of a BookTop event.
inline Price book_top_ask_price(const OutboundEvent& e) noexcept {
  return static_cast<Price>(e.counter);
}

}  // namespace lob
