// quoter.hpp — a market maker: decisions from the book, one request in flight per side.
//
// The strategy of the path. It keeps a bid and an ask around the market's mid,
// a few ticks wide, leans them away from the position it has accumulated, and
// moves only when moving is worth a message. It is deliberately simple: the
// point of this repository is the path and its measurement, and a strategy
// that can be explained in a paragraph is one whose latency you can reason
// about.
//
//   fair      = mid − skew × position              (in half-ticks, exact)
//   bid       = fair − half_spread, ask = fair + half_spread, rounded outward
//   post-only = never cross or lock the market: bid < best ask, ask > best bid
//   requote   = only if the desired price moved by at least `threshold` ticks
//   limits    = when |position| reaches max_position, pull the side that
//               would increase it
//
// Each side is a small state machine with at most ONE request in flight: a
// new order, a replace, or a cancel. While something is pending, the side does
// nothing else, because the exchange's answer may change what the right next
// action is. The races the answers can produce (a fill while a cancel is
// pending, a replace rejected, a fill that empties the order mid-replace) are
// handled in the on_* methods, and tests/test_quoter.cpp drives each one.
//
// The quoter never allocates and never reads a clock. Everything it decides
// is a function of the top of book, its position, and its own state, so a
// replay reproduces its decisions exactly.
#pragma once

#include <cstdint>

#include "lob/market_book.hpp"
#include "lob/types.hpp"

namespace lob {

struct QuoterParams {
    Price        half_spread_ticks = 2;
    Qty          quote_qty = 100;
    Price        skew_ticks_per_lot = 1;   // shift fair by this per quote_qty of position
    Price        requote_threshold = 1;    // ticks the desired price must move
    std::int64_t max_position = 500;       // shares; pull the growing side at this
};

enum class QuoteState : std::uint8_t { Idle, PendingNew, Live, PendingReplace, PendingCancel };

// What the quoter wants the gateway to do. At most one per side per decision.
struct Intent {
    enum class Kind : std::uint8_t { None, New, Replace, Cancel } kind = Kind::None;
    Side          side = Side::Buy;
    Price         price = 0;
    Qty           qty = 0;
    std::uint64_t token = 0;  // Replace / Cancel: the live order
};

class Quoter {
public:
    struct SideState {
        QuoteState    state = QuoteState::Idle;
        std::uint64_t token = 0;          // the live (or pending-new) order's token
        Price         price = 0;          // live price
        Qty           qty = 0;            // live remaining quantity
        std::uint64_t pending_token = 0;  // replacement token while PendingReplace
        Price         pending_price = 0;
        Qty           pending_qty = 0;
    };
    struct Stats {
        std::uint64_t decisions = 0, news = 0, replaces = 0, cancels = 0, fills = 0, filled_qty = 0;
        std::uint64_t rejects = 0, cancel_rejects = 0, pulled = 0;
    };

    explicit Quoter(QuoterParams p = {}) : p_(p) {}

    // The decision. Called whenever the top of the quoted symbol's book
    // changes. Fills `out[0..1]` with up to two intents and returns how many.
    int decide(const TopOfBook& top, Intent out[2]) noexcept {
        ++stats_.decisions;
        int n = 0;
        if (!top.two_sided()) {  // no market: pull whatever is live
            for (Side s : {Side::Buy, Side::Sell})
                if (SideState& ss = mut(s); ss.state == QuoteState::Live) { out[n++] = cancel_of(s, ss); }
            return n;
        }
        // fair in half-ticks: (bid + ask) - 2 * skew * (position / quote_qty)
        const std::int64_t lots = position_ / std::int64_t(p_.quote_qty);
        const std::int64_t fair2 = std::int64_t(top.mid2()) - 2 * std::int64_t(p_.skew_ticks_per_lot) * lots;
        // bid rounds down, ask rounds up, so the quote is never tighter than asked
        Price bid = Price(floor_div(fair2 - 2 * std::int64_t(p_.half_spread_ticks), 2));
        Price ask = Price(-floor_div(-(fair2 + 2 * std::int64_t(p_.half_spread_ticks)), 2));
        // post-only: never cross or lock the market, and never lock ourselves
        if (bid >= top.ask) bid = top.ask - 1;
        if (ask <= top.bid) ask = top.bid + 1;
        if (ask <= bid) ask = bid + 1;
        const bool want_bid = position_ < p_.max_position && bid > 0;
        const bool want_ask = position_ > -p_.max_position;
        n += decide_side(Side::Buy, want_bid, bid, out + n);
        n += decide_side(Side::Sell, want_ask, ask, out + n);
        return n;
    }

    // ---- what the gateway tells the quoter -------------------------------------
    // The gateway sent an intent and assigned it a token (a new token for
    // New and Replace). The side is now pending.
    void on_sent(const Intent& it, std::uint64_t token) noexcept {
        SideState& ss = mut(it.side);
        switch (it.kind) {
            case Intent::Kind::New:
                ss.state = QuoteState::PendingNew; ss.token = token; ss.price = it.price; ss.qty = it.qty; ++stats_.news; break;
            case Intent::Kind::Replace:
                ss.state = QuoteState::PendingReplace; ss.pending_token = token; ss.pending_price = it.price; ss.pending_qty = it.qty; ++stats_.replaces; break;
            case Intent::Kind::Cancel:
                ss.state = QuoteState::PendingCancel; ++stats_.cancels; break;
            case Intent::Kind::None: break;
        }
    }
    // The intent was refused before it left (risk): the side goes back to
    // what it was.
    void on_refused(const Intent& it) noexcept {
        SideState& ss = mut(it.side);
        if (it.kind == Intent::Kind::New) ss = SideState{};
    }
    void on_accepted(std::uint64_t token, Price price, Qty qty) noexcept {
        if (SideState* ss = by_token(token); ss && ss->state == QuoteState::PendingNew) {
            ss->state = QuoteState::Live; ss->price = price; ss->qty = qty;
        }
    }
    // Replaced: the exchange names the new token and the quantity now resting.
    // A fill during the replace shows up as a smaller qty, or as state 'D'
    // (dead), which the gateway passes as qty == 0.
    void on_replaced(std::uint64_t new_token, Price price, Qty qty) noexcept {
        for (SideState& ss : sides_)
            if (ss.state == QuoteState::PendingReplace && ss.pending_token == new_token) {
                ss.token = new_token; ss.price = price; ss.qty = qty;
                ss.pending_token = 0;
                ss.state = qty ? QuoteState::Live : QuoteState::Idle;
                if (!qty) ss = SideState{};
                return;
            }
    }
    void on_canceled(std::uint64_t token, Qty decrement) noexcept {
        if (SideState* ss = by_token(token)) {
            ss->qty = decrement >= ss->qty ? 0 : ss->qty - decrement;
            if (ss->state == QuoteState::PendingCancel || ss->qty == 0) *ss = SideState{};
        }
    }
    // A fill. Updates the position whatever state the side is in: fills can
    // arrive while a cancel or a replace is pending, and the position must
    // be right before the next decision.
    void on_executed(std::uint64_t token, Qty qty, Price /*price*/) noexcept {
        SideState* ss = by_token(token);
        if (!ss) ss = by_pending_token(token);
        if (!ss) return;
        const Side s = ss == &sides_[0] ? Side::Buy : Side::Sell;
        position_ += s == Side::Buy ? std::int64_t(qty) : -std::int64_t(qty);
        ++stats_.fills;
        stats_.filled_qty += qty;
        if (ss->token == token) {
            ss->qty = qty >= ss->qty ? 0 : ss->qty - qty;
            if (ss->qty == 0 && ss->state == QuoteState::Live) *ss = SideState{};
            // Pending states wait for their answer: the exchange will say what is left.
        } else {
            ss->pending_qty = qty >= ss->pending_qty ? 0 : ss->pending_qty - qty;
        }
    }
    void on_rejected(std::uint64_t token) noexcept {
        ++stats_.rejects;
        for (SideState& ss : sides_) {
            if (ss.state == QuoteState::PendingNew && ss.token == token) { ss = SideState{}; return; }
            if (ss.state == QuoteState::PendingReplace && ss.pending_token == token) {  // original stays live
                ss.state = ss.qty ? QuoteState::Live : QuoteState::Idle;
                ss.pending_token = 0;
                if (!ss.qty) ss = SideState{};
                return;
            }
        }
    }
    void on_cancel_rejected(std::uint64_t token) noexcept {
        ++stats_.cancel_rejects;
        if (SideState* ss = by_token(token); ss && ss->state == QuoteState::PendingCancel)
            *ss = ss->qty ? SideState{QuoteState::Live, ss->token, ss->price, ss->qty} : SideState{};
    }

    std::int64_t     position() const noexcept { return position_; }
    const SideState& side(Side s) const noexcept { return sides_[s == Side::Buy ? 0 : 1]; }
    const Stats&     stats() const noexcept { return stats_; }
    const QuoterParams& params() const noexcept { return p_; }

private:
    static std::int64_t floor_div(std::int64_t a, std::int64_t b) noexcept { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

    SideState& mut(Side s) noexcept { return sides_[s == Side::Buy ? 0 : 1]; }
    SideState* by_token(std::uint64_t t) noexcept {
        for (SideState& ss : sides_) if (ss.state != QuoteState::Idle && ss.token == t) return &ss;
        return nullptr;
    }
    SideState* by_pending_token(std::uint64_t t) noexcept {
        for (SideState& ss : sides_) if (ss.state == QuoteState::PendingReplace && ss.pending_token == t) return &ss;
        return nullptr;
    }
    static Intent cancel_of(Side s, const SideState& ss) noexcept { return {Intent::Kind::Cancel, s, ss.price, ss.qty, ss.token}; }

    int decide_side(Side s, bool want, Price desired, Intent* out) noexcept {
        SideState& ss = mut(s);
        switch (ss.state) {
            case QuoteState::PendingNew:
            case QuoteState::PendingReplace:
            case QuoteState::PendingCancel:
                return 0;  // one request in flight
            case QuoteState::Idle:
                if (!want) return 0;
                *out = {Intent::Kind::New, s, desired, p_.quote_qty, 0};
                return 1;
            case QuoteState::Live: {
                if (!want) { *out = cancel_of(s, ss); ++stats_.pulled; return 1; }
                const Price moved = desired > ss.price ? desired - ss.price : ss.price - desired;
                if (moved < p_.requote_threshold && ss.qty == p_.quote_qty) return 0;
                *out = {Intent::Kind::Replace, s, desired, p_.quote_qty, ss.token};
                return 1;
            }
        }
        return 0;
    }

    QuoterParams p_;
    SideState    sides_[2];
    std::int64_t position_ = 0;
    Stats        stats_;
};

}  // namespace lob
