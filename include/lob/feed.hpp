// feed.hpp — the feed handler: an ITCH message stream in, books out.
//
// Input is the message-stream form of ITCH: each message preceded by its
// 2-byte big-endian length, which is the format of Nasdaq's public sample
// files and of the synthetic captures gen_itch writes. (On the wire, the same
// messages travel in sequenced UDP packets; that framing and its gap handling
// are a separate layer, see docs/PATH.md.)
//
// The handler keeps one table of live orders for all symbols, keyed by the
// exchange's 64-bit order reference, and one MarketBook per stock locate
// code. Every message type a book needs is applied; everything else is
// counted. After construction nothing on the per-message path allocates: the
// order table is a pool plus a flat hash map, and books are created when the
// symbol directory names them (or on first sight, in a capture without one).
#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "lob/flat_hash_map.hpp"
#include "lob/itch.hpp"
#include "lob/market_book.hpp"

namespace lob {

// What the handler reports upward, per message. The path uses it to decide
// whether the quoted symbol's top of book moved.
struct FeedEvent {
    std::uint16_t locate = 0;
    char          type = 0;         // the ITCH type
    bool          top_changed = false;
    bool          is_trade = false;  // E, C, P: a print happened
    Price         trade_price = 0;   // ticks, when is_trade
    Qty           trade_qty = 0;
    Side          trade_side = Side::Buy;  // the RESTING order's side (E, C); the order's side (P)
};

struct FeedConfig {
    std::uint32_t expected_orders = 1 << 18;  // live orders at once, all symbols
    std::uint32_t book_levels = 1 << 16;      // price window per symbol, in ticks
    std::uint16_t max_locate = 1 << 14;       // size of the locate -> book table
    std::uint32_t raw_per_tick = 100;         // ITCH prices have 4 decimals; a cent is 100
};

class FeedHandler {
public:
    struct Stats {
        std::uint64_t messages = 0, adds = 0, executes = 0, cancels = 0, deletes = 0, replaces = 0, trades = 0;
        std::uint64_t unknown_ref = 0, bad_price = 0, undecodable = 0, other = 0, books = 0;
    };

    explicit FeedHandler(FeedConfig cfg = {})
        : cfg_(cfg), index_(cfg.expected_orders), books_(cfg.max_locate) {
        pool_.resize(cfg.expected_orders);
        pool_.clear();
    }

    // Apply one message. Returns what happened.
    FeedEvent on_message(wire::Bytes bytes) noexcept {
        FeedEvent ev;
        ++stats_.messages;
        if (!itch::decode(bytes, msg_)) { ++stats_.undecodable; return ev; }
        ev.locate = msg_.locate;
        ev.type = msg_.type;
        switch (msg_.type) {
            case itch::StockDirectory: {
                if (msg_.locate < books_.size()) {
                    if (msg_.locate >= symbols_.size()) symbols_.resize(std::size_t(msg_.locate) + 1);
                    symbols_[msg_.locate].assign(reinterpret_cast<const char*>(msg_.stock), 8);
                }
                break;
            }
            case itch::AddOrder:
            case itch::AddOrderMpid: {
                Price p;
                if (!to_ticks(msg_.price, p)) { ++stats_.bad_price; break; }
                ++stats_.adds;
                MarketBook& b = book_for(msg_.locate, p);
                const std::uint32_t n = acquire();
                pool_[n] = Order{msg_.ref, p, msg_.shares, kNil, msg_.locate, msg_.side == 'B' ? Side::Buy : Side::Sell};
                index_.insert(msg_.ref, n);
                ev.top_changed = b.add(pool_[n].side, p, msg_.shares);
                break;
            }
            case itch::Executed:
            case itch::ExecutedWithPrice: {
                const std::uint32_t n = index_.find(msg_.ref);
                if (n == FlatHashMap::kEmpty) { ++stats_.unknown_ref; break; }
                ++stats_.executes;
                Order& o = pool_[n];
                MarketBook& b = book_for(o.locate, o.price);
                ev.is_trade = true;
                ev.trade_qty = msg_.shares;
                ev.trade_price = o.price;
                ev.trade_side = o.side;
                if (msg_.type == itch::ExecutedWithPrice) { Price p; if (to_ticks(msg_.price, p)) ev.trade_price = p; }
                ev.top_changed = shrink(b, o, n, msg_.shares);
                break;
            }
            case itch::Cancel: {
                const std::uint32_t n = index_.find(msg_.ref);
                if (n == FlatHashMap::kEmpty) { ++stats_.unknown_ref; break; }
                ++stats_.cancels;
                Order& o = pool_[n];
                ev.top_changed = shrink(book_for(o.locate, o.price), o, n, msg_.shares);
                break;
            }
            case itch::Delete: {
                const std::uint32_t n = index_.find(msg_.ref);
                if (n == FlatHashMap::kEmpty) { ++stats_.unknown_ref; break; }
                ++stats_.deletes;
                Order& o = pool_[n];
                ev.top_changed = book_for(o.locate, o.price).remove(o.side, o.price, o.qty);
                index_.erase(msg_.ref);
                release(n);
                break;
            }
            case itch::Replace: {
                const std::uint32_t n = index_.find(msg_.ref);
                if (n == FlatHashMap::kEmpty) { ++stats_.unknown_ref; break; }
                Price p;
                if (!to_ticks(msg_.price, p)) { ++stats_.bad_price; break; }
                ++stats_.replaces;
                Order& o = pool_[n];
                MarketBook& b = book_for(o.locate, o.price);
                bool changed = b.remove(o.side, o.price, o.qty);
                index_.erase(msg_.ref);
                o.ref = msg_.new_ref;
                o.price = p;
                o.qty = msg_.shares;
                index_.insert(msg_.new_ref, n);
                changed |= b.add(o.side, p, msg_.shares);
                ev.top_changed = changed;
                break;
            }
            case itch::Trade: {
                Price p;
                if (!to_ticks(msg_.price, p)) { ++stats_.bad_price; break; }
                ++stats_.trades;
                ev.is_trade = true;
                ev.trade_price = p;
                ev.trade_qty = msg_.shares;
                ev.trade_side = msg_.side == 'B' ? Side::Buy : Side::Sell;
                break;
            }
            default:
                ++stats_.other;
                break;
        }
        return ev;
    }

    // The book for a locate code, or nullptr if none has been seen.
    const MarketBook* book(std::uint16_t locate) const noexcept {
        return locate < books_.size() ? books_[locate].get() : nullptr;
    }
    std::string_view symbol(std::uint16_t locate) const noexcept {
        return locate < symbols_.size() ? std::string_view(symbols_[locate]) : std::string_view{};
    }
    // Locate code for a symbol, from the directory messages seen; -1 if unknown.
    int locate_of(std::string_view sym) const noexcept {
        for (std::size_t i = 0; i < symbols_.size(); ++i)
            if (wire::alpha(reinterpret_cast<const std::uint8_t*>(symbols_[i].data()), 8) == sym) return int(i);
        return -1;
    }
    // Is this exchange reference live, and if so where does it rest?
    struct LiveOrder { Price price; Qty qty; Side side; std::uint16_t locate; };
    bool find(std::uint64_t ref, LiveOrder& out) const noexcept {
        const std::uint32_t n = index_.find(ref);
        if (n == FlatHashMap::kEmpty) return false;
        const Order& o = pool_[n];
        out = {o.price, o.qty, o.side, o.locate};
        return true;
    }
    const Stats&      stats() const noexcept { return stats_; }
    std::size_t       live_orders() const noexcept { return index_.size(); }
    const itch::Msg&  last() const noexcept { return msg_; }

private:
    struct Order {
        std::uint64_t ref;
        Price         price;
        Qty           qty;
        std::uint32_t next_free;  // free-list link while the slot is unused
        std::uint16_t locate;
        Side          side;
    };
    static constexpr std::uint32_t kNil = UINT32_MAX;

    bool to_ticks(std::uint32_t raw, Price& out) const noexcept {
        if (raw % cfg_.raw_per_tick != 0) return false;  // sub-penny: not representable in this book
        out = Price(raw / cfg_.raw_per_tick);
        return out > 0;
    }

    // Reduce an order by q shares; remove it when nothing is left.
    bool shrink(MarketBook& b, Order& o, std::uint32_t n, Qty q) noexcept {
        if (q >= o.qty) {
            bool changed = b.remove(o.side, o.price, o.qty);
            index_.erase(o.ref);
            release(n);
            return changed;
        }
        o.qty -= q;
        return b.reduce(o.side, o.price, q);
    }

    MarketBook& book_for(std::uint16_t locate, Price near) {
        if (locate >= books_.size()) locate = 0;  // out-of-range locates share a bucket
        auto& slot = books_[locate];
        if (!slot) {
            // Centre the window on the first price seen for the symbol.
            Price base = near - Price(cfg_.book_levels / 2);
            if (base < 1) base = 1;
            slot = std::make_unique<MarketBook>(base, cfg_.book_levels);
            ++stats_.books;
        }
        return *slot;
    }

    std::uint32_t acquire() noexcept {
        if (free_ != kNil) { std::uint32_t n = free_; free_ = pool_[n].next_free; return n; }
        pool_.emplace_back();  // only once the reservation is used up
        return std::uint32_t(pool_.size() - 1);
    }
    void release(std::uint32_t n) noexcept {
        pool_[n].next_free = free_;
        free_ = n;
    }

    FeedConfig                               cfg_;
    itch::Msg                                msg_;
    std::vector<Order>                       pool_;
    std::uint32_t                            free_ = kNil;
    FlatHashMap                              index_;
    std::vector<std::unique_ptr<MarketBook>> books_;
    std::vector<std::string>                 symbols_;
    Stats                                    stats_;
};

// ---- the message-stream file format ----------------------------------------
// [2-byte big-endian length][message] ... as in Nasdaq's sample files. The
// synthetic generator prefixes each message with nothing else; the ITCH
// timestamp inside the message is the capture time used for paced replay.
class StreamReader {
public:
    explicit StreamReader(wire::Bytes all) : all_(all) {}
    // Next message, or an empty span at the end (or on a truncated record).
    wire::Bytes next() noexcept {
        if (pos_ + 2 > all_.size()) return {};
        std::size_t len = wire::be16(all_.data() + pos_);
        if (pos_ + 2 + len > all_.size()) { pos_ = all_.size(); return {}; }
        wire::Bytes m{all_.data() + pos_ + 2, len};
        pos_ += 2 + len;
        return m;
    }
    void   rewind() noexcept { pos_ = 0; }
    bool   done() const noexcept { return pos_ >= all_.size(); }
    std::size_t offset() const noexcept { return pos_; }

private:
    wire::Bytes all_;
    std::size_t pos_ = 0;
};

inline void stream_append(std::vector<std::uint8_t>& out, wire::Bytes msg) {
    out.push_back(std::uint8_t(msg.size() >> 8));
    out.push_back(std::uint8_t(msg.size()));
    out.insert(out.end(), msg.begin(), msg.end());
}

}  // namespace lob
