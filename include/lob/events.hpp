// events.hpp — what the engine reports, and the listeners that receive it.
//
// The books are templates on a Listener type rather than calling through a
// virtual interface. The compiler sees the concrete listener, inlines its
// methods, and a listener that does nothing costs nothing. The C++20 concept
// below documents (and checks) the methods a listener must have.
#pragma once

#include <concepts>
#include <cstdint>
#include <ostream>
#include <vector>

#include "lob/types.hpp"

namespace lob {

struct Trade {
    OrderId taker;
    OrderId maker;
    Price   price;            // always the resting (maker) order's price
    Qty     qty;
    Qty     maker_remaining;  // maker quantity left after this fill
};

template <class L>
concept Listener = requires(L l, OrderId id, Price p, Qty q, const Trade& t) {
    { l.on_accept(id) };
    { l.on_reject(id, RejectReason::InvalidQty) };
    { l.on_trade(t) };
    { l.on_cancel(id, q, CancelReason::User) };
    { l.on_modify(id, p, q) };
};

// A price level as seen from outside: aggregated, the way a depth feed shows it.
struct LevelView {
    Price         price;
    QtySum        total_qty;
    std::uint32_t order_count;
    friend bool operator==(const LevelView&, const LevelView&) = default;
};

// ---------------------------------------------------------------------------
// Listeners

// Does nothing but count, so the benchmark measures the book and not I/O.
// The counters stop the optimiser from deleting the work as unobservable.
struct CountingListener {
    std::uint64_t accepts = 0, rejects = 0, trades = 0, traded_qty = 0, cancels = 0, modifies = 0;
    void on_accept(OrderId) noexcept { ++accepts; }
    void on_reject(OrderId, RejectReason) noexcept { ++rejects; }
    void on_trade(const Trade& t) noexcept { ++trades; traded_qty += t.qty; }
    void on_cancel(OrderId, Qty, CancelReason) noexcept { ++cancels; }
    void on_modify(OrderId, Price, Qty) noexcept { ++modifies; }
};

// Records every event in order. Used by the tests, which demand that every
// implementation produces exactly the same event stream.
struct Event {
    enum class Kind : std::uint8_t { Accept, Reject, Trade, Cancel, Modify } kind;
    OrderId id = 0;       // taker for trades
    OrderId other = 0;    // maker for trades
    Price   price = 0;
    Qty     qty = 0;
    Qty     extra = 0;    // maker_remaining for trades
    std::uint8_t reason = 0;
    friend bool operator==(const Event&, const Event&) = default;
};

inline std::ostream& operator<<(std::ostream& os, const Event& e) {
    switch (e.kind) {
        case Event::Kind::Accept: return os << "Accept{" << e.id << "}";
        case Event::Kind::Reject: return os << "Reject{" << e.id << " reason=" << int(e.reason) << "}";
        case Event::Kind::Trade:
            return os << "Trade{taker=" << e.id << " maker=" << e.other << " px=" << e.price
                      << " qty=" << e.qty << " maker_left=" << e.extra << "}";
        case Event::Kind::Cancel:
            return os << "Cancel{" << e.id << " left=" << e.qty << " reason=" << int(e.reason) << "}";
        case Event::Kind::Modify:
            return os << "Modify{" << e.id << " px=" << e.price << " qty=" << e.qty << "}";
    }
    return os;
}

struct RecordingListener {
    std::vector<Event> events;
    void on_accept(OrderId id) { events.push_back({Event::Kind::Accept, id}); }
    void on_reject(OrderId id, RejectReason r) {
        events.push_back({Event::Kind::Reject, id, 0, 0, 0, 0, std::uint8_t(r)});
    }
    void on_trade(const Trade& t) {
        events.push_back({Event::Kind::Trade, t.taker, t.maker, t.price, t.qty, t.maker_remaining});
    }
    void on_cancel(OrderId id, Qty left, CancelReason r) {
        events.push_back({Event::Kind::Cancel, id, 0, 0, left, 0, std::uint8_t(r)});
    }
    void on_modify(OrderId id, Price p, Qty q) { events.push_back({Event::Kind::Modify, id, 0, p, q}); }
};

static_assert(Listener<CountingListener>);
static_assert(Listener<RecordingListener>);

}  // namespace lob
