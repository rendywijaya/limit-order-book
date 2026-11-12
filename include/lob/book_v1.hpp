// book_v1.hpp — Version 1: correct first, built from the standard containers.
//
//   bids_  : std::map<Price, Level, greater>   best bid  = bids_.begin()
//   asks_  : std::map<Price, Level, less>      best ask  = asks_.begin()
//   Level  : std::list<Order> in arrival order (time priority) + total qty
//   index_ : std::unordered_map<OrderId, Loc>  O(1) average cancel / modify
//
// Every structure here allocates a node per element and chases pointers to
// walk it. That is what the profile of this version shows, and what version 2
// removes (docs/DESIGN.md). The semantics are specified in docs/SPEC.md and
// are identical in all three books; the tests hold them to it.
#pragma once

#include <algorithm>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "lob/events.hpp"
#include "lob/types.hpp"

namespace lob {

template <Listener L>
class BookV1 {
public:
    explicit BookV1(L& listener, BookConfig cfg = {}) : listener_(listener), cfg_(cfg) {
        index_.reserve(cfg.expected_orders);
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

        Qty left = side == Side::Buy ? match(asks_, id, price, market, qty, std::less_equal<Price>{})
                                     : match(bids_, id, price, market, qty, std::greater_equal<Price>{});
        if (left == 0) return;
        if (type == OrderType::Limit) rest(id, side, price, left);
        else listener_.on_cancel(id, left, CancelReason::Unfilled);
    }

    void cancel(OrderId id) {
        auto it = index_.find(id);
        if (it == index_.end()) return listener_.on_reject(id, RejectReason::UnknownOrder);
        Qty left = it->second.it->qty;
        unlink(it->second);
        index_.erase(it);
        listener_.on_cancel(id, left, CancelReason::User);
    }

    void modify(OrderId id, Price price, Qty qty) {
        auto it = index_.find(id);
        if (it == index_.end()) return listener_.on_reject(id, RejectReason::UnknownOrder);
        if (qty == 0) return listener_.on_reject(id, RejectReason::InvalidQty);
        if (!price_ok(price)) return listener_.on_reject(id, RejectReason::InvalidPrice);

        Loc loc = it->second;
        // Same price and not bigger: shrink in place and keep time priority.
        if (price == loc.price && qty <= loc.it->qty) {
            level_of(loc).total -= loc.it->qty - qty;
            loc.it->qty = qty;
            return listener_.on_modify(id, price, qty);
        }
        // Otherwise the order loses its place: take it out and send it back in
        // as a new limit order, which may trade on the way.
        unlink(loc);
        index_.erase(it);
        listener_.on_modify(id, price, qty);
        Qty left = loc.side == Side::Buy ? match(asks_, id, price, false, qty, std::less_equal<Price>{})
                                         : match(bids_, id, price, false, qty, std::greater_equal<Price>{});
        if (left) rest(id, loc.side, price, left);
    }

    // ---- read-only views (tests, depth snapshots; not on the hot path) ----
    std::optional<Price> best_bid() const {
        return bids_.empty() ? std::nullopt : std::optional(bids_.begin()->first);
    }
    std::optional<Price> best_ask() const {
        return asks_.empty() ? std::nullopt : std::optional(asks_.begin()->first);
    }
    std::vector<LevelView> depth(Side side, std::size_t n = SIZE_MAX) const {
        return side == Side::Buy ? depth_of(bids_, n) : depth_of(asks_, n);
    }
    std::size_t order_count() const { return index_.size(); }
    bool contains(OrderId id) const { return index_.contains(id); }

private:
    struct Order {
        OrderId id;
        Qty     qty;
    };
    struct Level {
        std::list<Order> orders;
        QtySum           total = 0;
    };
    using Bids = std::map<Price, Level, std::greater<Price>>;
    using Asks = std::map<Price, Level, std::less<Price>>;
    struct Loc {
        Side                       side;
        Price                      price;
        typename std::list<Order>::iterator it;
    };

    bool price_ok(Price p) const { return p >= cfg_.min_price && p <= cfg_.max_price; }

    // Walk the opposite side from its best level while the price crosses.
    // `crosses(level_px, limit)` is <= for a buy taker and >= for a sell taker.
    template <class Map, class Cmp>
    Qty match(Map& book, OrderId taker, Price limit, bool market, Qty qty, Cmp crosses) {
        while (qty && !book.empty()) {
            auto lvl_it = book.begin();
            if (!market && !crosses(lvl_it->first, limit)) break;
            Level& lvl = lvl_it->second;
            while (qty && !lvl.orders.empty()) {
                Order& m = lvl.orders.front();
                Qty fill = std::min(qty, m.qty);
                qty -= fill;
                m.qty -= fill;
                lvl.total -= fill;
                listener_.on_trade({taker, m.id, lvl_it->first, fill, m.qty});
                if (m.qty == 0) {
                    index_.erase(m.id);
                    lvl.orders.pop_front();
                }
            }
            if (lvl.orders.empty()) book.erase(lvl_it);
        }
        return qty;
    }

    bool can_fill(Side side, Price limit, Qty qty) const {
        auto sum = [&](const auto& book, auto crosses) {
            QtySum avail = 0;
            for (const auto& [px, lvl] : book) {
                if (!crosses(px, limit)) break;
                avail += lvl.total;
                if (avail >= qty) return true;
            }
            return false;
        };
        return side == Side::Buy ? sum(asks_, std::less_equal<Price>{})
                                 : sum(bids_, std::greater_equal<Price>{});
    }

    void rest(OrderId id, Side side, Price price, Qty qty) {
        Level& lvl = side == Side::Buy ? bids_[price] : asks_[price];
        lvl.orders.push_back({id, qty});
        lvl.total += qty;
        index_.emplace(id, Loc{side, price, std::prev(lvl.orders.end())});
    }

    Level& level_of(const Loc& loc) {
        return loc.side == Side::Buy ? bids_.find(loc.price)->second : asks_.find(loc.price)->second;
    }

    void unlink(const Loc& loc) {
        auto drop = [&](auto& book) {
            auto lvl_it = book.find(loc.price);
            lvl_it->second.total -= loc.it->qty;
            lvl_it->second.orders.erase(loc.it);
            if (lvl_it->second.orders.empty()) book.erase(lvl_it);
        };
        loc.side == Side::Buy ? drop(bids_) : drop(asks_);
    }

    template <class Map>
    static std::vector<LevelView> depth_of(const Map& book, std::size_t n) {
        std::vector<LevelView> out;
        for (const auto& [px, lvl] : book) {
            if (out.size() == n) break;
            out.push_back({px, lvl.total, static_cast<std::uint32_t>(lvl.orders.size())});
        }
        return out;
    }

    L&                                listener_;
    BookConfig                        cfg_;
    Bids                              bids_;
    Asks                              asks_;
    std::unordered_map<OrderId, Loc>  index_;
};

}  // namespace lob
