#pragma once
// ----------------------------------------------------------------------------
// flat_map.hpp -- open-addressing hash map: u64 key -> V.
//
// Purpose: OrderId -> Order* lookup for cancels/replaces. Requirements that
// std::unordered_map cannot meet on the hot path:
//   * zero heap allocation after construction (no node allocs, no rehash),
//   * predictable probe behaviour over billions of insert/erase cycles.
//
// Implementation: linear probing over a flat, power-of-two slot array with
// *backward-shift deletion* instead of tombstones. Tombstones would slowly
// poison probe chains in a workload that inserts and erases forever (every
// resting order eventually fills or cancels); backward shifting restores the
// table to the exact state it would have had if the erased key were never
// inserted, so performance is stable indefinitely.
//
// The table is sized at 2x max_entries (load factor <= 0.5), keys are mixed
// through the splitmix64 finalizer, and key 0 is reserved as "empty".
// Verified against std::unordered_map by a randomized differential test.
// ----------------------------------------------------------------------------
#include <cstdint>
#include <cstddef>
#include <memory>
#include <cassert>
#include <bit>
#include <algorithm>

namespace lob {

template <typename V>
class FlatMap {
 public:
  explicit FlatMap(std::size_t max_entries) : max_entries_(max_entries) {
    const std::size_t want = std::max<std::size_t>(max_entries * 2, 16);
    table_size_ = std::bit_ceil(want);
    mask_ = table_size_ - 1;
    slots_ = std::make_unique<Slot[]>(table_size_);  // value-init: keys = 0
  }

  // Returns false if the key already exists or the map is at capacity.
  bool insert(std::uint64_t key, const V& val) noexcept {
    assert(key != 0);
    if (size_ == max_entries_) return false;
    std::size_t i = home(key);
    for (;;) {
      Slot& s = slots_[i];
      if (s.key == 0) {
        s.key = key;
        s.val = val;
        ++size_;
        return true;
      }
      if (s.key == key) return false;
      i = (i + 1) & mask_;
    }
  }

  V* find(std::uint64_t key) noexcept {
    std::size_t i = home(key);
    for (;;) {
      Slot& s = slots_[i];
      if (s.key == key) return &s.val;
      if (s.key == 0) return nullptr;
      i = (i + 1) & mask_;
    }
  }

  const V* find(std::uint64_t key) const noexcept {
    return const_cast<FlatMap*>(this)->find(key);
  }

  // Backward-shift deletion (standard open-addressing erase):
  // after removing slot i we walk forward and pull back any element whose
  // home position permits it, leaving no hole that would break probes and
  // no tombstone that would lengthen them.
  bool erase(std::uint64_t key) noexcept {
    std::size_t i = home(key);
    for (;;) {
      Slot& s = slots_[i];
      if (s.key == 0) return false;
      if (s.key == key) break;
      i = (i + 1) & mask_;
    }
    std::size_t j = i;
    for (;;) {
      slots_[i].key = 0;
      for (;;) {
        j = (j + 1) & mask_;
        if (slots_[j].key == 0) {
          --size_;
          return true;
        }
        const std::size_t k = home(slots_[j].key);
        // slots_[j] may move into the hole at i unless its home k lies in
        // the cyclic interval (i, j] -- in that case moving it would place
        // it *before* its home and break lookups.
        const bool k_in_range = (i <= j) ? (k > i && k <= j) : (k > i || k <= j);
        if (!k_in_range) break;
      }
      slots_[i] = slots_[j];
      i = j;
    }
  }

  std::size_t size() const noexcept { return size_; }
  std::size_t capacity() const noexcept { return max_entries_; }

 private:
  struct Slot {
    std::uint64_t key;
    V val;
  };

  static std::uint64_t mix(std::uint64_t x) noexcept {  // splitmix64 finalizer
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
  }

  std::size_t home(std::uint64_t key) const noexcept {
    return static_cast<std::size_t>(mix(key)) & mask_;
  }

  std::size_t max_entries_;
  std::size_t table_size_;
  std::size_t mask_;
  std::size_t size_ = 0;
  std::unique_ptr<Slot[]> slots_;
};

}  // namespace lob
