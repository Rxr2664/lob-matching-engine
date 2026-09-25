#pragma once
// ----------------------------------------------------------------------------
// pool.hpp -- fixed-capacity object pool.
//
// All Order nodes are carved out of one contiguous slab that is allocated
// once at startup. alloc()/release() are O(1) index pushes/pops on a
// pre-reserved free stack, so the matching hot path performs no heap
// allocation whatsoever after construction (tests/test_no_alloc.cpp proves
// this by instrumenting global operator new).
//
// A contiguous slab also means order nodes that were allocated close in time
// tend to be close in memory, which keeps level walks cache-friendly.
// ----------------------------------------------------------------------------
#include <cstdint>
#include <cstddef>
#include <memory>
#include <vector>
#include <cassert>

namespace lob {

template <typename T>
class FixedPool {
 public:
  explicit FixedPool(std::size_t capacity) : cap_(capacity) {
    assert(capacity > 0 && capacity < UINT32_MAX);
    slab_.reset(new T[capacity]);
    free_.reserve(capacity);
    // Hand out low indices first (front of the slab) for locality.
    for (std::size_t i = capacity; i > 0; --i) {
      free_.push_back(static_cast<std::uint32_t>(i - 1));
    }
  }

  // Returns nullptr when exhausted; the engine converts that into a
  // BOOK_FULL cancel instead of ever touching the heap.
  T* alloc() noexcept {
    if (free_.empty()) return nullptr;
    const std::uint32_t idx = free_.back();
    free_.pop_back();  // never reallocates: capacity() == cap_ forever
    return &slab_[idx];
  }

  void release(T* p) noexcept {
    assert(p >= slab_.get() && p < slab_.get() + cap_);
    free_.push_back(static_cast<std::uint32_t>(p - slab_.get()));
  }

  std::size_t capacity() const noexcept { return cap_; }
  std::size_t in_use() const noexcept { return cap_ - free_.size(); }

 private:
  std::size_t cap_;
  std::unique_ptr<T[]> slab_;
  std::vector<std::uint32_t> free_;
};

}  // namespace lob
