// histogram.hpp — a latency histogram that records without disturbing.
//
// Recording a sample must cost a few nanoseconds and never allocate, or the
// measurement changes what it measures. This histogram has 64 x 16 fixed
// buckets, log-linear: the exponent (position of the highest set bit) picks
// the row, the next four bits pick the column. Resolution is better than 7 %
// at every magnitude from 1 ns to centuries, 8 KB of memory, and record() is
// one count-leading-zeros, two shifts and an increment. Percentiles are
// computed on demand by walking the table, which is cheap and off the hot
// path. The same shape as HdrHistogram's, without the library.
#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>

namespace lob {

class Histogram {
public:
    static constexpr int kRows = 64, kCols = 16;

    void record(std::uint64_t v) noexcept {
        if (v == 0) v = 1;
        const int hi = 63 - std::countl_zero(v);                 // exponent
        const int col = hi >= 4 ? int((v >> (hi - 4)) & 15) : int((v << (4 - hi)) & 15);
        ++bins_[hi][col];
        ++count_;
        sum_ += v;
        if (v > max_) max_ = v;
        if (v < min_) min_ = v;
    }

    std::uint64_t count() const noexcept { return count_; }
    std::uint64_t max() const noexcept { return count_ ? max_ : 0; }
    std::uint64_t min() const noexcept { return count_ ? min_ : 0; }
    double        mean() const noexcept { return count_ ? double(sum_) / double(count_) : 0.0; }

    // Lower bound of the bucket containing the q-quantile (0 <= q <= 1).
    std::uint64_t percentile(double q) const noexcept {
        if (!count_) return 0;
        std::uint64_t target = std::uint64_t(q * double(count_));
        if (target >= count_) target = count_ - 1;
        std::uint64_t seen = 0;
        for (int r = 0; r < kRows; ++r)
            for (int c = 0; c < kCols; ++c) {
                seen += bins_[r][c];
                if (seen > target) return lower(r, c);
            }
        return max_;
    }

    void merge(const Histogram& o) noexcept {
        for (int r = 0; r < kRows; ++r) for (int c = 0; c < kCols; ++c) bins_[r][c] += o.bins_[r][c];
        count_ += o.count_; sum_ += o.sum_;
        max_ = std::max(max_, o.max_); min_ = std::min(min_, o.min_);
    }
    void clear() noexcept { *this = Histogram{}; }

private:
    static std::uint64_t lower(int r, int c) noexcept {
        if (r < 4) return (std::uint64_t(16 | c)) >> (4 - r);  // small values: exact-ish
        return (std::uint64_t(16 | c)) << (r - 4);
    }
    std::uint64_t bins_[kRows][kCols] = {};
    std::uint64_t count_ = 0, sum_ = 0, max_ = 0, min_ = UINT64_MAX;
};

}  // namespace lob
