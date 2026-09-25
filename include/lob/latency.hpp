#pragma once
// ----------------------------------------------------------------------------
// latency.hpp -- HDR-style log-linear histogram for nanosecond latencies.
//
// record() is O(1), allocation-free and branch-light: compute a bucket index
// from the value's magnitude (power of two) plus 6 linear bits of precision,
// then bump a counter. Relative quantization error is <= 1/64 (~1.6%),
// which is far below run-to-run noise for this kind of measurement.
//
// Values 0..63 get exact buckets; beyond that each power-of-two decade is
// split into 64 linear sub-buckets, out to ~2^43 ns (~2.4 hours), after
// which values clamp into the final bucket (the true max is tracked exactly).
//
// This is the instrument behind the per-message p50/p99 numbers: the bench
// harness keeps one histogram for engine processing time and one for
// end-to-end (enqueue -> processed) time, resets both after warmup, and can
// dump every bucket to CSV so results are auditable.
// ----------------------------------------------------------------------------
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <bit>
#include <cmath>
#include <cstring>

namespace lob {

class Histogram {
 public:
  static constexpr unsigned kLinearBits = 6;   // 64 sub-buckets per decade
  static constexpr unsigned kMaxExp = 42;      // top decade: [2^42, 2^43)
  static constexpr std::size_t kBuckets = (kMaxExp - kLinearBits + 2) * 64;

  Histogram() { reset(); }

  void reset() noexcept {
    std::memset(counts_, 0, sizeof(counts_));
    count_ = 0;
    sum_ = 0;
    max_ = 0;
    min_ = UINT64_MAX;
  }

  void record(std::uint64_t v) noexcept {
    ++count_;
    sum_ += v;
    if (v > max_) max_ = v;
    if (v < min_) min_ = v;
    ++counts_[index_of(v)];
  }

  static std::size_t index_of(std::uint64_t v) noexcept {
    if (v < 64) return static_cast<std::size_t>(v);
    const unsigned exp = 63u - static_cast<unsigned>(std::countl_zero(v));  // >= 6
    if (exp > kMaxExp) return kBuckets - 1;
    const std::uint64_t sub = (v >> (exp - kLinearBits)) & 63u;
    return static_cast<std::size_t>(exp - (kLinearBits - 1)) * 64 +
           static_cast<std::size_t>(sub);
  }

  // Inclusive upper edge of a bucket -- percentiles are reported
  // conservatively (they can only overstate, never understate, latency).
  static std::uint64_t bucket_high(std::size_t idx) noexcept {
    if (idx < 64) return idx;
    const unsigned exp = static_cast<unsigned>(idx / 64) + (kLinearBits - 1);
    const std::uint64_t sub = idx % 64;
    const std::uint64_t low = (1ull << exp) | (sub << (exp - kLinearBits));
    return low + (1ull << (exp - kLinearBits)) - 1;
  }

  std::uint64_t percentile(double p) const noexcept {
    if (count_ == 0) return 0;
    if (p <= 0.0) return min_;
    std::uint64_t target =
        static_cast<std::uint64_t>(std::ceil((p / 100.0) * double(count_)));
    if (target == 0) target = 1;
    if (target > count_) target = count_;
    std::uint64_t cum = 0;
    for (std::size_t i = 0; i < kBuckets; ++i) {
      cum += counts_[i];
      if (cum >= target) {
        const std::uint64_t hi = bucket_high(i);
        return hi < max_ ? hi : max_;
      }
    }
    return max_;
  }

  std::uint64_t count() const noexcept { return count_; }
  std::uint64_t max() const noexcept { return count_ ? max_ : 0; }
  std::uint64_t min() const noexcept { return count_ ? min_ : 0; }
  double mean() const noexcept { return count_ ? double(sum_) / double(count_) : 0.0; }

  void merge(const Histogram& o) noexcept {
    for (std::size_t i = 0; i < kBuckets; ++i) counts_[i] += o.counts_[i];
    count_ += o.count_;
    sum_ += o.sum_;
    if (o.count_) {
      if (o.max_ > max_) max_ = o.max_;
      if (o.min_ < min_) min_ = o.min_;
    }
  }

  // Full-transparency dump: every non-empty bucket as CSV.
  void dump_csv(std::FILE* f, const char* label) const {
    std::fprintf(f, "label,bucket_high_ns,count\n");
    for (std::size_t i = 0; i < kBuckets; ++i) {
      if (counts_[i]) {
        std::fprintf(f, "%s,%llu,%llu\n", label,
                     static_cast<unsigned long long>(bucket_high(i)),
                     static_cast<unsigned long long>(counts_[i]));
      }
    }
  }

 private:
  std::uint64_t counts_[kBuckets];
  std::uint64_t count_;
  std::uint64_t sum_;
  std::uint64_t max_;
  std::uint64_t min_;
};

}  // namespace lob
