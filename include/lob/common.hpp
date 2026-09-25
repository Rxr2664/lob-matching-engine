#pragma once
// ----------------------------------------------------------------------------
// common.hpp -- core scalar types, enums and small utilities.
//
// Design notes:
//  * Prices are integer ticks (Price). All arithmetic is exact; there is no
//    floating point anywhere on the hot path.
//  * The monotonic clock is read through now_ns(). On Linux/glibc this is a
//    vDSO call (~20-25ns) and does not enter the kernel.
//  * cpu_relax() is used inside spin loops to reduce pipeline pressure and
//    be polite to a hyperthread sibling.
// ----------------------------------------------------------------------------
#include <cstdint>
#include <cstddef>
#include <ctime>
#include <limits>

namespace lob {

inline constexpr std::size_t kCacheLine = 64;

using OrderId = std::uint64_t;   // 0 is reserved / invalid
using Price   = std::int64_t;    // integer ticks
using Qty     = std::uint64_t;   // shares / contracts
using Seq     = std::uint64_t;   // monotonically increasing event sequence

// Sentinel used in market-data events when a side of the book is empty.
inline constexpr Price kNoPrice = std::numeric_limits<Price>::min();

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };
enum class OrdType : std::uint8_t { Limit = 0, Market = 1 };
enum class Tif : std::uint8_t { GTC = 0, IOC = 1, FOK = 2 };

enum class RejectReason : std::uint8_t {
  None = 0,
  BadMessage,    // malformed input (e.g. order id 0)
  BadQty,        // quantity of zero
  BadPrice,      // limit price outside the configured price band
  DuplicateId,   // an open order with this id already exists
  UnknownOrder,  // cancel/replace target does not exist
  BookFull,      // order pool / open-order table exhausted
  NoLiquidity,   // unfilled remainder of IOC/FOK/market flow
};

inline const char* to_string(Side s) noexcept { return s == Side::Buy ? "BUY" : "SELL"; }

inline const char* to_string(OrdType t) noexcept {
  return t == OrdType::Limit ? "LIMIT" : "MARKET";
}

inline const char* to_string(Tif t) noexcept {
  switch (t) {
    case Tif::GTC: return "GTC";
    case Tif::IOC: return "IOC";
    case Tif::FOK: return "FOK";
  }
  return "?";
}

inline const char* to_string(RejectReason r) noexcept {
  switch (r) {
    case RejectReason::None:         return "NONE";
    case RejectReason::BadMessage:   return "BAD_MESSAGE";
    case RejectReason::BadQty:       return "BAD_QTY";
    case RejectReason::BadPrice:     return "BAD_PRICE";
    case RejectReason::DuplicateId:  return "DUPLICATE_ID";
    case RejectReason::UnknownOrder: return "UNKNOWN_ORDER";
    case RejectReason::BookFull:     return "BOOK_FULL";
    case RejectReason::NoLiquidity:  return "NO_LIQUIDITY";
  }
  return "?";
}

// Monotonic wall-independent nanosecond clock (vDSO fast path on Linux).
inline std::uint64_t now_ns() noexcept {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return std::uint64_t(ts.tv_sec) * 1000000000ull + std::uint64_t(ts.tv_nsec);
}

// Spin-loop hint.
inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  asm volatile("yield" ::: "memory");
#else
  // no-op on other architectures
#endif
}

}  // namespace lob
