// test_quoter.cpp — the market maker's decisions and its state machine, race by race.
#include <gtest/gtest.h>

#include "lob/quoter.hpp"

using namespace lob;
using K = Intent::Kind;

namespace {
TopOfBook top(Price bid, Price ask, QtySum bq = 100, QtySum aq = 100) {
    TopOfBook t;
    t.bid = bid; t.ask = ask; t.bid_qty = bq; t.ask_qty = aq; t.has_bid = t.has_ask = true;
    return t;
}
}  // namespace

TEST(Quoter, QuotesAroundTheMidAtTheConfiguredSpread) {
    Quoter q({.half_spread_ticks = 2, .quote_qty = 100});
    Intent it[2];
    // mid = 100.5; fair bid = 98.5 → 98 (floor), fair ask = 102.5 → 103 (ceil)
    ASSERT_EQ(q.decide(top(100, 101), it), 2);
    EXPECT_EQ(it[0].kind, K::New); EXPECT_EQ(it[0].side, Side::Buy); EXPECT_EQ(it[0].price, 98); EXPECT_EQ(it[0].qty, 100u);
    EXPECT_EQ(it[1].kind, K::New); EXPECT_EQ(it[1].side, Side::Sell); EXPECT_EQ(it[1].price, 103);
}

TEST(Quoter, NeverCrossesOrLocksTheMarket) {
    Quoter q({.half_spread_ticks = 0, .quote_qty = 100});  // would quote at the mid
    Intent it[2];
    ASSERT_EQ(q.decide(top(100, 101), it), 2);
    EXPECT_EQ(it[0].price, 100);  // bid: floor(100.5) = 100, which is < ask 101: allowed
    EXPECT_EQ(it[1].price, 101);  // ask: ceil(100.5) = 101 > bid 100: allowed
    Quoter w({.half_spread_ticks = 0, .quote_qty = 100});
    ASSERT_EQ(w.decide(top(100, 102), it), 2);  // mid 101 exactly: both sides would sit at 101
    EXPECT_EQ(it[0].price, 101); EXPECT_EQ(it[1].price, 102);  // the ask is pushed off our own bid
}

TEST(Quoter, OneRequestInFlightPerSideThenRequotesOnlyWhenWorthIt) {
    Quoter q({.half_spread_ticks = 2, .quote_qty = 100, .requote_threshold = 2});
    Intent it[2];
    ASSERT_EQ(q.decide(top(100, 101), it), 2);
    q.on_sent(it[0], 1);
    q.on_sent(it[1], 2);
    EXPECT_EQ(q.side(Side::Buy).state, QuoteState::PendingNew);
    EXPECT_EQ(q.decide(top(110, 111), it), 0);  // nothing while pending, however far the market moved
    q.on_accepted(1, 98, 100);
    q.on_accepted(2, 103, 100);
    EXPECT_EQ(q.side(Side::Buy).state, QuoteState::Live);
    EXPECT_EQ(q.decide(top(100, 102), it), 0);  // desired bid 99: moved 1 < threshold 2: stay
    EXPECT_EQ(q.decide(top(101, 102), it), 0);  // bid 99, ask 104: each moved 1: stay
}

TEST(Quoter, RequoteArithmetic) {
    Quoter q({.half_spread_ticks = 2, .quote_qty = 100, .requote_threshold = 2});
    Intent it[2];
    ASSERT_EQ(q.decide(top(100, 101), it), 2);
    q.on_sent(it[0], 1); q.on_sent(it[1], 2);
    q.on_accepted(1, 98, 100); q.on_accepted(2, 103, 100);
    EXPECT_EQ(q.decide(top(101, 102), it), 0);          // moved 1 tick each: under threshold
    ASSERT_EQ(q.decide(top(102, 103), it), 2);          // moved 2: replace both
    EXPECT_EQ(it[0].kind, K::Replace); EXPECT_EQ(it[0].token, 1u); EXPECT_EQ(it[0].price, 100);
    EXPECT_EQ(it[1].kind, K::Replace); EXPECT_EQ(it[1].token, 2u); EXPECT_EQ(it[1].price, 105);
    q.on_sent(it[0], 3);
    EXPECT_EQ(q.side(Side::Buy).state, QuoteState::PendingReplace);
    q.on_replaced(3, 100, 100);
    EXPECT_EQ(q.side(Side::Buy).state, QuoteState::Live);
    EXPECT_EQ(q.side(Side::Buy).token, 3u);
    EXPECT_EQ(q.side(Side::Buy).price, 100);
}

TEST(Quoter, SkewsAwayFromInventoryAndPullsAtTheLimit) {
    Quoter q({.half_spread_ticks = 2, .quote_qty = 100, .skew_ticks_per_lot = 1, .max_position = 200});
    Intent it[2];
    ASSERT_EQ(q.decide(top(100, 101), it), 2);
    q.on_sent(it[0], 1); q.on_sent(it[1], 2);
    q.on_accepted(1, 98, 100); q.on_accepted(2, 103, 100);
    // Fill the bid: long 100 = 1 lot → fair shifts down 1 tick: bid 97, ask 102.
    q.on_executed(1, 100, 98);
    EXPECT_EQ(q.position(), 100);
    EXPECT_EQ(q.side(Side::Buy).state, QuoteState::Idle);  // the order is gone
    ASSERT_EQ(q.decide(top(100, 101), it), 2);
    EXPECT_EQ(it[0].kind, K::New); EXPECT_EQ(it[0].price, 97);
    EXPECT_EQ(it[1].kind, K::Replace); EXPECT_EQ(it[1].price, 102);
    q.on_sent(it[0], 3); q.on_accepted(3, 97, 100);
    q.on_sent(it[1], 4); q.on_replaced(4, 102, 100);
    // Another fill: long 200 = the limit. The bid is pulled, the ask stays.
    q.on_executed(3, 100, 97);
    EXPECT_EQ(q.position(), 200);
    ASSERT_EQ(q.decide(top(100, 101), it), 1);
    EXPECT_EQ(it[0].side, Side::Sell);
    EXPECT_EQ(it[0].kind, K::Replace);  // skew 2 lots: ask at 101... clamped above bid: 101
    EXPECT_EQ(it[0].price, 101);
    EXPECT_EQ(q.side(Side::Buy).state, QuoteState::Idle);  // no new bid at the limit
}

TEST(Quoter, FillWhileCancelPendingThenCanceledZero) {
    Quoter q({.half_spread_ticks = 2, .quote_qty = 100});
    Intent it[2];
    q.decide(top(100, 101), it);
    q.on_sent(it[0], 1); q.on_accepted(1, 98, 100);
    q.on_sent(it[1], 2); q.on_accepted(2, 103, 100);
    // Market vanishes: pull both.
    TopOfBook none;
    ASSERT_EQ(q.decide(none, it), 2);
    EXPECT_EQ(it[0].kind, K::Cancel); EXPECT_EQ(it[0].token, 1u);
    q.on_sent(it[0], 1);
    EXPECT_EQ(q.side(Side::Buy).state, QuoteState::PendingCancel);
    // The fill races the cancel: position updates, side still waits for the answer.
    q.on_executed(1, 100, 98);
    EXPECT_EQ(q.position(), 100);
    EXPECT_EQ(q.side(Side::Buy).state, QuoteState::PendingCancel);
    EXPECT_EQ(q.decide(top(100, 101), it), 0 + 1);  // only the ask side can act (it is Live)
    // The exchange answers the cancel with nothing left to cancel.
    q.on_canceled(1, 0);
    EXPECT_EQ(q.side(Side::Buy).state, QuoteState::Idle);
}

TEST(Quoter, ReplaceRejectedLeavesTheOriginalLive) {
    Quoter q({.half_spread_ticks = 2, .quote_qty = 100, .requote_threshold = 1});
    Intent it[2];
    q.decide(top(100, 101), it);
    q.on_sent(it[0], 1); q.on_accepted(1, 98, 100);
    ASSERT_GE(q.decide(top(101, 102), it), 1);
    ASSERT_EQ(it[0].kind, K::Replace);
    q.on_sent(it[0], 7);
    q.on_rejected(7);
    EXPECT_EQ(q.side(Side::Buy).state, QuoteState::Live);
    EXPECT_EQ(q.side(Side::Buy).token, 1u);
    EXPECT_EQ(q.side(Side::Buy).price, 98);
    EXPECT_EQ(q.stats().rejects, 1u);
}

TEST(Quoter, FillDuringReplaceShowsUpInTheReplacedQuantity) {
    Quoter q({.half_spread_ticks = 2, .quote_qty = 100, .requote_threshold = 1});
    Intent it[2];
    q.decide(top(100, 101), it);
    q.on_sent(it[0], 1); q.on_accepted(1, 98, 100);
    q.decide(top(101, 102), it);
    q.on_sent(it[0], 7);                 // replace 1 → 7
    q.on_executed(1, 40, 98);            // 40 filled on the original meanwhile
    EXPECT_EQ(q.position(), 40);
    q.on_replaced(7, 99, 60);            // exchange: replacement rests with 60
    EXPECT_EQ(q.side(Side::Buy).state, QuoteState::Live);
    EXPECT_EQ(q.side(Side::Buy).qty, 60u);
    EXPECT_EQ(q.side(Side::Buy).token, 7u);
    // And a replace answered with nothing resting (everything filled) ends Idle.
    q.decide(top(102, 103), it);
    q.on_sent(it[0], 8);
    q.on_executed(7, 60, 99);
    q.on_replaced(8, 100, 0);
    EXPECT_EQ(q.side(Side::Buy).state, QuoteState::Idle);
    EXPECT_EQ(q.position(), 100);
}

TEST(Quoter, NewRejectedAndRefusedByRiskReturnToIdle) {
    Quoter q;
    Intent it[2];
    q.decide(top(100, 101), it);
    q.on_sent(it[0], 1);
    q.on_rejected(1);
    EXPECT_EQ(q.side(Side::Buy).state, QuoteState::Idle);
    q.decide(top(100, 101), it);
    q.on_refused(it[0]);
    EXPECT_EQ(q.side(Side::Buy).state, QuoteState::Idle);
}
