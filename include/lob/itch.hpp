// itch.hpp — Nasdaq TotalView-ITCH 5.0, decoded from raw bytes.
//
// ITCH is the exchange's own view of its book, message by message: every
// order added, executed, cancelled, deleted or replaced, for every stock,
// with a nanosecond timestamp. A feed handler that applies these messages
// has a copy of the exchange's book.
//
// Layout of every message: type (1 byte), stock locate (2), tracking number
// (2), timestamp (6, nanoseconds since midnight), then the fields. All
// integers big-endian. Prices are 4 bytes with four implied decimals
// (1234500 = $123.45). Order reference numbers are 8 bytes and unique for
// the day. The stock locate code is a small integer the exchange assigns per
// symbol per day, announced in the Stock Directory message; it is what a
// feed handler indexes its books by.
//
// Only the fields a book needs are decoded. Every message type in the spec
// is in the length table, so unknown-to-us messages are skipped correctly.
#pragma once

#include <cstddef>
#include <cstdint>

#include "lob/wire.hpp"

namespace lob::itch {

enum Type : char {
    SystemEvent = 'S', StockDirectory = 'R', TradingAction = 'H', RegSho = 'Y', MarketParticipant = 'L',
    MwcbDecline = 'V', MwcbStatus = 'W', IpoQuoting = 'K', LuldCollar = 'J', OperationalHalt = 'h',
    AddOrder = 'A', AddOrderMpid = 'F', Executed = 'E', ExecutedWithPrice = 'C', Cancel = 'X', Delete = 'D',
    Replace = 'U', Trade = 'P', CrossTrade = 'Q', BrokenTrade = 'B', Noii = 'I', Rpii = 'N', DirectListing = 'O',
};

// Message length by type, from the specification; 0 for an unknown type.
constexpr std::size_t length_of(char type) noexcept {
    switch (type) {
        case 'S': return 12; case 'R': return 39; case 'H': return 25; case 'Y': return 20; case 'L': return 26;
        case 'V': return 35; case 'W': return 12; case 'K': return 28; case 'J': return 35; case 'h': return 21;
        case 'A': return 36; case 'F': return 40; case 'E': return 31; case 'C': return 36; case 'X': return 23;
        case 'D': return 19; case 'U': return 35; case 'P': return 44; case 'Q': return 40; case 'B': return 19;
        case 'I': return 50; case 'N': return 20; case 'O': return 48;
        default: return 0;
    }
}

// One decoded message. A single struct rather than a variant so the hot path
// has no dispatch beyond a switch on `type`, and nothing is allocated.
struct Msg {
    char          type = 0;
    std::uint16_t locate = 0;
    std::uint16_t tracking = 0;
    std::uint64_t ns = 0;        // nanoseconds since midnight
    std::uint64_t ref = 0;       // order reference (original, for Replace)
    std::uint64_t new_ref = 0;   // Replace: the new reference
    std::uint64_t match = 0;     // Executed / Trade: match number
    std::uint32_t shares = 0;    // added / executed / cancelled / traded
    std::uint32_t price = 0;     // raw, four implied decimals
    char          side = 0;      // 'B' or 'S'
    char          event = 0;     // SystemEvent code; Cross type; Printable flag; Trading state
    std::uint8_t  stock[8] = {}; // symbol, space padded (A, F, P, Q, R, H)
    std::uint8_t  mpid[4] = {};  // AddOrderMpid attribution
};

// Decode one message. Returns false if the buffer is shorter than the type's
// length, or the type is unknown. On success, m.type says which fields mean
// anything (see the table in docs/PROTOCOLS.md).
inline bool decode(wire::Bytes b, Msg& m) noexcept {
    using namespace wire;
    if (b.empty()) return false;
    const char type = char(b[0]);
    const std::size_t need = length_of(type);
    if (need == 0 || b.size() < need) return false;
    const std::uint8_t* p = b.data();
    m.type = type;
    m.locate = be16(p + 1);
    m.tracking = be16(p + 3);
    m.ns = be48(p + 5);
    switch (type) {
        case SystemEvent:
            m.event = char(p[11]);
            break;
        case StockDirectory:
            std::memcpy(m.stock, p + 11, 8);
            m.shares = be32(p + 21);  // round lot size
            break;
        case TradingAction:
            std::memcpy(m.stock, p + 11, 8);
            m.event = char(p[19]);    // 'H' halted, 'P' paused, 'Q' quotation only, 'T' trading
            break;
        case AddOrder:
        case AddOrderMpid:
            m.ref = be64(p + 11);
            m.side = char(p[19]);
            m.shares = be32(p + 20);
            std::memcpy(m.stock, p + 24, 8);
            m.price = be32(p + 32);
            if (type == AddOrderMpid) std::memcpy(m.mpid, p + 36, 4);
            break;
        case Executed:
            m.ref = be64(p + 11);
            m.shares = be32(p + 19);
            m.match = be64(p + 23);
            break;
        case ExecutedWithPrice:
            m.ref = be64(p + 11);
            m.shares = be32(p + 19);
            m.match = be64(p + 23);
            m.event = char(p[31]);    // printable 'Y'/'N'
            m.price = be32(p + 32);
            break;
        case Cancel:
            m.ref = be64(p + 11);
            m.shares = be32(p + 19);
            break;
        case Delete:
            m.ref = be64(p + 11);
            break;
        case Replace:
            m.ref = be64(p + 11);
            m.new_ref = be64(p + 19);
            m.shares = be32(p + 27);
            m.price = be32(p + 31);
            break;
        case Trade:
            m.ref = be64(p + 11);
            m.side = char(p[19]);
            m.shares = be32(p + 20);
            std::memcpy(m.stock, p + 24, 8);
            m.price = be32(p + 32);
            m.match = be64(p + 36);
            break;
        case CrossTrade:
            m.shares = std::uint32_t(be64(p + 11));
            std::memcpy(m.stock, p + 19, 8);
            m.price = be32(p + 27);
            m.match = be64(p + 31);
            m.event = char(p[39]);    // cross type
            break;
        case BrokenTrade:
            m.match = be64(p + 11);
            break;
        default:
            break;  // known length, fields not needed by a book
    }
    return true;
}

// ---- encoding, for the synthetic capture and the tests ----------------------
// Each writes exactly length_of(type) bytes and returns that length.

inline std::size_t put_header(std::uint8_t* p, char type, std::uint16_t locate, std::uint16_t tracking,
                              std::uint64_t ns) noexcept {
    p[0] = std::uint8_t(type);
    wire::put16(p + 1, locate);
    wire::put16(p + 3, tracking);
    wire::put48(p + 5, ns);
    return 11;
}
inline std::size_t encode_system_event(std::uint8_t* p, std::uint64_t ns, char code) noexcept {
    put_header(p, SystemEvent, 0, 0, ns);
    p[11] = std::uint8_t(code);
    return 12;
}
inline std::size_t encode_stock_directory(std::uint8_t* p, std::uint16_t locate, std::uint64_t ns,
                                          std::string_view stock, std::uint32_t round_lot = 100) noexcept {
    put_header(p, StockDirectory, locate, 0, ns);
    wire::put_alpha(p + 11, 8, stock);
    p[19] = 'Q';            // market category: Nasdaq Global Select
    p[20] = 'N';            // financial status: normal
    wire::put32(p + 21, round_lot);
    p[25] = 'N';            // round lots only
    p[26] = 'C';            // issue classification: common
    p[27] = 'Z'; p[28] = ' ';  // issue sub-type
    p[29] = 'P';            // authenticity: live
    p[30] = 'N';            // short sale threshold
    p[31] = 'N';            // IPO flag
    p[32] = '1';            // LULD tier
    p[33] = 'N';            // ETP flag
    wire::put32(p + 34, 0); // ETP leverage
    p[38] = 'N';            // inverse
    return 39;
}
inline std::size_t encode_add(std::uint8_t* p, std::uint16_t locate, std::uint64_t ns, std::uint64_t ref, char side,
                              std::uint32_t shares, std::string_view stock, std::uint32_t price) noexcept {
    put_header(p, AddOrder, locate, 0, ns);
    wire::put64(p + 11, ref);
    p[19] = std::uint8_t(side);
    wire::put32(p + 20, shares);
    wire::put_alpha(p + 24, 8, stock);
    wire::put32(p + 32, price);
    return 36;
}
inline std::size_t encode_executed(std::uint8_t* p, std::uint16_t locate, std::uint64_t ns, std::uint64_t ref,
                                   std::uint32_t shares, std::uint64_t match) noexcept {
    put_header(p, Executed, locate, 0, ns);
    wire::put64(p + 11, ref);
    wire::put32(p + 19, shares);
    wire::put64(p + 23, match);
    return 31;
}
inline std::size_t encode_cancel(std::uint8_t* p, std::uint16_t locate, std::uint64_t ns, std::uint64_t ref,
                                 std::uint32_t shares) noexcept {
    put_header(p, Cancel, locate, 0, ns);
    wire::put64(p + 11, ref);
    wire::put32(p + 19, shares);
    return 23;
}
inline std::size_t encode_delete(std::uint8_t* p, std::uint16_t locate, std::uint64_t ns, std::uint64_t ref) noexcept {
    put_header(p, Delete, locate, 0, ns);
    wire::put64(p + 11, ref);
    return 19;
}
inline std::size_t encode_replace(std::uint8_t* p, std::uint16_t locate, std::uint64_t ns, std::uint64_t ref,
                                  std::uint64_t new_ref, std::uint32_t shares, std::uint32_t price) noexcept {
    put_header(p, Replace, locate, 0, ns);
    wire::put64(p + 11, ref);
    wire::put64(p + 19, new_ref);
    wire::put32(p + 27, shares);
    wire::put32(p + 31, price);
    return 35;
}
inline std::size_t encode_trade(std::uint8_t* p, std::uint16_t locate, std::uint64_t ns, char side, std::uint32_t shares,
                                std::string_view stock, std::uint32_t price, std::uint64_t match) noexcept {
    put_header(p, Trade, locate, 0, ns);
    wire::put64(p + 11, 0);   // non-displayable orders carry reference 0
    p[19] = std::uint8_t(side);
    wire::put32(p + 20, shares);
    wire::put_alpha(p + 24, 8, stock);
    wire::put32(p + 32, price);
    wire::put64(p + 36, match);
    return 44;
}

}  // namespace lob::itch
