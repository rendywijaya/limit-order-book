// types.hpp — the vocabulary shared by every book in this repository.
//
// Everything here is deliberately small and fixed-size. Prices are integer
// ticks, never floating point: 0.1 + 0.2 != 0.3 in binary floating point, and
// an exchange's price grid is a set of integers anyway (price = ticks * tick_size).
#pragma once

#include <cstdint>
#include <string_view>

namespace lob {

using Price   = std::int32_t;   // price in ticks
using Qty     = std::uint32_t;  // shares / contracts
using OrderId = std::uint64_t;  // assigned by the client, unique while live
using QtySum  = std::uint64_t;  // sums of quantities (a level's total) never overflow

enum class Side : std::uint8_t { Buy, Sell };

// The four order types from the spec (docs/SPEC.md).
enum class OrderType : std::uint8_t {
    Limit,   // match what crosses, rest the remainder (good-till-cancel)
    Market,  // match at any price, cancel the remainder; never rests
    IOC,     // immediate-or-cancel: like Limit, but the remainder is cancelled
    FOK,     // fill-or-kill: fill the whole quantity now, or do nothing
};

enum class RejectReason : std::uint8_t {
    DuplicateId,
    InvalidQty,
    InvalidPrice,
    UnknownOrder,
};

enum class CancelReason : std::uint8_t {
    User,          // explicit cancel request
    Unfilled,      // IOC / Market remainder that could not trade
    FokNotFilled,  // FOK that could not be filled in full
};

// One message into the engine. 24 bytes, trivially copyable, so a whole
// "trading day" of them can be stored in a flat array and replayed.
enum class MsgType : std::uint8_t { New, Cancel, Modify };

struct Command {
    MsgType   type;
    Side      side;
    OrderType otype;
    OrderId   id;
    Price     price;
    Qty       qty;
};
static_assert(sizeof(Command) == 24);

// Fixed price window that every book enforces identically. Version 2 needs a
// bounded range to index levels by array; giving version 1 and the reference
// the same rule keeps all three books in agreement on what is rejected.
struct BookConfig {
    Price min_price = 1;
    Price max_price = 1 << 16;           // inclusive
    // Orders expected to rest AT ONCE (not per day): pre-sizes the order pool
    // and the id hash map. Size it for the working set — a hash table far
    // bigger than the cache makes every new id a cache miss (BENCHMARKS.md).
    std::uint32_t expected_orders = 1 << 16;
};

constexpr Side opposite(Side s) noexcept { return s == Side::Buy ? Side::Sell : Side::Buy; }

constexpr std::string_view to_string(Side s) noexcept { return s == Side::Buy ? "Buy" : "Sell"; }
constexpr std::string_view to_string(OrderType t) noexcept {
    switch (t) {
        case OrderType::Limit: return "Limit";
        case OrderType::Market: return "Market";
        case OrderType::IOC: return "IOC";
        case OrderType::FOK: return "FOK";
    }
    return "?";
}

}  // namespace lob
