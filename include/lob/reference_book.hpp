// reference_book.hpp — a book too simple to be wrong.
//
// One flat vector of resting orders. Every query is a linear scan: the best
// opposite order is the one with the best price, and among equal prices the
// smallest arrival sequence number. It is O(n) per message and that is the
// point — there is no clever structure in it to get wrong, so the property
// tests can hold the fast books to it after every single message.
#pragma once

#include <algorithm>
#include <map>
#include <optional>
#include <vector>

#include "lob/events.hpp"
#include "lob/types.hpp"

namespace lob {

template <Listener L>
class ReferenceBook {
public:
    explicit ReferenceBook(L& listener, BookConfig cfg = {}) : listener_(listener), cfg_(cfg) {}

    void submit(const Command& c) {
        switch (c.type) {
            case MsgType::New: add(c.id, c.side, c.otype, c.price, c.qty); break;
            case MsgType::Cancel: cancel(c.id); break;
            case MsgType::Modify: modify(c.id, c.price, c.qty); break;
        }
    }

    void add(OrderId id, Side side, OrderType type, Price price, Qty qty) {
        if (find(id)) return listener_.on_reject(id, RejectReason::DuplicateId);
        if (qty == 0) return listener_.on_reject(id, RejectReason::InvalidQty);
        if (type != OrderType::Market && !price_ok(price))
            return listener_.on_reject(id, RejectReason::InvalidPrice);
        listener_.on_accept(id);

        const bool market = type == OrderType::Market;
        if (type == OrderType::FOK) {
            QtySum avail = 0;
            for (const auto& o : orders_)
                if (o.side != side && crosses(side, price, o.price, false)) avail += o.qty;
            if (avail < qty) return listener_.on_cancel(id, qty, CancelReason::FokNotFilled);
        }
        Qty left = match(id, side, price, market, qty);
        if (left == 0) return;
        if (type == OrderType::Limit) orders_.push_back({id, side, price, left, seq_++});
        else listener_.on_cancel(id, left, CancelReason::Unfilled);
    }

    void cancel(OrderId id) {
        auto* o = find(id);
        if (!o) return listener_.on_reject(id, RejectReason::UnknownOrder);
        Qty left = o->qty;
        erase(id);
        listener_.on_cancel(id, left, CancelReason::User);
    }

    void modify(OrderId id, Price price, Qty qty) {
        auto* o = find(id);
        if (!o) return listener_.on_reject(id, RejectReason::UnknownOrder);
        if (qty == 0) return listener_.on_reject(id, RejectReason::InvalidQty);
        if (!price_ok(price)) return listener_.on_reject(id, RejectReason::InvalidPrice);
        if (price == o->price && qty <= o->qty) {
            o->qty = qty;  // keeps its sequence number, so keeps its priority
            return listener_.on_modify(id, price, qty);
        }
        Side side = o->side;
        erase(id);
        listener_.on_modify(id, price, qty);
        Qty left = match(id, side, price, false, qty);
        if (left) orders_.push_back({id, side, price, left, seq_++});
    }

    std::vector<LevelView> depth(Side side, std::size_t n = SIZE_MAX) const {
        std::map<Price, LevelView> agg;
        for (const auto& o : orders_) {
            if (o.side != side) continue;
            auto& lv = agg[o.price];
            lv.price = o.price;
            lv.total_qty += o.qty;
            lv.order_count += 1;
        }
        std::vector<LevelView> out;
        for (auto& [_, lv] : agg) out.push_back(lv);
        if (side == Side::Buy) std::reverse(out.begin(), out.end());  // best (highest) bid first
        if (out.size() > n) out.resize(n);
        return out;
    }
    std::size_t order_count() const { return orders_.size(); }
    bool contains(OrderId id) { return find(id) != nullptr; }

private:
    struct Resting {
        OrderId       id;
        Side          side;
        Price         price;
        Qty           qty;
        std::uint64_t seq;
    };

    bool price_ok(Price p) const { return p >= cfg_.min_price && p <= cfg_.max_price; }

    static bool crosses(Side taker, Price limit, Price resting, bool market) {
        if (market) return true;
        return taker == Side::Buy ? resting <= limit : resting >= limit;
    }

    Resting* find(OrderId id) {
        for (auto& o : orders_)
            if (o.id == id) return &o;
        return nullptr;
    }
    void erase(OrderId id) {
        orders_.erase(std::find_if(orders_.begin(), orders_.end(), [&](auto& o) { return o.id == id; }));
    }

    Qty match(OrderId taker, Side side, Price limit, bool market, Qty qty) {
        while (qty) {
            // Find the best opposite order by brute force.
            Resting* best = nullptr;
            for (auto& o : orders_) {
                if (o.side == side || !crosses(side, limit, o.price, market)) continue;
                if (!best) { best = &o; continue; }
                bool better_px = side == Side::Buy ? o.price < best->price : o.price > best->price;
                if (better_px || (o.price == best->price && o.seq < best->seq)) best = &o;
            }
            if (!best) break;
            Qty fill = std::min(qty, best->qty);
            qty -= fill;
            best->qty -= fill;
            listener_.on_trade({taker, best->id, best->price, fill, best->qty});
            if (best->qty == 0) erase(best->id);
        }
        return qty;
    }

    L&                   listener_;
    BookConfig           cfg_;
    std::vector<Resting> orders_;
    std::uint64_t        seq_ = 0;
};

}  // namespace lob
