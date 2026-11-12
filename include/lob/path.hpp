// path.hpp — the whole path from a market-data message to an order, with a
// timestamp at every stage.
//
//   message ─► FeedSide ─────────────────────► MarketUpdate ─► Strategy ─► order bytes
//              decode, book, snapshot         (one thread:   quoter, risk,   (to the simulated
//                                              a function     gateway        exchange, whose
//                                              call; two:                    answers come back
//                                              an SPSC ring)                 the same way)
//
// Stage timestamps (clock ticks, see clock.hpp):
//   t_arrive     when the message is taken up: now() in a flat-out replay, the
//                message's DUE time in a paced replay (so waiting counts)
//   t_feed_done  after decode + book update + snapshot
//   t_pop        when the strategy picked the update up (== t_feed_done on one thread)
//   t_decided    after the quoter's decision
//   t_risk       after the risk gate
//   t_wire       when the order's bytes reach the wire (the end of OUR work)
//   t_sent       after the exchange's synchronous answer has been processed
//
// Histograms:
//   feed             t_feed_done - t_arrive        every message
//   queue            t_pop - t_feed_done           every update that reached the strategy
//   decide           t_decided - t_pop             every update with a top change
//   risk             t_risk - t_decided            every decision that produced intents
//   send             t_wire - t_risk               every intent that passed risk: gateway state,
//                                                  OUCH encode, SoupBinTCP frame
//   exchange         t_sent - t_wire               the simulated exchange's answer and our
//                                                  processing of it (not part of tick-to-trade)
//   tick_to_trade    t_wire - t_arrive             every message that produced at least one order
//   wire_to_decision t_decided - t_arrive          every message that reached the quoter
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "lob/clock.hpp"
#include "lob/exchange_sim.hpp"
#include "lob/feed.hpp"
#include "lob/gateway.hpp"
#include "lob/histogram.hpp"
#include "lob/market_book.hpp"
#include "lob/quoter.hpp"
#include "lob/risk.hpp"

namespace lob {

struct PathConfig {
    std::string  symbol = "AAPL";
    QuoterParams quoter;
    RiskLimits   risk;
    FeedConfig   feed;
};

// What crosses from the feed side to the strategy side.
struct MarketUpdate {
    FeedEvent     ev;
    BookSnapshot  book;
    std::uint64_t itch_ns = 0;      // the exchange's timestamp on the message
    std::uint64_t t_arrive = 0;     // clock ticks
    std::uint64_t t_feed_done = 0;  // clock ticks
    std::uint64_t seq = 0;          // message number in the capture
};

struct StageHistograms {
    Histogram feed, queue, decide, risk, send, exchange, tick_to_trade, wire_to_decision, lateness;
};

// ---- the feed side --------------------------------------------------------------
class FeedSide {
public:
    explicit FeedSide(const PathConfig& cfg) : cfg_(cfg), feed_(cfg.feed) {}

    // Apply one message. Returns true and fills `u` when the message concerns
    // the quoted symbol (which is the only thing the strategy needs to see).
    bool on_message(wire::Bytes msg, std::uint64_t t_arrive, std::uint64_t seq, MarketUpdate& u) noexcept {
        u.ev = feed_.on_message(msg);
        if (locate_ < 0 && u.ev.type == itch::StockDirectory) {
            int l = feed_.locate_of(cfg_.symbol);
            if (l >= 0) locate_ = l;
        }
        if (locate_ < 0 || u.ev.locate != locate_) return false;
        const MarketBook* b = feed_.book(std::uint16_t(locate_));
        if (!b) return false;
        snapshot(*b, u.book);
        u.itch_ns = feed_.last().ns;
        u.t_arrive = t_arrive;
        u.seq = seq;
        u.t_feed_done = bench::ticks();
        return true;
    }

    int                locate() const noexcept { return locate_; }
    const FeedHandler& feed() const noexcept { return feed_; }

private:
    PathConfig  cfg_;
    FeedHandler feed_;
    int         locate_ = -1;
};

// ---- the strategy side ----------------------------------------------------------
class Strategy {
public:
    struct Counts {
        std::uint64_t updates = 0, top_changes = 0, intents = 0, refused = 0, unsendable = 0, sent_new = 0,
                      sent_replace = 0, sent_cancel = 0, messages_with_orders = 0;
    };

    Strategy(const PathConfig& cfg, std::uint16_t locate)
        : cfg_(cfg),
          quoter_(cfg.quoter),
          risk_(cfg.risk),
          sim_([this](wire::Bytes b) { gw_.on_wire(b, now_ns_); }, locate),
          gw_(quoter_, [this](wire::Bytes b) { t_wire_ = bench::ticks(); hash_bytes(b); sim_.on_client_bytes(b, now_ns_); }, cfg.symbol) {
        sim_.set_book(&book_);
    }

    void start(std::uint64_t now_ns) {
        now_ns_ = now_ns;
        gw_.login(now_ns);
    }

    // One update. Records into h. t_pop is when this thread picked it up.
    void on_update(const MarketUpdate& u, std::uint64_t t_pop, StageHistograms& h, bool record) noexcept {
        ++counts_.updates;
        now_ns_ = u.itch_ns;
        book_ = u.book;  // the snapshot the sim and the quoter both see
        if (record) h.queue.record(t_pop - u.t_feed_done);
        // Fills driven by the market come first: they change the position
        // before the decision, as they would with a real exchange.
        sim_.on_market(u.ev, book_, now_ns_);
        if (!u.ev.top_changed) return;
        ++counts_.top_changes;
        Intent its[2];
        const int n = quoter_.decide(book_.top, its);
        const std::uint64_t t_decided = bench::ticks();
        if (record) { h.decide.record(t_decided - t_pop); h.wire_to_decision.record(t_decided - u.t_arrive); }
        if (n == 0) return;
        counts_.intents += std::uint64_t(n);
        RiskGate::TopRef ref{book_.top.two_sided(), book_.top.mid2()};
        bool any = false;
        for (int i = 0; i < n; ++i) {
            const Intent& it = its[i];
            RiskReason r = RiskReason::Ok;
            switch (it.kind) {
                case Intent::Kind::New: r = risk_.check_new(it.side, it.price, it.qty, ref, gw_.exposure(), now_ns_); break;
                case Intent::Kind::Replace: {
                    const OrderRecord* o = gw_.order(it.token);
                    r = risk_.check_replace(it.side, it.price, it.qty, o ? o->open : 0, ref, gw_.exposure(), now_ns_);
                    break;
                }
                case Intent::Kind::Cancel: r = risk_.check_cancel(now_ns_); break;
                case Intent::Kind::None: continue;
            }
            const std::uint64_t t_risk = bench::ticks();
            if (record) h.risk.record(t_risk - t_decided);
            if (r != RiskReason::Ok) { ++counts_.refused; quoter_.on_refused(it); continue; }
            // Reserve the token and mark the side pending BEFORE the send:
            // the simulated exchange answers inside the send, and the answer
            // must find the quoter already expecting it.
            std::uint64_t tok = it.kind == Intent::Kind::Cancel ? it.token : gw_.reserve_token();
            quoter_.on_sent(it, tok);
            bool ok = false;
            switch (it.kind) {
                case Intent::Kind::New: ok = gw_.send_new(it.side, it.price, it.qty, now_ns_, tok) != 0; ++counts_.sent_new; break;
                case Intent::Kind::Replace: ok = gw_.send_replace(it.token, it.price, it.qty, now_ns_, tok) != 0; ++counts_.sent_replace; break;
                case Intent::Kind::Cancel: ok = gw_.send_cancel(it.token, now_ns_); ++counts_.sent_cancel; break;
                case Intent::Kind::None: break;
            }
            const std::uint64_t t_sent = bench::ticks();
            if (!ok) { ++counts_.unsendable; quoter_.on_refused(it); continue; }
            if (record) { h.send.record(t_wire_ - t_risk); h.exchange.record(t_sent - t_wire_); }
            any = true;
            last_wire_ = t_wire_;
        }
        if (any) {
            ++counts_.messages_with_orders;
            if (record) h.tick_to_trade.record(last_wire_ - u.t_arrive);
        }
    }

    // A fold over every outbound byte: two replays that agree here sent
    // exactly the same orders in the same sequence.
    std::uint64_t order_hash() const noexcept { return hash_; }
    const Counts&            counts() const noexcept { return counts_; }
    const Quoter&            quoter() const noexcept { return quoter_; }
    const RiskGate&          risk() const noexcept { return risk_; }
    const Gateway<Quoter>&   gateway() const noexcept { return gw_; }
    const ExchangeSim&       sim() const noexcept { return sim_; }
    // Mark-to-market: cash from fills plus the position valued at the mid.
    // In ticks x shares (cents for a one-cent tick).
    std::int64_t pnl_ticks(const TopOfBook& t) const noexcept {
        std::int64_t mtm = t.two_sided() ? gw_.exposure().position * std::int64_t(t.mid2()) / 2 : 0;
        return gw_.cash_ticks() + mtm;
    }

private:
    void hash_bytes(wire::Bytes b) noexcept {
        for (std::uint8_t c : b) hash_ = (hash_ ^ c) * 1099511628211ull;
    }

    PathConfig          cfg_;
    Quoter              quoter_;
    RiskGate            risk_;
    ExchangeSim         sim_;
    Gateway<Quoter>     gw_;
    BookSnapshot        book_;
    std::uint64_t       now_ns_ = 0;
    std::uint64_t       hash_ = 1469598103934665603ull;
    std::uint64_t       t_wire_ = 0, last_wire_ = 0;
    Counts              counts_;
};

}  // namespace lob
