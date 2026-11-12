// test_risk.cpp — every check, in its fixed order; cancels pass; the kill switch.
#include <gtest/gtest.h>

#include "lob/risk.hpp"

using namespace lob;

namespace {
RiskGate::TopRef market(Price bid, Price ask) { return {true, bid + ask}; }
}  // namespace

TEST(Risk, PassesAnOrdinaryOrder) {
    RiskGate g;
    Exposure x;
    EXPECT_EQ(g.check_new(Side::Buy, 9999, 100, market(9999, 10001), x, 0), RiskReason::Ok);
    EXPECT_EQ(g.stats().passed, 1u);
}

TEST(Risk, ChecksRunInAFixedOrderAndTheFirstFailureWins) {
    RiskGate g({.max_order_qty = 100, .collar_ticks = 5, .max_notional = 1'000, .max_open_orders = 1, .max_position = 100});
    Exposure x{.position = 100, .open_buy_qty = 0, .open_sell_qty = 0, .open_orders = 1};
    // An order that fails EVERY check: too big, outside the collar, too much
    // notional, too many open orders, too much position. Size is reported,
    // because it is checked first.
    EXPECT_EQ(g.check_new(Side::Buy, 20'000, 500, market(9999, 10001), x, 0), RiskReason::OrderSize);
    // Fix the size: the collar is next.
    EXPECT_EQ(g.check_new(Side::Buy, 20'000, 100, market(9999, 10001), x, 0), RiskReason::PriceCollar);
    // Inside the collar: notional (10000 * 100 > 1000).
    EXPECT_EQ(g.check_new(Side::Buy, 10'000, 100, market(9999, 10001), x, 0), RiskReason::Notional);
    // Small enough notional: open orders.
    EXPECT_EQ(g.check_new(Side::Buy, 10, 100, market(9, 11), x, 0), RiskReason::OpenOrders);
    x.open_orders = 0;
    // Position: 100 held + 100 more > 100.
    EXPECT_EQ(g.check_new(Side::Buy, 10, 100, market(9, 11), x, 0), RiskReason::Position);
    // Selling reduces it: fine.
    EXPECT_EQ(g.check_new(Side::Sell, 10, 100, market(9, 11), x, 0), RiskReason::Ok);
}

TEST(Risk, NoReferencePriceMeansNoNewOrders) {
    RiskGate g;
    Exposure x;
    EXPECT_EQ(g.check_new(Side::Buy, 100, 100, RiskGate::TopRef{}, x, 0), RiskReason::NoReference);
}

TEST(Risk, CollarIsSymmetricAndExactInHalfTicks) {
    RiskGate g({.collar_ticks = 3});
    Exposure x;
    // mid = 100.5 (bid 100, ask 101). Collar 3 ticks: 97.5 .. 103.5 → 98..103 pass, 97 and 104 fail.
    EXPECT_EQ(g.check_new(Side::Buy, 98, 10, market(100, 101), x, 0), RiskReason::Ok);
    EXPECT_EQ(g.check_new(Side::Buy, 97, 10, market(100, 101), x, 0), RiskReason::PriceCollar);
    EXPECT_EQ(g.check_new(Side::Sell, 103, 10, market(100, 101), x, 0), RiskReason::Ok);
    EXPECT_EQ(g.check_new(Side::Sell, 104, 10, market(100, 101), x, 0), RiskReason::PriceCollar);
}

TEST(Risk, WorstCasePositionCountsOpenOrders) {
    RiskGate g({.max_position = 300});
    Exposure x{.position = 100, .open_buy_qty = 100, .open_sell_qty = 0, .open_orders = 1};
    EXPECT_EQ(g.check_new(Side::Buy, 100, 100, market(99, 101), x, 0), RiskReason::Ok);        // 100 + 100 + 100 = 300
    EXPECT_EQ(g.check_new(Side::Buy, 100, 101, market(99, 101), x, 0), RiskReason::Position);  // 301
    // A replace releases the order's own open quantity first.
    EXPECT_EQ(g.check_replace(Side::Buy, 100, 200, 100, market(99, 101), x, 0), RiskReason::Ok);  // 100 + 0 + 200 = 300
    EXPECT_EQ(g.check_replace(Side::Buy, 100, 201, 100, market(99, 101), x, 0), RiskReason::Position);
    // Short side is symmetric.
    Exposure y{.position = -100, .open_buy_qty = 0, .open_sell_qty = 100, .open_orders = 1};
    EXPECT_EQ(g.check_new(Side::Sell, 100, 100, market(99, 101), y, 0), RiskReason::Ok);
    EXPECT_EQ(g.check_new(Side::Sell, 100, 101, market(99, 101), y, 0), RiskReason::Position);
}

TEST(Risk, MessageRateIsAOneSecondWindow) {
    RiskGate g({.max_msgs_per_sec = 3});
    Exposure x;
    const std::uint64_t s = 1'000'000'000ull;
    EXPECT_EQ(g.check_new(Side::Buy, 100, 1, market(99, 101), x, 10), RiskReason::Ok);
    EXPECT_EQ(g.check_new(Side::Buy, 100, 1, market(99, 101), x, 20), RiskReason::Ok);
    EXPECT_EQ(g.check_cancel(30), RiskReason::Ok);  // counted
    EXPECT_EQ(g.check_new(Side::Buy, 100, 1, market(99, 101), x, 40), RiskReason::MessageRate);
    EXPECT_EQ(g.check_cancel(50), RiskReason::Ok);  // but never refused
    EXPECT_EQ(g.check_new(Side::Buy, 100, 1, market(99, 101), x, 10 + s), RiskReason::Ok);  // new window
}

TEST(Risk, KillSwitchStopsEverythingButCancels) {
    RiskGate g;
    Exposure x;
    g.kill();
    EXPECT_TRUE(g.killed());
    EXPECT_EQ(g.check_new(Side::Buy, 100, 1, market(99, 101), x, 0), RiskReason::KillSwitch);
    EXPECT_EQ(g.check_replace(Side::Buy, 100, 1, 1, market(99, 101), x, 0), RiskReason::KillSwitch);
    EXPECT_EQ(g.check_cancel(0), RiskReason::Ok);
    g.arm();
    EXPECT_EQ(g.check_new(Side::Buy, 100, 1, market(99, 101), x, 0), RiskReason::Ok);
    EXPECT_EQ(g.stats().refused[int(RiskReason::KillSwitch)], 2u);
}
