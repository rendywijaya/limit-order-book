// book_v2.hpp — Version 2: rewritten from the profile of version 1.
//
// Version 1 spends its time allocating nodes and chasing pointers (see
// docs/DESIGN.md for the profile). Version 2 keeps the same semantics and the
// same event stream, and changes only the data structures:
//
//   price levels  std::map<Price, Level>       ->  Level arrays indexed by (price - min)
//   best level    map.begin()                  ->  cached index + LevelBitmap search
//   orders        std::list nodes on the heap  ->  one pool (vector<Node>), linked by
//                                                  32-bit index: an intrusive list
//   id -> order   std::unordered_map           ->  FlatHashMap (open addressing)
//
// After warm-up, no message allocates: every Node comes from the pool's free
// list and every hash slot already exists.
#pragma once

#include <algorithm>
#include <cassert>
#include <optional>
#include <vector>

#include "lob/events.hpp"
#include "lob/flat_hash_map.hpp"
#include "lob/level_bitmap.hpp"
#include "lob/types.hpp"

namespace lob {

template <Listener L>
class BookV2 {
public:
    explicit BookV2(L& listener, BookConfig cfg = {})
        : listener_(listener),
          cfg_(cfg),
          nlevels_(static_cast<std::size_t>(cfg.max_price - cfg.min_price) + 1),
          levels_{std::vector<Level>(nlevels_), std::vector<Level>(nlevels_)},
          bits_{LevelBitmap(nlevels_), LevelBitmap(nlevels_)},
          index_(cfg.expected_orders) {
        // resize() writes every node, so the operating system maps the pages
        // now, at start-up, instead of on the first order that lands on them.
        // clear() keeps the capacity: the pool is empty, its memory is warm.
        pool_.resize(cfg.expected_orders);
        pool_.clear();
    }

    void submit(const Command& c) {
        switch (c.type) {
            case MsgType::New: add(c.id, c.side, c.otype, c.price, c.qty); break;
            case MsgType::Cancel: cancel(c.id); break;
            case MsgType::Modify: modify(c.id, c.price, c.qty); break;
        }
    }

    void add(OrderId id, Side side, OrderType type, Price price, Qty qty) {
        if (index_.contains(id)) return listener_.on_reject(id, RejectReason::DuplicateId);
        if (qty == 0) return listener_.on_reject(id, RejectReason::InvalidQty);
        if (type != OrderType::Market && !price_ok(price))
            return listener_.on_reject(id, RejectReason::InvalidPrice);

        listener_.on_accept(id);
        const bool market = type == OrderType::Market;

        if (type == OrderType::FOK && !can_fill(side, price, qty))
            return listener_.on_cancel(id, qty, CancelReason::FokNotFilled);

        Qty left = side == Side::Buy ? match<Side::Buy>(id, price, market, qty)
                                     : match<Side::Sell>(id, price, market, qty);
        if (left == 0) return;
        if (type == OrderType::Limit) rest(id, side, price, left);
        else listener_.on_cancel(id, left, CancelReason::Unfilled);
    }

    void cancel(OrderId id) {
        std::uint32_t n = index_.find(id);
        if (n == FlatHashMap::kEmpty) return listener_.on_reject(id, RejectReason::UnknownOrder);
        Qty left = pool_[n].qty;
        unlink(n);
        index_.erase(id);
        release(n);
        listener_.on_cancel(id, left, CancelReason::User);
    }

    void modify(OrderId id, Price price, Qty qty) {
        std::uint32_t n = index_.find(id);
        if (n == FlatHashMap::kEmpty) return listener_.on_reject(id, RejectReason::UnknownOrder);
        if (qty == 0) return listener_.on_reject(id, RejectReason::InvalidQty);
        if (!price_ok(price)) return listener_.on_reject(id, RejectReason::InvalidPrice);

        Node& o = pool_[n];
        if (price == o.price && qty <= o.qty) {  // shrink in place, keep priority
            level(o.side, idx(price)).total -= o.qty - qty;
            o.qty = qty;
            return listener_.on_modify(id, price, qty);
        }
        Side side = o.side;
        unlink(n);
        index_.erase(id);
        release(n);
        listener_.on_modify(id, price, qty);
        Qty left = side == Side::Buy ? match<Side::Buy>(id, price, false, qty)
                                     : match<Side::Sell>(id, price, false, qty);
        if (left) rest(id, side, price, left);
    }

    // ---- read-only views ----
    std::optional<Price> best_bid() const {
        return best_[0] == LevelBitmap::kNone ? std::nullopt : std::optional(px(best_[0]));
    }
    std::optional<Price> best_ask() const {
        return best_[1] == LevelBitmap::kNone ? std::nullopt : std::optional(px(best_[1]));
    }
    std::vector<LevelView> depth(Side side, std::size_t n = SIZE_MAX) const {
        std::vector<LevelView> out;
        const int s = si(side);
        for (std::int64_t i = best_[s]; i != LevelBitmap::kNone && out.size() < n;
             i = side == Side::Buy ? bits_[s].prev_le(i - 1) : bits_[s].next_ge(i + 1)) {
            const Level& lv = levels_[s][static_cast<std::size_t>(i)];
            out.push_back({px(i), lv.total, lv.count});
        }
        return out;
    }
    std::size_t order_count() const { return index_.size(); }
    bool contains(OrderId id) const { return index_.contains(id); }

private:
    static constexpr std::uint32_t kNil = UINT32_MAX;

    // 32 bytes: two orders per 64-byte cache line.
    struct Node {
        OrderId       id;
        Price         price;
        Qty           qty;
        std::uint32_t prev;
        std::uint32_t next;
        Side          side;
    };
    static_assert(sizeof(Node) == 32);

    // 24 bytes. head/tail are pool indices: an intrusive doubly linked list.
    struct Level {
        std::uint32_t head = kNil;
        std::uint32_t tail = kNil;
        std::uint32_t count = 0;
        QtySum        total = 0;
    };

    static constexpr int si(Side s) { return s == Side::Buy ? 0 : 1; }
    bool price_ok(Price p) const { return p >= cfg_.min_price && p <= cfg_.max_price; }
    std::size_t idx(Price p) const { return static_cast<std::size_t>(p - cfg_.min_price); }
    Price px(std::int64_t i) const { return cfg_.min_price + static_cast<Price>(i); }
    Level& level(Side s, std::size_t i) { return levels_[si(s)][i]; }

    // ---- order pool: a vector of nodes with a free list threaded through `next`
    std::uint32_t acquire() {
        if (free_ != kNil) {
            std::uint32_t n = free_;
            free_ = pool_[n].next;
            return n;
        }
        pool_.emplace_back();  // allocates only once the reserve is used up
        return static_cast<std::uint32_t>(pool_.size() - 1);
    }
    void release(std::uint32_t n) {
        pool_[n].next = free_;
        free_ = n;
    }

    // Match a taker on side S against the opposite side, best level first.
    template <Side S>
    Qty match(OrderId taker, Price limit, bool market, Qty qty) {
        constexpr int o = si(opposite(S));
        std::int64_t& best = best_[o];
        while (qty && best != LevelBitmap::kNone) {
            const Price lvl_px = px(best);
            if (!market) {
                if constexpr (S == Side::Buy) { if (lvl_px > limit) break; }
                else { if (lvl_px < limit) break; }
            }
            Level& lv = levels_[o][static_cast<std::size_t>(best)];
            while (qty && lv.head != kNil) {
                const std::uint32_t mi = lv.head;
                Node& m = pool_[mi];
                const Qty fill = std::min(qty, m.qty);
                qty -= fill;
                m.qty -= fill;
                lv.total -= fill;
                listener_.on_trade({taker, m.id, lvl_px, fill, m.qty});
                if (m.qty == 0) {
                    lv.head = m.next;
                    if (lv.head == kNil) lv.tail = kNil;
                    else pool_[lv.head].prev = kNil;
                    --lv.count;
                    index_.erase(m.id);
                    release(mi);
                }
            }
            if (lv.head == kNil) {  // level emptied: find the next one
                bits_[o].clear(static_cast<std::size_t>(best));
                best = S == Side::Buy ? bits_[o].next_ge(best + 1) : bits_[o].prev_le(best - 1);
            }
        }
        return qty;
    }

    bool can_fill(Side side, Price limit, Qty qty) const {
        const int o = si(opposite(side));
        QtySum avail = 0;
        for (std::int64_t i = best_[o]; i != LevelBitmap::kNone;
             i = side == Side::Buy ? bits_[o].next_ge(i + 1) : bits_[o].prev_le(i - 1)) {
            if (side == Side::Buy ? px(i) > limit : px(i) < limit) break;
            avail += levels_[o][static_cast<std::size_t>(i)].total;
            if (avail >= qty) return true;
        }
        return false;
    }

    void rest(OrderId id, Side side, Price price, Qty qty) {
        const int s = si(side);
        const std::size_t i = idx(price);
        const std::uint32_t n = acquire();
        Level& lv = levels_[s][i];
        pool_[n] = Node{id, price, qty, lv.tail, kNil, side};
        if (lv.tail == kNil) {
            lv.head = n;
            bits_[s].set(i);
            const auto ii = static_cast<std::int64_t>(i);
            if (best_[s] == LevelBitmap::kNone || (side == Side::Buy ? ii > best_[s] : ii < best_[s]))
                best_[s] = ii;
        } else {
            pool_[lv.tail].next = n;
        }
        lv.tail = n;
        ++lv.count;
        lv.total += qty;
        index_.insert(id, n);
    }

    // Remove node n from its level (does not touch the index or the pool).
    void unlink(std::uint32_t n) {
        Node& o = pool_[n];
        const int s = si(o.side);
        const std::size_t i = idx(o.price);
        Level& lv = levels_[s][i];
        if (o.prev != kNil) pool_[o.prev].next = o.next; else lv.head = o.next;
        if (o.next != kNil) pool_[o.next].prev = o.prev; else lv.tail = o.prev;
        --lv.count;
        lv.total -= o.qty;
        if (lv.head == kNil) {
            bits_[s].clear(i);
            const auto ii = static_cast<std::int64_t>(i);
            if (best_[s] == ii)
                best_[s] = o.side == Side::Buy ? bits_[s].prev_le(ii - 1) : bits_[s].next_ge(ii + 1);
        }
    }

    L&                 listener_;
    BookConfig         cfg_;
    std::size_t        nlevels_;
    std::vector<Level> levels_[2];   // [0] bids, [1] asks
    LevelBitmap        bits_[2];
    std::int64_t       best_[2] = {LevelBitmap::kNone, LevelBitmap::kNone};
    std::vector<Node>  pool_;
    std::uint32_t      free_ = kNil;
    FlatHashMap        index_;
};

}  // namespace lob
