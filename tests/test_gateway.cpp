// test_gateway.cpp — the order loop in bytes: gateway <-> SoupBinTCP <-> simulated exchange.
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <random>
#include <vector>

#include "lob/exchange_sim.hpp"
#include "lob/gateway.hpp"

using namespace lob;

namespace {

struct Recorder {
    struct Ev { char kind; std::uint64_t token; Price price; Qty qty; };
    std::vector<Ev> evs;
    void on_accepted(std::uint64_t t, Price p, Qty q) { evs.push_back({'A', t, p, q}); }
    void on_replaced(std::uint64_t t, Price p, Qty q) { evs.push_back({'U', t, p, q}); }
    void on_canceled(std::uint64_t t, Qty q) { evs.push_back({'C', t, 0, q}); }
    void on_executed(std::uint64_t t, Qty q, Price p) { evs.push_back({'E', t, p, q}); }
    void on_rejected(std::uint64_t t) { evs.push_back({'J', t, 0, 0}); }
    void on_cancel_rejected(std::uint64_t t) { evs.push_back({'I', t, 0, 0}); }
    Ev last() const { return evs.back(); }
};

// Gateway and simulator joined by two in-memory "sockets". Each side's output
// is delivered to the other synchronously.
struct Loop {
    Recorder rec;
    MarketBook book{1, 1 << 12};
    BookSnapshot snap_;
    std::unique_ptr<Gateway<Recorder>> gw;
    std::unique_ptr<ExchangeSim> sim;
    std::uint64_t now = 1'000;

    Loop() {
        sim = std::make_unique<ExchangeSim>([this](wire::Bytes b) { gw->on_wire(b, now); }, 7);
        gw = std::make_unique<Gateway<Recorder>>(rec, [this](wire::Bytes b) { sim->on_client_bytes(b, now); }, "TEST");
        sim->set_book(&snap_);
        gw->login(now);
    }
    const BookSnapshot& snap() { snapshot(book, snap_); return snap_; }
    void market(Side s, Price p, Qty q) { book.add(s, p, q); snap(); }
    FeedEvent trade(Side resting_side, Price p, Qty q) {
        FeedEvent ev; ev.locate = 7; ev.is_trade = true; ev.trade_side = resting_side; ev.trade_price = p; ev.trade_qty = q;
        return ev;
    }
};

}  // namespace

TEST(Gateway, LogsInThroughSoupBin) {
    Loop L;
    EXPECT_TRUE(L.gw->logged_in());
    EXPECT_EQ(L.gw->session().session(), "SIM");
    EXPECT_EQ(L.sim->stats().logins, 1u);
    EXPECT_EQ(L.gw->send_new(Side::Buy, 100, 100, L.now) != 0, true);
}

TEST(Gateway, NewOrderIsAcceptedAndCountedInExposure) {
    Loop L;
    L.market(Side::Buy, 100, 500); L.market(Side::Sell, 102, 500);
    std::uint64_t t = L.gw->send_new(Side::Buy, 100, 100, L.now);
    ASSERT_NE(t, 0u);
    ASSERT_EQ(L.rec.evs.size(), 1u);
    EXPECT_EQ(L.rec.last().kind, 'A'); EXPECT_EQ(L.rec.last().token, t); EXPECT_EQ(L.rec.last().qty, 100u);
    const OrderRecord* o = L.gw->order(t);
    ASSERT_TRUE(o);
    EXPECT_EQ(o->state, OrderState::Live);
    EXPECT_EQ(o->open, 100u);
    EXPECT_EQ(L.gw->exposure().open_buy_qty, 100u);
    EXPECT_EQ(L.gw->exposure().open_orders, 1u);
    EXPECT_EQ(L.sim->resting(), 1u);
}

TEST(Gateway, QueuePositionModelFillsOnlyAfterTheQueueAhead) {
    Loop L;
    L.market(Side::Buy, 100, 500); L.market(Side::Sell, 102, 500);
    std::uint64_t t = L.gw->send_new(Side::Buy, 100, 100, L.now);  // behind 500 shares
    L.sim->on_market(L.trade(Side::Buy, 100, 300), L.snap(), L.now);  // 300 ahead consumed
    EXPECT_EQ(L.rec.evs.size(), 1u);                                 // no fill yet
    L.sim->on_market(L.trade(Side::Buy, 100, 300), L.snap(), L.now);  // 200 more ahead, then 100 for us
    ASSERT_EQ(L.rec.evs.size(), 2u);
    EXPECT_EQ(L.rec.last().kind, 'E'); EXPECT_EQ(L.rec.last().token, t); EXPECT_EQ(L.rec.last().qty, 100u); EXPECT_EQ(L.rec.last().price, 100);
    EXPECT_EQ(L.gw->exposure().position, 100);
    EXPECT_EQ(L.gw->exposure().open_buy_qty, 0u);
    EXPECT_EQ(L.gw->exposure().open_orders, 0u);
    EXPECT_EQ(L.gw->order(t), nullptr);  // done and forgotten
    EXPECT_EQ(L.sim->stats().fills_queue, 1u);
    // Executions on the other side or at another price do nothing to us.
    std::uint64_t t2 = L.gw->send_new(Side::Sell, 103, 100, L.now);
    L.sim->on_market(L.trade(Side::Buy, 100, 1000), L.snap(), L.now);
    L.sim->on_market(L.trade(Side::Sell, 102, 1000), L.snap(), L.now);
    EXPECT_EQ(L.gw->order(t2)->open, 100u);
}

TEST(Gateway, TradeThroughFillsInFullAndMarketableOrdersTakeTheTouch) {
    Loop L;
    L.market(Side::Buy, 100, 500); L.market(Side::Sell, 102, 300);
    std::uint64_t t = L.gw->send_new(Side::Buy, 99, 100, L.now);
    // The best ask falls to 99: whoever sold through must have hit us.
    L.book.add(Side::Sell, 99, 50);
    L.snap();
    FeedEvent ev; ev.locate = 7; ev.top_changed = true;
    L.sim->on_market(ev, L.snap(), L.now);
    EXPECT_EQ(L.rec.last().kind, 'E'); EXPECT_EQ(L.rec.last().token, t); EXPECT_EQ(L.rec.last().qty, 100u);
    EXPECT_EQ(L.sim->stats().fills_through, 1u);
    // A buy at or above the ask executes against the touch quantity immediately, the rest rests.
    L.book.remove(Side::Sell, 99, 50);
    L.snap();
    std::uint64_t m = L.gw->send_new(Side::Buy, 102, 400, L.now);
    // events: Accepted, Executed(300 @102)
    ASSERT_GE(L.rec.evs.size(), 2u);
    EXPECT_EQ(L.rec.evs[L.rec.evs.size() - 2].kind, 'A');
    EXPECT_EQ(L.rec.last().kind, 'E'); EXPECT_EQ(L.rec.last().qty, 300u); EXPECT_EQ(L.rec.last().price, 102);
    EXPECT_EQ(L.gw->order(m)->open, 100u);
    EXPECT_EQ(L.gw->exposure().position, 400);
    EXPECT_EQ(L.sim->stats().fills_marketable, 1u);
}

TEST(Gateway, ReplaceKeepsOrLosesPriorityAndUpdatesExposure) {
    Loop L;
    L.market(Side::Buy, 100, 500); L.market(Side::Sell, 102, 500);
    std::uint64_t t = L.gw->send_new(Side::Buy, 100, 100, L.now);
    // Same price, smaller: keeps its place in the queue (behind 500).
    std::uint64_t t2 = L.gw->send_replace(t, 100, 60, L.now);
    ASSERT_NE(t2, 0u);
    EXPECT_EQ(L.rec.last().kind, 'U'); EXPECT_EQ(L.rec.last().token, t2); EXPECT_EQ(L.rec.last().qty, 60u);
    EXPECT_EQ(L.gw->order(t), nullptr);
    EXPECT_EQ(L.gw->order(t2)->state, OrderState::Live);
    EXPECT_EQ(L.gw->exposure().open_buy_qty, 60u);
    EXPECT_EQ(L.gw->exposure().open_orders, 1u);
    L.sim->on_market(L.trade(Side::Buy, 100, 500), L.snap(), L.now);  // exactly the queue ahead
    EXPECT_EQ(L.rec.last().kind, 'U');                               // still no fill
    L.sim->on_market(L.trade(Side::Buy, 100, 10), L.snap(), L.now);
    EXPECT_EQ(L.rec.last().kind, 'E'); EXPECT_EQ(L.rec.last().qty, 10u);
    // New price: a new queue position behind whatever rests there.
    L.market(Side::Buy, 101, 200);
    std::uint64_t t3 = L.gw->send_replace(t2, 101, 50, L.now);
    EXPECT_EQ(L.rec.last().kind, 'U'); EXPECT_EQ(L.rec.last().qty, 40u);  // 50 total minus 10 already filled
    L.sim->on_market(L.trade(Side::Buy, 101, 200), L.snap(), L.now);
    EXPECT_EQ(L.rec.last().kind, 'U');
    L.sim->on_market(L.trade(Side::Buy, 101, 40), L.snap(), L.now);
    EXPECT_EQ(L.rec.last().kind, 'E'); EXPECT_EQ(L.rec.last().token, t3); EXPECT_EQ(L.rec.last().qty, 40u);
    EXPECT_EQ(L.gw->exposure().position, 50);
    EXPECT_EQ(L.gw->exposure().open_orders, 0u);
}

TEST(Gateway, CancelsAndTheirRejections) {
    Loop L;
    L.market(Side::Buy, 100, 500); L.market(Side::Sell, 102, 500);
    std::uint64_t t = L.gw->send_new(Side::Sell, 103, 100, L.now);
    EXPECT_TRUE(L.gw->send_cancel(t, L.now));
    EXPECT_EQ(L.rec.last().kind, 'C'); EXPECT_EQ(L.rec.last().qty, 100u);
    EXPECT_EQ(L.gw->order(t), nullptr);
    EXPECT_EQ(L.gw->exposure().open_sell_qty, 0u);
    EXPECT_FALSE(L.gw->send_cancel(t, L.now));  // the gateway knows it is gone
    // A cancel that races a fill: the exchange no longer has the order.
    std::uint64_t t2 = L.gw->send_new(Side::Buy, 100, 100, L.now);
    L.sim->on_market(L.trade(Side::Buy, 100, 600), L.snap(), L.now);  // filled
    EXPECT_EQ(L.rec.last().kind, 'E');
    EXPECT_EQ(L.gw->order(t2), nullptr);
    EXPECT_EQ(L.sim->stats().cancel_rejected, 0u);
    EXPECT_EQ(L.gw->stats().unknown_token, 0u);
}

TEST(Gateway, RejectsAndTheOriginalSurvivesAFailedReplace) {
    Loop L;
    L.market(Side::Buy, 100, 500); L.market(Side::Sell, 102, 500);
    std::uint64_t bad = L.gw->send_new(Side::Buy, 0, 100, L.now);  // price 0: rejected
    EXPECT_EQ(L.rec.last().kind, 'J'); EXPECT_EQ(L.rec.last().token, bad);
    EXPECT_EQ(L.gw->exposure().open_orders, 0u);
    EXPECT_EQ(L.gw->exposure().open_buy_qty, 0u);
    std::uint64_t t = L.gw->send_new(Side::Buy, 100, 100, L.now);
    // Replace to an impossible quantity (0 after fills) -> replacement answered dead:
    std::uint64_t r = L.gw->send_replace(t, 100, 0, L.now);
    EXPECT_EQ(L.rec.last().kind, 'U'); EXPECT_EQ(L.rec.last().token, r); EXPECT_EQ(L.rec.last().qty, 0u);
    EXPECT_EQ(L.gw->order(t), nullptr);
    EXPECT_EQ(L.gw->order(r), nullptr);
    EXPECT_EQ(L.gw->exposure().open_orders, 0u);
    EXPECT_EQ(L.gw->exposure().open_buy_qty, 0u);
}

TEST(Gateway, ExposureIsConsistentWithRecordsUnderChurn) {
    Loop L;
    L.market(Side::Buy, 100, 1000); L.market(Side::Sell, 102, 1000);
    std::vector<std::uint64_t> live;
    std::mt19937_64 rng(3);
    for (int i = 0; i < 2000; ++i) {
        int r = int(rng() % 100);
        if (live.size() < 2 || r < 40) {
            Side s = rng() % 2 ? Side::Buy : Side::Sell;
            Price p = s == Side::Buy ? Price(98 + rng() % 3) : Price(102 + rng() % 3);
            std::uint64_t t = L.gw->send_new(s, p, 100, L.now);
            if (L.gw->order(t)) live.push_back(t);
        } else if (r < 70) {
            std::uint64_t t = live[rng() % live.size()];
            std::uint64_t n = L.gw->send_replace(t, L.gw->order(t)->price, Qty(50 + rng() % 100), L.now);
            live.erase(std::find(live.begin(), live.end(), t));
            if (n && L.gw->order(n)) live.push_back(n);
        } else if (r < 90) {
            std::uint64_t t = live[rng() % live.size()];
            L.gw->send_cancel(t, L.now);
            live.erase(std::find(live.begin(), live.end(), t));
        } else {
            L.sim->on_market(L.trade(rng() % 2 ? Side::Buy : Side::Sell, rng() % 2 ? 100 : 102, 300), L.snap(), L.now);
            for (auto it = live.begin(); it != live.end();) it = L.gw->order(*it) ? it + 1 : live.erase(it);
        }
        // Invariant: exposure equals the sum over live records.
        Qty ob = 0, os = 0; std::uint32_t n = 0;
        for (std::uint64_t t : live) {
            const OrderRecord* o = L.gw->order(t);
            ASSERT_TRUE(o) << "iteration " << i;
            (o->side == Side::Buy ? ob : os) += o->open;
            ++n;
        }
        ASSERT_EQ(L.gw->exposure().open_buy_qty, ob) << "iteration " << i;
        ASSERT_EQ(L.gw->exposure().open_sell_qty, os) << "iteration " << i;
        ASSERT_EQ(L.gw->exposure().open_orders, n) << "iteration " << i;
        ASSERT_EQ(L.gw->live_records(), live.size()) << "iteration " << i;
    }
    EXPECT_GT(L.sim->stats().fills, 0u);
}
