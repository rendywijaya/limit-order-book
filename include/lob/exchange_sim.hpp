// exchange_sim.hpp — a simulated exchange, so the whole loop runs under replay.
//
// It speaks the real bytes (SoupBinTCP framing, OUCH 4.2 in and out), accepts,
// replaces, cancels and rejects like Nasdaq would, and fills resting orders
// with a QUEUE-POSITION model driven by the market data: our bid at price P
// joins the back of the exchange's bid queue at P; every execution the feed
// shows against bids at P consumes the queue ahead of us; once the queue
// ahead is gone, further executions at P fill us. If the market trades
// through our price (the best ask falls to P or below), we are filled.
//
// The simulation is deterministic: its answers depend only on the bytes it
// receives and the market events it is shown, in that order. Its answers are
// produced synchronously, which keeps a one-thread and a two-thread replay
// identical message for message. Our fills do not appear on the feed (the
// feed is a recording; we are a shadow participant), which is the usual and
// honest limitation of a replay simulator.
#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

#include "lob/feed.hpp"
#include "lob/ouch.hpp"
#include "lob/soupbin.hpp"

namespace lob {

class ExchangeSim {
public:
    struct Stats {
        std::uint64_t logins = 0, enters = 0, replaces = 0, cancels = 0, heartbeats = 0;
        std::uint64_t accepted = 0, rejected = 0, replaced = 0, canceled = 0, cancel_rejected = 0, fills = 0, filled_qty = 0;
        std::uint64_t fills_queue = 0, fills_through = 0, fills_marketable = 0;
    };

    // `to_client` receives the bytes for the client's socket. The sim fills
    // orders in the symbol with this stock locate code only.
    ExchangeSim(std::function<void(wire::Bytes)> to_client, std::uint16_t locate, std::uint32_t raw_per_tick = 100)
        : to_client_(std::move(to_client)), locate_(locate), raw_per_tick_(raw_per_tick) {
        orders_.reserve(256);
    }

    // Bytes from the client. Parses SoupBinTCP, answers logins and orders.
    void on_client_bytes(wire::Bytes b, std::uint64_t now_ns) {
        framer_.feed(b, [&](char type, wire::Bytes payload) { on_packet(type, payload, now_ns); });
    }

    // A market event for the quoted symbol, after the book has been updated.
    void on_market(const FeedEvent& ev, const BookSnapshot& book, std::uint64_t now_ns) {
        if (ev.locate != locate_) return;
        const TopOfBook& t = book.top;
        for (std::size_t i = 0; i < orders_.size(); ++i) {
            Resting& o = orders_[i];
            if (o.open == 0) continue;
            // 1. The market traded through our price: we are hit in full.
            if (o.side == Side::Buy && t.has_ask && t.ask <= o.price) { fill(o, o.open, o.price, now_ns, 1); continue; }
            if (o.side == Side::Sell && t.has_bid && t.bid >= o.price) { fill(o, o.open, o.price, now_ns, 1); continue; }
            // 2. Executions at our price on our side consume the queue, then us.
            if (ev.is_trade && ev.trade_side == o.side && ev.trade_price == o.price) {
                Qty q = ev.trade_qty;
                if (o.queue_ahead >= q) { o.queue_ahead -= q; continue; }
                q -= Qty(o.queue_ahead);
                o.queue_ahead = 0;
                fill(o, q > o.open ? o.open : q, o.price, now_ns, 0);
            }
        }
        compact();
    }

    const Stats& stats() const noexcept { return stats_; }
    std::size_t  resting() const noexcept {
        std::size_t n = 0;
        for (const Resting& o : orders_) n += o.open > 0;
        return n;
    }
    // The current market snapshot, needed to place new orders in the queue.
    void set_book(const BookSnapshot* b) noexcept { book_ = b; }

private:
    struct Resting {
        std::uint64_t token;
        Side          side;
        Price         price;
        Qty           open;
        Qty           filled;
        std::uint64_t ref;
        QtySum        queue_ahead;
        std::uint8_t  stock[8];
    };

    void send(std::size_t n) { std::uint8_t f[160]; to_client_({f, soup::frame(f, soup::SequencedData, {obuf_, n})}); }

    void on_packet(char type, wire::Bytes payload, std::uint64_t now_ns) {
        switch (type) {
            case soup::LoginRequest: {
                ++stats_.logins;
                std::uint8_t f[64];
                to_client_({f, soup::frame_login_accepted(f, "SIM", 1)});
                send(ouch::encode_system_event(obuf_, now_ns, 'S'));  // start of day
                break;
            }
            case soup::ClientHeartbeat: ++stats_.heartbeats; break;
            case soup::UnsequencedData: on_ouch(payload, now_ns); break;
            default: break;
        }
    }

    void on_ouch(wire::Bytes b, std::uint64_t now_ns) {
        if (b.empty()) return;
        switch (char(b[0])) {
            case ouch::EnterOrder: { ouch::Enter e; if (ouch::decode(b, e)) on_enter(e, now_ns); break; }
            case ouch::ReplaceOrder: { ouch::Replace r; if (ouch::decode(b, r)) on_replace(r, now_ns); break; }
            case ouch::CancelOrder: { ouch::Cancel c; if (ouch::decode(b, c)) on_cancel(c, now_ns); break; }
            default: break;
        }
    }

    Resting* find(std::uint64_t token) noexcept {
        for (Resting& o : orders_) if (o.token == token && o.open) return &o;
        return nullptr;
    }

    void on_enter(const ouch::Enter& e, std::uint64_t now_ns) {
        ++stats_.enters;
        const std::uint64_t tok = ouch::token_value(e.token);
        const Price price = Price(e.price / raw_per_tick_);
        if (e.shares == 0 || e.price == 0 || e.price % raw_per_tick_ != 0 || (e.side != 'B' && e.side != 'S')) {
            ++stats_.rejected;
            send(ouch::encode_rejected(obuf_, now_ns, e.token, 'X'));
            return;
        }
        Resting o{tok, e.side == 'B' ? Side::Buy : Side::Sell, price, e.shares, 0, ++next_ref_, 0, {}};
        std::memcpy(o.stock, e.stock, 8);
        ++stats_.accepted;
        send(ouch::encode_accepted(obuf_, now_ns, e.token, e.side, e.shares, e.stock, e.price, e.tif, o.ref, 'L'));
        place(o, now_ns);
    }

    // Put an order in the book: take what is marketable against the touch,
    // rest the remainder behind the quantity already at its price.
    void place(Resting o, std::uint64_t now_ns) {
        const TopOfBook* t = book_ ? &book_->top : nullptr;
        if (t) {
            if (o.side == Side::Buy && t->has_ask && o.price >= t->ask) {
                Qty q = Qty(t->ask_qty < o.open ? t->ask_qty : o.open);
                fill(o, q, t->ask, now_ns, 2);
            } else if (o.side == Side::Sell && t->has_bid && o.price <= t->bid) {
                Qty q = Qty(t->bid_qty < o.open ? t->bid_qty : o.open);
                fill(o, q, t->bid, now_ns, 2);
            }
            if (o.open) o.queue_ahead = book_->qty_at(o.side, o.price);  // the queue we join the back of
        }
        if (o.open) orders_.push_back(o);
    }

    void on_replace(const ouch::Replace& r, std::uint64_t now_ns) {
        ++stats_.replaces;
        Resting* o = find(ouch::token_value(r.token));
        if (!o) { ++stats_.rejected; send(ouch::encode_rejected(obuf_, now_ns, r.new_token, 'O')); return; }
        const Price price = Price(r.price / raw_per_tick_);
        Resting n = *o;
        n.token = ouch::token_value(r.new_token);
        n.price = price;
        n.open = r.shares > o->filled ? r.shares - o->filled : 0;  // new total minus what already filled
        n.ref = ++next_ref_;
        const bool keeps_priority = price == o->price && n.open <= o->open;
        o->open = 0;  // the original is gone
        ++stats_.replaced;
        if (n.open == 0) {
            send(ouch::encode_replaced(obuf_, now_ns, r.new_token, n.side == Side::Buy ? 'B' : 'S', 0, n.stock, r.price, r.tif, n.ref, 'D', r.token));
            return;
        }
        send(ouch::encode_replaced(obuf_, now_ns, r.new_token, n.side == Side::Buy ? 'B' : 'S', n.open, n.stock, r.price, r.tif, n.ref, 'L', r.token));
        if (keeps_priority) orders_.push_back(n);
        else place(n, now_ns);
    }

    void on_cancel(const ouch::Cancel& c, std::uint64_t now_ns) {
        ++stats_.cancels;
        Resting* o = find(ouch::token_value(c.token));
        if (!o) { ++stats_.cancel_rejected; send(ouch::encode_cancel_reject(obuf_, now_ns, c.token)); return; }
        Qty dec = c.shares == 0 || c.shares >= o->open ? o->open : o->open - c.shares;
        o->open -= dec;
        ++stats_.canceled;
        send(ouch::encode_canceled(obuf_, now_ns, c.token, dec, 'U'));
    }

    void fill(Resting& o, Qty q, Price px, std::uint64_t now_ns, int how) {
        if (q == 0) return;
        o.open -= q;
        o.filled += q;
        ++stats_.fills;
        stats_.filled_qty += q;
        (how == 0 ? stats_.fills_queue : how == 1 ? stats_.fills_through : stats_.fills_marketable) += 1;
        send(ouch::encode_executed(obuf_, now_ns, ouch::make_token(o.token), q, std::uint32_t(px) * raw_per_tick_, how == 2 ? 'R' : 'A', ++next_match_));
    }

    void compact() {
        std::size_t w = 0;
        for (std::size_t i = 0; i < orders_.size(); ++i) if (orders_[i].open) orders_[w++] = orders_[i];
        orders_.resize(w);
    }

    std::function<void(wire::Bytes)> to_client_;
    std::uint16_t                    locate_;
    std::uint32_t                    raw_per_tick_;
    const BookSnapshot*              book_ = nullptr;
    soup::Framer<4096>               framer_;
    std::uint8_t                     obuf_[128];
    std::vector<Resting>             orders_;
    std::uint64_t                    next_ref_ = 1'000'000, next_match_ = 0;
    Stats                            stats_;
};

}  // namespace lob
