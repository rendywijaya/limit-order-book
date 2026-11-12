// ouch.hpp — Nasdaq OUCH 4.2, the order-entry protocol, byte for byte.
//
// OUCH is what a trading system sends to the exchange to enter, replace and
// cancel orders, and what the exchange sends back: accepted, replaced,
// canceled, executed, rejected. Every message is a fixed layout, big-endian,
// with orders identified by a 14-byte client-chosen token that must be unique
// for the day. OUCH travels inside SoupBinTCP (soupbin.hpp), which gives it a
// session, sequence numbers and heartbeats.
//
// Prices have four implied decimals; 0x7FFFFFFF means "market". Time in
// force is in seconds: 0 = immediate or cancel, 99998 = until market close,
// 99999 = until the end of the system's day. Timestamps on outbound messages
// are nanoseconds since midnight.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "lob/wire.hpp"

namespace lob::ouch {

constexpr std::size_t kTokenLen = 14;
constexpr std::uint32_t kMarketPrice = 0x7FFFFFFF;
constexpr std::uint32_t kTifIoc = 0, kTifDay = 99998, kTifSystem = 99999;

struct Token {
    std::uint8_t b[kTokenLen];
    bool operator==(const Token& o) const noexcept { return std::memcmp(b, o.b, kTokenLen) == 0; }
};
// Tokens are built from a counter so the gateway never allocates: 14 ASCII
// digits, zero padded, which is also easy to read in a packet dump.
inline Token make_token(std::uint64_t n) noexcept {
    Token t;
    for (std::size_t i = kTokenLen; i-- > 0;) { t.b[i] = std::uint8_t('0' + n % 10); n /= 10; }
    return t;
}
inline std::uint64_t token_value(const Token& t) noexcept { return wire::ascii_num(t.b, kTokenLen); }

// ---- inbound to the exchange ------------------------------------------------

enum InType : char { EnterOrder = 'O', ReplaceOrder = 'U', CancelOrder = 'X' };

struct Enter {   // 49 bytes
    Token         token;
    char          side;        // 'B' buy, 'S' sell, 'T' sell short, 'E' sell short exempt
    std::uint32_t shares;
    std::uint8_t  stock[8];
    std::uint32_t price;       // 4 decimals
    std::uint32_t tif;
    std::uint8_t  firm[4];
    char          display;     // 'Y' displayed, 'N' hidden, 'P' post-only, ...
    char          capacity;    // 'A' agency, 'P' principal, 'R' riskless
    char          iso;         // intermarket sweep eligibility 'Y'/'N'
    std::uint32_t min_qty;
    char          cross_type;  // 'N' none
    char          customer;    // 'R' retail, 'N' non-retail
};
struct Replace {  // 47 bytes
    Token         token;       // existing
    Token         new_token;   // replacement
    std::uint32_t shares;
    std::uint32_t price;
    std::uint32_t tif;
    char          display;
    char          iso;
    std::uint32_t min_qty;
};
struct Cancel {   // 19 bytes
    Token         token;
    std::uint32_t shares;      // new intended total; 0 cancels everything
};

constexpr std::size_t kEnterLen = 49, kReplaceLen = 47, kCancelLen = 19;

inline std::size_t encode(std::uint8_t* p, const Enter& m) noexcept {
    p[0] = 'O';
    std::memcpy(p + 1, m.token.b, 14);
    p[15] = std::uint8_t(m.side);
    wire::put32(p + 16, m.shares);
    std::memcpy(p + 20, m.stock, 8);
    wire::put32(p + 28, m.price);
    wire::put32(p + 32, m.tif);
    std::memcpy(p + 36, m.firm, 4);
    p[40] = std::uint8_t(m.display);
    p[41] = std::uint8_t(m.capacity);
    p[42] = std::uint8_t(m.iso);
    wire::put32(p + 43, m.min_qty);
    p[47] = std::uint8_t(m.cross_type);
    p[48] = std::uint8_t(m.customer);
    return kEnterLen;
}
inline std::size_t encode(std::uint8_t* p, const Replace& m) noexcept {
    p[0] = 'U';
    std::memcpy(p + 1, m.token.b, 14);
    std::memcpy(p + 15, m.new_token.b, 14);
    wire::put32(p + 29, m.shares);
    wire::put32(p + 33, m.price);
    wire::put32(p + 37, m.tif);
    p[41] = std::uint8_t(m.display);
    p[42] = std::uint8_t(m.iso);
    wire::put32(p + 43, m.min_qty);
    return kReplaceLen;
}
inline std::size_t encode(std::uint8_t* p, const Cancel& m) noexcept {
    p[0] = 'X';
    std::memcpy(p + 1, m.token.b, 14);
    wire::put32(p + 15, m.shares);
    return kCancelLen;
}

inline bool decode(wire::Bytes b, Enter& m) noexcept {
    if (b.size() < kEnterLen || b[0] != 'O') return false;
    const std::uint8_t* p = b.data();
    std::memcpy(m.token.b, p + 1, 14);
    m.side = char(p[15]);
    m.shares = wire::be32(p + 16);
    std::memcpy(m.stock, p + 20, 8);
    m.price = wire::be32(p + 28);
    m.tif = wire::be32(p + 32);
    std::memcpy(m.firm, p + 36, 4);
    m.display = char(p[40]); m.capacity = char(p[41]); m.iso = char(p[42]);
    m.min_qty = wire::be32(p + 43);
    m.cross_type = char(p[47]); m.customer = char(p[48]);
    return true;
}
inline bool decode(wire::Bytes b, Replace& m) noexcept {
    if (b.size() < kReplaceLen || b[0] != 'U') return false;
    const std::uint8_t* p = b.data();
    std::memcpy(m.token.b, p + 1, 14);
    std::memcpy(m.new_token.b, p + 15, 14);
    m.shares = wire::be32(p + 29);
    m.price = wire::be32(p + 33);
    m.tif = wire::be32(p + 37);
    m.display = char(p[41]); m.iso = char(p[42]);
    m.min_qty = wire::be32(p + 43);
    return true;
}
inline bool decode(wire::Bytes b, Cancel& m) noexcept {
    if (b.size() < kCancelLen || b[0] != 'X') return false;
    std::memcpy(m.token.b, b.data() + 1, 14);
    m.shares = wire::be32(b.data() + 15);
    return true;
}

// ---- outbound from the exchange ---------------------------------------------

enum OutType : char {
    SystemEvent = 'S', Accepted = 'A', Replaced = 'U', Canceled = 'C', Executed = 'E', Rejected = 'J',
    CancelPending = 'P', CancelReject = 'I', OrderPriorityUpdate = 'T', BrokenTrade = 'B'
};

constexpr std::size_t out_length_of(char type) noexcept {
    switch (type) {
        case 'S': return 10; case 'A': return 66; case 'U': return 80; case 'C': return 28; case 'E': return 40;
        case 'J': return 24; case 'P': return 23; case 'I': return 23; case 'T': return 36; case 'B': return 32;
        default: return 0;
    }
}

// One decoded outbound message, fields valid according to `type`.
struct Out {
    char          type = 0;
    std::uint64_t ns = 0;
    Token         token{};
    Token         prev_token{};   // Replaced: the token being replaced
    char          side = 0;
    std::uint32_t shares = 0;     // Accepted/Replaced: resting; Canceled: decrement; Executed: filled
    std::uint8_t  stock[8] = {};
    std::uint32_t price = 0;      // Accepted/Replaced: order price; Executed: execution price
    std::uint32_t tif = 0;
    std::uint64_t order_ref = 0;  // the exchange's reference, as seen on ITCH
    std::uint64_t match = 0;
    char          state = 0;      // Accepted/Replaced: 'L' live, 'D' dead
    char          reason = 0;     // Canceled: why; Rejected: why; Executed: liquidity flag
    char          event = 0;      // SystemEvent code
};

inline bool decode_out(wire::Bytes b, Out& m) noexcept {
    if (b.empty()) return false;
    const char type = char(b[0]);
    const std::size_t need = out_length_of(type);
    if (need == 0 || b.size() < need) return false;
    const std::uint8_t* p = b.data();
    m.type = type;
    m.ns = wire::be64(p + 1);
    switch (type) {
        case SystemEvent: m.event = char(p[9]); break;
        case Accepted:
            std::memcpy(m.token.b, p + 9, 14);
            m.side = char(p[23]); m.shares = wire::be32(p + 24);
            std::memcpy(m.stock, p + 28, 8);
            m.price = wire::be32(p + 36); m.tif = wire::be32(p + 40);
            m.order_ref = wire::be64(p + 49);
            m.state = char(p[64]);
            break;
        case Replaced:
            std::memcpy(m.token.b, p + 9, 14);
            m.side = char(p[23]); m.shares = wire::be32(p + 24);
            std::memcpy(m.stock, p + 28, 8);
            m.price = wire::be32(p + 36); m.tif = wire::be32(p + 40);
            m.order_ref = wire::be64(p + 49);
            m.state = char(p[64]);
            std::memcpy(m.prev_token.b, p + 65, 14);
            break;
        case Canceled:
            std::memcpy(m.token.b, p + 9, 14);
            m.shares = wire::be32(p + 23);
            m.reason = char(p[27]);
            break;
        case Executed:
            std::memcpy(m.token.b, p + 9, 14);
            m.shares = wire::be32(p + 23);
            m.price = wire::be32(p + 27);
            m.reason = char(p[31]);  // liquidity flag
            m.match = wire::be64(p + 32);
            break;
        case Rejected:
            std::memcpy(m.token.b, p + 9, 14);
            m.reason = char(p[23]);
            break;
        case CancelPending:
        case CancelReject:
            std::memcpy(m.token.b, p + 9, 14);
            break;
        default: break;
    }
    return true;
}

// Encoders for the simulated exchange and the tests.
inline std::size_t encode_accepted(std::uint8_t* p, std::uint64_t ns, const Token& tok, char side, std::uint32_t shares,
                                   const std::uint8_t* stock, std::uint32_t price, std::uint32_t tif,
                                   std::uint64_t order_ref, char state) noexcept {
    std::memset(p, 0, 66);
    p[0] = 'A'; wire::put64(p + 1, ns);
    std::memcpy(p + 9, tok.b, 14);
    p[23] = std::uint8_t(side); wire::put32(p + 24, shares);
    std::memcpy(p + 28, stock, 8);
    wire::put32(p + 36, price); wire::put32(p + 40, tif);
    wire::put_alpha(p + 44, 4, "FIRM");
    p[48] = 'Y';                       // display
    wire::put64(p + 49, order_ref);
    p[57] = 'P';                       // capacity
    p[58] = 'N';                       // iso
    wire::put32(p + 59, 0);            // min qty
    p[63] = 'N';                       // cross type
    p[64] = std::uint8_t(state);
    p[65] = ' ';                       // BBO weight indicator
    return 66;
}
inline std::size_t encode_replaced(std::uint8_t* p, std::uint64_t ns, const Token& new_tok, char side, std::uint32_t shares,
                                   const std::uint8_t* stock, std::uint32_t price, std::uint32_t tif, std::uint64_t order_ref,
                                   char state, const Token& prev) noexcept {
    encode_accepted(p, ns, new_tok, side, shares, stock, price, tif, order_ref, state);
    p[0] = 'U';
    std::memcpy(p + 65, prev.b, 14);
    p[79] = ' ';
    return 80;
}
inline std::size_t encode_canceled(std::uint8_t* p, std::uint64_t ns, const Token& tok, std::uint32_t dec, char reason) noexcept {
    p[0] = 'C'; wire::put64(p + 1, ns);
    std::memcpy(p + 9, tok.b, 14);
    wire::put32(p + 23, dec);
    p[27] = std::uint8_t(reason);
    return 28;
}
inline std::size_t encode_executed(std::uint8_t* p, std::uint64_t ns, const Token& tok, std::uint32_t shares, std::uint32_t price,
                                   char liquidity, std::uint64_t match) noexcept {
    p[0] = 'E'; wire::put64(p + 1, ns);
    std::memcpy(p + 9, tok.b, 14);
    wire::put32(p + 23, shares);
    wire::put32(p + 27, price);
    p[31] = std::uint8_t(liquidity);
    wire::put64(p + 32, match);
    return 40;
}
inline std::size_t encode_rejected(std::uint8_t* p, std::uint64_t ns, const Token& tok, char reason) noexcept {
    p[0] = 'J'; wire::put64(p + 1, ns);
    std::memcpy(p + 9, tok.b, 14);
    p[23] = std::uint8_t(reason);
    return 24;
}
inline std::size_t encode_cancel_reject(std::uint8_t* p, std::uint64_t ns, const Token& tok) noexcept {
    p[0] = 'I'; wire::put64(p + 1, ns);
    std::memcpy(p + 9, tok.b, 14);
    return 23;
}
inline std::size_t encode_system_event(std::uint8_t* p, std::uint64_t ns, char code) noexcept {
    p[0] = 'S'; wire::put64(p + 1, ns);
    p[9] = std::uint8_t(code);
    return 10;
}

}  // namespace lob::ouch
