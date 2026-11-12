// market_book.hpp — the other kind of book: the exchange's, rebuilt from its feed.
//
// The matching engine (book_v2.hpp) decides trades. This book decides nothing:
// it mirrors what the exchange's feed says its book looks like, one symbol per
// MarketBook, so a strategy can read the best bid and ask without ever
// talking to the exchange. It is the structure every feed handler keeps.
//
// It reuses version 2's data structures for the same reasons they were chosen
// there: price levels in an array indexed by (price - base), a bitmap to find
// the next best level, and no allocation after construction. Orders are not
// kept here (the feed handler keeps them in one table keyed by the exchange's
// reference number, see feed.hpp); the book only aggregates quantity and order
// count per level, which is exactly what a depth feed shows.
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "lob/events.hpp"
#include "lob/level_bitmap.hpp"
#include "lob/types.hpp"

namespace lob {

// The two numbers a strategy reads most, plus a version that changes
// whenever either side's best price or size does.
struct TopOfBook {
    Price         bid = 0, ask = 0;
    QtySum        bid_qty = 0, ask_qty = 0;
    std::uint64_t version = 0;
    bool          has_bid = false, has_ask = false;
    bool two_sided() const noexcept { return has_bid && has_ask; }
    // Mid in half-ticks, so it is exact: (bid + ask).
    Price mid2() const noexcept { return bid + ask; }
    Price spread() const noexcept { return ask - bid; }
};

class MarketBook {
public:
    // Prices are tracked in a window of `levels` ticks starting at `base`.
    // Orders priced outside the window are counted but not aggregated, so the
    // window should be wide enough for the day's range (the feed handler
    // centres it on the first price it sees for the symbol).
    MarketBook(Price base, std::uint32_t levels)
        : base_(base), n_(levels), lv_{std::vector<Level>(levels), std::vector<Level>(levels)},
          bits_{LevelBitmap(levels), LevelBitmap(levels)} {}

    MarketBook(MarketBook&&) noexcept = default;

    bool in_window(Price p) const noexcept { return p >= base_ && p < base_ + Price(n_); }

    // Apply a delta: an order added (qty > 0, count +1), reduced (count 0),
    // or removed (count -1). Returns true if the top of book changed.
    bool add(Side s, Price p, Qty q) noexcept {
        if (!in_window(p)) { ++outside_; return false; }
        const int si = s == Side::Buy ? 0 : 1;
        const std::size_t i = idx(p);
        Level& lv = lv_[si][i];
        if (lv.count == 0) {
            bits_[si].set(i);
            if (best_[si] == LevelBitmap::kNone || (s == Side::Buy ? std::int64_t(i) > best_[si] : std::int64_t(i) < best_[si]))
                best_[si] = std::int64_t(i);
        }
        lv.total += q;
        ++lv.count;
        return touched(si, best_[si] == std::int64_t(i));
    }
    bool reduce(Side s, Price p, Qty q) noexcept {
        if (!in_window(p)) return false;
        const int si = s == Side::Buy ? 0 : 1;
        const std::size_t i = idx(p);
        Level& lv = lv_[si][i];
        lv.total -= q;
        return touched(si, best_[si] == std::int64_t(i));
    }
    bool remove(Side s, Price p, Qty remaining) noexcept {
        if (!in_window(p)) return false;
        const int si = s == Side::Buy ? 0 : 1;
        const std::size_t i = idx(p);
        Level& lv = lv_[si][i];
        // Decide whether the top is affected BEFORE moving best_: once the
        // best level empties, best_ points at the next one, and the top must
        // be recomputed from there.
        const bool was_best = best_[si] == std::int64_t(i);
        lv.total -= remaining;
        --lv.count;
        if (lv.count == 0) {
            lv.total = 0;
            bits_[si].clear(i);
            if (was_best)
                best_[si] = s == Side::Buy ? bits_[si].prev_le(std::int64_t(i) - 1) : bits_[si].next_ge(std::int64_t(i) + 1);
        }
        return touched(si, was_best);
    }

    const TopOfBook& top() const noexcept { return top_; }
    std::optional<Price> best_bid() const noexcept { return top_.has_bid ? std::optional(top_.bid) : std::nullopt; }
    std::optional<Price> best_ask() const noexcept { return top_.has_ask ? std::optional(top_.ask) : std::nullopt; }

    std::vector<LevelView> depth(Side s, std::size_t n = SIZE_MAX) const {
        std::vector<LevelView> out;
        const int si = s == Side::Buy ? 0 : 1;
        for (std::int64_t i = best_[si]; i != LevelBitmap::kNone && out.size() < n;
             i = s == Side::Buy ? bits_[si].prev_le(i - 1) : bits_[si].next_ge(i + 1)) {
            const Level& lv = lv_[si][std::size_t(i)];
            out.push_back({px(i), lv.total, lv.count});
        }
        return out;
    }
    // Up to n best levels into a caller-provided array, no allocation.
    std::size_t levels(Side s, LevelView* out, std::size_t n) const noexcept {
        std::size_t k = 0;
        const int si = s == Side::Buy ? 0 : 1;
        for (std::int64_t i = best_[si]; i != LevelBitmap::kNone && k < n;
             i = s == Side::Buy ? bits_[si].prev_le(i - 1) : bits_[si].next_ge(i + 1)) {
            const Level& lv = lv_[si][std::size_t(i)];
            out[k++] = {px(i), lv.total, lv.count};
        }
        return k;
    }
    // Quantity resting at one price (0 if none, or outside the window).
    QtySum qty_at(Side s, Price p) const noexcept {
        return in_window(p) ? lv_[s == Side::Buy ? 0 : 1][idx(p)].total : 0;
    }
    std::uint64_t outside_window() const noexcept { return outside_; }
    Price base() const noexcept { return base_; }

private:
    struct Level {
        QtySum        total = 0;
        std::uint32_t count = 0;
    };
    std::size_t idx(Price p) const noexcept { return std::size_t(p - base_); }
    Price       px(std::int64_t i) const noexcept { return base_ + Price(i); }

    // Recompute one side of the top from best_[si]; returns whether it changed.
    bool touched(int si, bool affects_top) noexcept {
        if (!affects_top) return false;
        const bool had = si == 0 ? top_.has_bid : top_.has_ask;
        const Price was = si == 0 ? top_.bid : top_.ask;
        const QtySum was_q = si == 0 ? top_.bid_qty : top_.ask_qty;
        bool has = best_[si] != LevelBitmap::kNone;
        Price p = has ? px(best_[si]) : 0;
        QtySum q = has ? lv_[si][std::size_t(best_[si])].total : 0;
        if (si == 0) { top_.has_bid = has; top_.bid = p; top_.bid_qty = q; }
        else { top_.has_ask = has; top_.ask = p; top_.ask_qty = q; }
        if (has != had || p != was || q != was_q) { ++top_.version; return true; }
        return false;
    }

    Price              base_;
    std::uint32_t      n_;
    std::vector<Level> lv_[2];
    LevelBitmap        bits_[2];
    std::int64_t       best_[2] = {LevelBitmap::kNone, LevelBitmap::kNone};
    TopOfBook          top_;
    std::uint64_t      outside_ = 0;
};

// A fixed-size copy of the top of a book: what crosses from the feed thread
// to the strategy thread, and what the simulated exchange places orders
// against. Five levels is what the quoter and the queue model can use.
struct BookSnapshot {
    static constexpr std::size_t kDepth = 5;
    TopOfBook   top;
    LevelView   bids[kDepth] = {};
    LevelView   asks[kDepth] = {};
    std::uint8_t nb = 0, na = 0;

    QtySum qty_at(Side s, Price p) const noexcept {
        const LevelView* lv = s == Side::Buy ? bids : asks;
        const std::uint8_t n = s == Side::Buy ? nb : na;
        for (std::uint8_t i = 0; i < n; ++i) if (lv[i].price == p) return lv[i].total_qty;
        return 0;
    }
};

inline void snapshot(const MarketBook& b, BookSnapshot& out) noexcept {
    out.top = b.top();
    out.nb = std::uint8_t(b.levels(Side::Buy, out.bids, BookSnapshot::kDepth));
    out.na = std::uint8_t(b.levels(Side::Sell, out.asks, BookSnapshot::kDepth));
}

}  // namespace lob
