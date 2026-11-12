// gateway.hpp — the order gateway: intents in, OUCH out, answers back, state kept.
//
// The only component that talks to the exchange's order port. It owns the
// SoupBinTCP session, turns the quoter's intents into OUCH bytes, assigns the
// tokens, and keeps the state of every order from the exchange's answers:
// pending, live, how much is open, how much filled, dead. From that state it
// maintains the Exposure the risk gate reads (position, open quantity per
// side, open order count), so risk never has to trust the strategy's idea of
// what is out there.
//
// The gateway reports each answer to a Listener (the quoter) through inlined
// calls, the same pattern as the books. It never allocates after construction:
// order records live in a pool indexed through a flat hash map keyed by token.
#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

#include "lob/flat_hash_map.hpp"
#include "lob/ouch.hpp"
#include "lob/risk.hpp"
#include "lob/soupbin.hpp"

namespace lob {

enum class OrderState : std::uint8_t { PendingNew, Live, PendingReplace, PendingCancel, Dead };

struct OrderRecord {
    std::uint64_t token = 0;
    Side          side = Side::Buy;
    Price         price = 0;
    Qty           open = 0;        // resting (or requested, while pending new)
    Qty           filled = 0;
    OrderState    state = OrderState::Dead;
    std::uint64_t exchange_ref = 0;
    std::uint64_t replaced_by = 0;  // token of the pending replacement
    std::uint64_t replaces = 0;     // for a replacement: the original's token
    std::uint32_t next_free = 0;
};

template <class L>
concept GatewayListener = requires(L l, std::uint64_t t, Price p, Qty q) {
    { l.on_accepted(t, p, q) };
    { l.on_replaced(t, p, q) };
    { l.on_canceled(t, q) };
    { l.on_executed(t, q, p) };
    { l.on_rejected(t) };
    { l.on_cancel_rejected(t) };
};

template <GatewayListener L>
class Gateway {
public:
    struct Stats {
        std::uint64_t sent_new = 0, sent_replace = 0, sent_cancel = 0;
        std::uint64_t accepted = 0, replaced = 0, canceled = 0, executed = 0, rejected = 0, cancel_rejected = 0;
        std::uint64_t undecodable = 0, unknown_token = 0;
    };

    // `to_wire` receives the bytes to put on the socket. `symbol` is the
    // 8-byte stock field OUCH wants. `raw_per_tick` converts our integer
    // ticks to OUCH's four-decimal prices.
    Gateway(L& listener, std::function<void(wire::Bytes)> to_wire, std::string_view symbol,
            std::uint32_t expected_orders = 1 << 12, std::uint32_t raw_per_tick = 100)
        : listener_(listener),
          session_([this](wire::Bytes b) { to_wire_(b); }, [this](std::uint64_t, wire::Bytes b) { on_answer(b); }),
          to_wire_(std::move(to_wire)),
          raw_per_tick_(raw_per_tick),
          index_(expected_orders) {
        wire::put_alpha(stock_, 8, symbol);
        pool_.resize(expected_orders);
        pool_.clear();
    }

    // ---- session ------------------------------------------------------------
    void login(std::uint64_t now_ns, std::string_view user = "TRADER", std::string_view pass = "pass") {
        session_.login(user, pass, "", now_ns);
    }
    void on_wire(wire::Bytes from_socket, std::uint64_t now_ns) { session_.on_bytes(from_socket, now_ns); }
    bool tick(std::uint64_t now_ns) { return session_.tick(now_ns); }
    bool logged_in() const noexcept { return session_.state() == soup::ClientSession::State::LoggedIn; }
    const soup::ClientSession& session() const noexcept { return session_; }

    // ---- outbound -------------------------------------------------------------
    // Tokens can be reserved ahead of the send, so the strategy can mark its
    // side pending with the right token BEFORE the exchange's answer arrives
    // (with an in-memory exchange the answer comes back inside the send).
    std::uint64_t reserve_token() noexcept { return ++next_token_; }

    // Each returns the token used (0 if not logged in or not sendable).
    std::uint64_t send_new(Side side, Price price, Qty qty, std::uint64_t now_ns, std::uint64_t reserved = 0) {
        if (!logged_in()) return 0;
        const std::uint64_t tok = reserved ? reserved : ++next_token_;
        ouch::Enter e{};
        e.token = ouch::make_token(tok);
        e.side = side == Side::Buy ? 'B' : 'S';
        e.shares = qty;
        std::memcpy(e.stock, stock_, 8);
        e.price = std::uint32_t(price) * raw_per_tick_;
        e.tif = ouch::kTifDay;
        wire::put_alpha(e.firm, 4, "FIRM");
        e.display = 'Y'; e.capacity = 'P'; e.iso = 'N'; e.min_qty = 0; e.cross_type = 'N'; e.customer = 'N';
        const std::size_t n = ouch::encode(buf_, e);
        const std::uint32_t slot = acquire();
        pool_[slot] = OrderRecord{tok, side, price, qty, 0, OrderState::PendingNew, 0, 0, 0, 0};
        index_.insert(tok, slot);
        add_exposure(side, qty, +1);
        ++stats_.sent_new;
        session_.send_data({buf_, n}, now_ns);
        return tok;
    }
    std::uint64_t send_replace(std::uint64_t token, Price price, Qty qty, std::uint64_t now_ns, std::uint64_t reserved = 0) {
        const std::uint32_t slot = index_.find(token);
        if (slot == FlatHashMap::kEmpty || !logged_in()) return 0;
        OrderRecord& o = pool_[slot];
        if (o.state != OrderState::Live) return 0;
        const std::uint64_t tok = reserved ? reserved : ++next_token_;
        ouch::Replace r{};
        r.token = ouch::make_token(token);
        r.new_token = ouch::make_token(tok);
        r.shares = qty;  // OUCH: the new TOTAL; the exchange subtracts what has filled
        r.price = std::uint32_t(price) * raw_per_tick_;
        r.tif = ouch::kTifDay; r.display = 'Y'; r.iso = 'N'; r.min_qty = 0;
        const std::size_t n = ouch::encode(buf_, r);
        o.state = OrderState::PendingReplace;
        o.replaced_by = tok;
        const std::uint32_t ns = acquire();
        pool_[ns] = OrderRecord{tok, o.side, price, qty > o.filled ? qty - o.filled : 0, 0, OrderState::PendingNew, 0, 0, token, 0};
        index_.insert(tok, ns);
        // Until the exchange answers, both could exist: the worst case counts
        // the replacement's quantity and one more order.
        add_exposure(o.side, pool_[ns].open, +1);
        ++stats_.sent_replace;
        session_.send_data({buf_, n}, now_ns);
        return tok;
    }
    bool send_cancel(std::uint64_t token, std::uint64_t now_ns) {
        const std::uint32_t slot = index_.find(token);
        if (slot == FlatHashMap::kEmpty || !logged_in()) return false;
        OrderRecord& o = pool_[slot];
        if (o.state != OrderState::Live) return false;
        ouch::Cancel c{ouch::make_token(token), 0};
        const std::size_t n = ouch::encode(buf_, c);
        o.state = OrderState::PendingCancel;
        ++stats_.sent_cancel;
        session_.send_data({buf_, n}, now_ns);
        return true;
    }

    // ---- inbound: one OUCH outbound message from the exchange -----------------
    void on_answer(wire::Bytes b) {
        if (!ouch::decode_out(b, out_)) { ++stats_.undecodable; return; }
        const std::uint64_t tok = ouch::token_value(out_.token);
        const std::uint32_t slot = out_.type == ouch::SystemEvent ? FlatHashMap::kEmpty : index_.find(tok);
        if (out_.type != ouch::SystemEvent && slot == FlatHashMap::kEmpty) { ++stats_.unknown_token; return; }
        switch (out_.type) {
            case ouch::Accepted: {
                OrderRecord& o = pool_[slot];
                ++stats_.accepted;
                o.exchange_ref = out_.order_ref;
                set_open(o, out_.shares);
                if (out_.state == 'D' || out_.shares == 0) kill(o, slot);
                else o.state = OrderState::Live;
                listener_.on_accepted(tok, o.price, out_.shares);
                break;
            }
            case ouch::Replaced: {
                // The new token's record is `slot`; the old one is found by prev_token.
                OrderRecord& n = pool_[slot];
                ++stats_.replaced;
                const std::uint64_t prev = ouch::token_value(out_.prev_token);
                const std::uint32_t ps = index_.find(prev);
                if (ps != FlatHashMap::kEmpty) { OrderRecord& p = pool_[ps]; set_open(p, 0); kill(p, ps); }
                n.exchange_ref = out_.order_ref;
                set_open(n, out_.shares);
                if (out_.state == 'D' || out_.shares == 0) kill(n, slot);
                else n.state = OrderState::Live;
                listener_.on_replaced(tok, n.price, out_.shares);
                break;
            }
            case ouch::Canceled: {
                OrderRecord& o = pool_[slot];
                ++stats_.canceled;
                const Qty dec = out_.shares > o.open ? o.open : out_.shares;
                set_open(o, o.open - dec);
                if (o.open == 0) kill(o, slot);
                else o.state = OrderState::Live;
                listener_.on_canceled(tok, dec);
                break;
            }
            case ouch::Executed: {
                OrderRecord& o = pool_[slot];
                ++stats_.executed;
                const Qty q = out_.shares > o.open ? o.open : out_.shares;
                o.filled += q;
                set_open(o, o.open - q);
                exposure_.position += o.side == Side::Buy ? std::int64_t(q) : -std::int64_t(q);
                const std::int64_t notional = std::int64_t(out_.price / raw_per_tick_) * std::int64_t(q);
                cash_ticks_ += o.side == Side::Buy ? -notional : notional;
                // An order with nothing open and nothing pending is done.
                if (o.open == 0 && o.state == OrderState::Live) kill(o, slot);
                listener_.on_executed(tok, q, Price(out_.price / raw_per_tick_));
                break;
            }
            case ouch::Rejected: {
                OrderRecord& o = pool_[slot];
                ++stats_.rejected;
                set_open(o, 0);
                kill(o, slot);
                // If this was a replacement, the original is live again.
                if (o.replaces) {
                    const std::uint32_t os = index_.find(o.replaces);
                    if (os != FlatHashMap::kEmpty && pool_[os].state == OrderState::PendingReplace) {
                        pool_[os].state = OrderState::Live;
                        pool_[os].replaced_by = 0;
                    }
                }
                listener_.on_rejected(tok);
                break;
            }
            case ouch::CancelReject: {
                OrderRecord& o = pool_[slot];
                ++stats_.cancel_rejected;
                if (o.state == OrderState::PendingCancel) o.state = o.open ? OrderState::Live : OrderState::Dead;
                if (o.state == OrderState::Dead) kill(o, slot);
                listener_.on_cancel_rejected(tok);
                break;
            }
            default: break;
        }
    }

    const Exposure& exposure() const noexcept { return exposure_; }
    // Cash from fills, in ticks x shares (cents for a one-cent tick): buys
    // spend, sells receive. Position x mid added to this is the mark-to-market.
    std::int64_t    cash_ticks() const noexcept { return cash_ticks_; }
    const Stats&    stats() const noexcept { return stats_; }
    const OrderRecord* order(std::uint64_t token) const noexcept {
        const std::uint32_t slot = index_.find(token);
        return slot == FlatHashMap::kEmpty ? nullptr : &pool_[slot];
    }
    std::size_t live_records() const noexcept { return index_.size(); }

private:
    static constexpr std::uint32_t kNil = UINT32_MAX;

    // Exposure bookkeeping: open quantity per side and the open-order count.
    void add_exposure(Side s, Qty q, int orders) noexcept {
        (s == Side::Buy ? exposure_.open_buy_qty : exposure_.open_sell_qty) += q;
        exposure_.open_orders += std::uint32_t(orders);
    }
    void set_open(OrderRecord& o, Qty open) noexcept {
        Qty& side_open = o.side == Side::Buy ? exposure_.open_buy_qty : exposure_.open_sell_qty;
        side_open = side_open - (o.open > side_open ? side_open : o.open) + open;
        o.open = open;
    }
    void kill(OrderRecord& o, std::uint32_t slot) noexcept {
        if (o.state != OrderState::Dead) {
            o.state = OrderState::Dead;
            if (exposure_.open_orders) --exposure_.open_orders;
        }
        set_open(o, 0);
        index_.erase(o.token);
        release(slot);
    }
    std::uint32_t acquire() noexcept {
        if (free_ != kNil) { std::uint32_t n = free_; free_ = pool_[n].next_free; return n; }
        pool_.emplace_back();
        return std::uint32_t(pool_.size() - 1);
    }
    void release(std::uint32_t n) noexcept { pool_[n].next_free = free_; free_ = n; }

    L&                                 listener_;
    soup::ClientSession                session_;
    std::function<void(wire::Bytes)>   to_wire_;
    std::uint32_t                      raw_per_tick_;
    std::uint8_t                       stock_[8];
    std::uint8_t                       buf_[128];
    ouch::Out                          out_;
    std::vector<OrderRecord>           pool_;
    std::uint32_t                      free_ = kNil;
    FlatHashMap                        index_;
    std::uint64_t                      next_token_ = 0;
    Exposure                           exposure_;
    std::int64_t                       cash_ticks_ = 0;
    Stats                              stats_;
};

}  // namespace lob
