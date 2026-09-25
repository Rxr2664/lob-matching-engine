#pragma once
// ----------------------------------------------------------------------------
// spsc_ring.hpp -- bounded lock-free single-producer/single-consumer queue.
//
// This is the handoff primitive between the feed thread and the matching
// thread (and again between the matching thread and the event consumer).
// It is wait-free for both sides: try_push / try_pop complete in a bounded
// number of steps and never block, never allocate, and never touch a lock.
//
// Memory-ordering argument (why acquire/release is sufficient):
//   * Only the producer writes tail_; only the consumer writes head_.
//   * The producer writes the slot, *then* publishes it with a release store
//     of tail_. The consumer's acquire load of tail_ therefore synchronizes-
//     with that store, making the slot write visible before it is read.
//   * Symmetrically, the consumer's release store of head_ guarantees the
//     slot has been fully copied out before the producer may overwrite it.
//   No seq_cst fences are needed; there is exactly one writer per index.
//
// Layout / false-sharing:
//   * Producer-owned state (tail_ + its cached copy of head_) and
//     consumer-owned state (head_ + its cached copy of tail_) live on
//     separate cache lines, so the two threads never contend on a line for
//     their private bookkeeping.
//   * The cached indices are the classic optimization from Rigtorp/Vyukov
//     queues: the producer only re-reads the consumer's head_ (a cross-core
//     cache miss) when its cached copy says the ring *looks* full, and vice
//     versa. Under steady state each side runs out of its own cache line.
//   * Slots are required to be trivially copyable; with 64-byte messages
//     each slot is exactly one line.
// ----------------------------------------------------------------------------
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <type_traits>
#include <algorithm>

#include "lob/common.hpp"

namespace lob {

template <typename T>
class SpscRing {
  static_assert(std::is_trivially_copyable_v<T>,
                "ring slots are copied raw between threads");

 public:
  // capacity must be a power of two (checked). The ring holds up to
  // `capacity` elements.
  explicit SpscRing(std::size_t capacity)
      : capacity_(capacity), mask_(capacity - 1),
        buf_(static_cast<T*>(::operator new(sizeof(T) * capacity,
                                            std::align_val_t{kCacheLine}))) {
    if (capacity < 2 || (capacity & (capacity - 1)) != 0) {
      ::operator delete(buf_, std::align_val_t{kCacheLine});
      buf_ = nullptr;
      throw_bad_capacity();
    }
  }

  ~SpscRing() {
    if (buf_) ::operator delete(buf_, std::align_val_t{kCacheLine});
  }

  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;

  // ---- producer side ------------------------------------------------------
  bool try_push(const T& v) noexcept {
    const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
    if (tail - cached_head_ >= capacity_) {
      cached_head_ = head_.load(std::memory_order_acquire);
      if (tail - cached_head_ >= capacity_) return false;  // genuinely full
    }
    buf_[tail & mask_] = v;
    tail_.store(tail + 1, std::memory_order_release);  // publish
    return true;
  }

  // ---- consumer side ------------------------------------------------------
  bool try_pop(T& out) noexcept {
    const std::uint64_t head = head_.load(std::memory_order_relaxed);
    if (head == cached_tail_) {
      cached_tail_ = tail_.load(std::memory_order_acquire);
      if (head == cached_tail_) return false;  // genuinely empty
    }
    out = buf_[head & mask_];
    head_.store(head + 1, std::memory_order_release);  // slot may be reused
    return true;
  }

  // Pop up to `max` elements in one go. Amortizes the head_ publish and the
  // cross-core tail_ read across a whole batch; this is what the matching
  // thread uses under load.
  std::size_t try_pop_batch(T* out, std::size_t max) noexcept {
    const std::uint64_t head = head_.load(std::memory_order_relaxed);
    if (head == cached_tail_) {
      cached_tail_ = tail_.load(std::memory_order_acquire);
      if (head == cached_tail_) return 0;
    }
    const std::size_t n =
        static_cast<std::size_t>(std::min<std::uint64_t>(max, cached_tail_ - head));
    for (std::size_t i = 0; i < n; ++i) out[i] = buf_[(head + i) & mask_];
    head_.store(head + n, std::memory_order_release);
    return n;
  }

  // ---- introspection (safe from either thread, values are snapshots) ------
  std::size_t capacity() const noexcept { return capacity_; }

  bool empty() const noexcept {
    return head_.load(std::memory_order_acquire) ==
           tail_.load(std::memory_order_acquire);
  }

  std::size_t size_approx() const noexcept {
    return static_cast<std::size_t>(tail_.load(std::memory_order_acquire) -
                                    head_.load(std::memory_order_acquire));
  }

 private:
  [[noreturn]] static void throw_bad_capacity();

  // Producer-owned line: index the producer bumps + its stale view of head.
  alignas(kCacheLine) std::atomic<std::uint64_t> tail_{0};
  std::uint64_t cached_head_{0};

  // Consumer-owned line.
  alignas(kCacheLine) std::atomic<std::uint64_t> head_{0};
  std::uint64_t cached_tail_{0};

  // Shared read-mostly line.
  alignas(kCacheLine) std::size_t capacity_;
  std::size_t mask_;
  T* buf_;
};

template <typename T>
[[noreturn]] void SpscRing<T>::throw_bad_capacity() {
  throw std::bad_alloc();  // capacity misuse is a programming error at startup
}

}  // namespace lob
