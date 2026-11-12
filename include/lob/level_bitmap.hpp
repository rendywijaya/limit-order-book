// level_bitmap.hpp — which price levels are occupied, and where the next one is.
//
// One bit per price level, plus summary layers above it: bit w of layer k+1 is
// set when word w of layer k is non-zero. Finding the next occupied level above
// (or below) a price is then a few word loads and one count-trailing-zeros (or
// count-leading-zeros) instruction per layer, instead of a walk over empty
// levels. With 3 layers the top layer has at most 64 words for 2^24 levels.
//
//   layer 2:  [....1...]                         1 word  per 262144 levels
//   layer 1:  [..1.....][........]  ...          1 word  per   4096 levels
//   layer 0:  [00010000][00000000]  ...          1 word  per     64 levels
#pragma once

#include <bit>
#include <cstdint>
#include <vector>

namespace lob {

class LevelBitmap {
public:
    static constexpr std::int64_t kNone = -1;

    explicit LevelBitmap(std::size_t nbits) {
        std::size_t n = nbits;
        for (int k = 0; k < kLayers; ++k) {
            bits_[k] = n;
            layer_[k].assign((n + 63) / 64, 0);
            n = (n + 63) / 64;
        }
    }

    void set(std::size_t i) noexcept {
        for (int k = 0; k < kLayers; ++k) {
            std::uint64_t& w = layer_[k][i >> 6];
            bool was_empty = w == 0;
            w |= 1ull << (i & 63);
            if (!was_empty) return;  // the layers above already know about this word
            i >>= 6;
        }
    }

    void clear(std::size_t i) noexcept {
        for (int k = 0; k < kLayers; ++k) {
            std::uint64_t& w = layer_[k][i >> 6];
            w &= ~(1ull << (i & 63));
            if (w != 0) return;  // word still has bits: the layers above stay set
            i >>= 6;
        }
    }

    bool test(std::size_t i) const noexcept { return layer_[0][i >> 6] >> (i & 63) & 1; }

    // Smallest set index >= i, or kNone.
    std::int64_t next_ge(std::int64_t i) const noexcept { return next_ge(0, i); }
    // Largest set index <= i, or kNone.
    std::int64_t prev_le(std::int64_t i) const noexcept { return prev_le(0, i); }

private:
    static constexpr int kLayers = 3;

    std::int64_t next_ge(int k, std::int64_t i) const noexcept {
        if (i < 0) i = 0;
        if (static_cast<std::size_t>(i) >= bits_[k]) return kNone;
        const auto& L = layer_[k];
        std::size_t w = static_cast<std::size_t>(i) >> 6;
        std::uint64_t m = L[w] & (~0ull << (i & 63));
        if (m) return static_cast<std::int64_t>((w << 6) + std::countr_zero(m));
        if (k + 1 == kLayers) {  // top layer: short linear scan
            for (++w; w < L.size(); ++w)
                if (L[w]) return static_cast<std::int64_t>((w << 6) + std::countr_zero(L[w]));
            return kNone;
        }
        std::int64_t nw = next_ge(k + 1, static_cast<std::int64_t>(w) + 1);
        if (nw == kNone) return kNone;
        return (nw << 6) + std::countr_zero(L[static_cast<std::size_t>(nw)]);
    }

    std::int64_t prev_le(int k, std::int64_t i) const noexcept {
        if (i < 0) return kNone;
        if (static_cast<std::size_t>(i) >= bits_[k]) i = static_cast<std::int64_t>(bits_[k]) - 1;
        const auto& L = layer_[k];
        std::size_t w = static_cast<std::size_t>(i) >> 6;
        std::uint64_t m = L[w] & (~0ull >> (63 - (i & 63)));
        if (m) return static_cast<std::int64_t>((w << 6) + 63 - std::countl_zero(m));
        if (w == 0) return kNone;
        if (k + 1 == kLayers) {
            while (w-- > 0)
                if (L[w]) return static_cast<std::int64_t>((w << 6) + 63 - std::countl_zero(L[w]));
            return kNone;
        }
        std::int64_t pw = prev_le(k + 1, static_cast<std::int64_t>(w) - 1);
        if (pw == kNone) return kNone;
        return (pw << 6) + 63 - std::countl_zero(L[static_cast<std::size_t>(pw)]);
    }

    std::vector<std::uint64_t> layer_[kLayers];
    std::size_t                bits_[kLayers];
};

}  // namespace lob
