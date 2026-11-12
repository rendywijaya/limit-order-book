// test_path.cpp — the whole path, end to end, on a small synthetic capture:
// it quotes, it gets filled, risk holds, and two threads replay identically.
#include <gtest/gtest.h>

#include <atomic>
#include <random>
#include <thread>
#include <vector>

#include "lob/path.hpp"
#include "lob/spsc.hpp"

using namespace lob;

namespace {

// A capture for two symbols with a drifting mid, in the stream format.
std::vector<std::uint8_t> capture(std::uint64_t seed, std::size_t n) {
    std::mt19937_64 rng(seed);
    std::vector<std::uint8_t> out;
    std::uint8_t buf[64];
    auto emit = [&](std::size_t k) { stream_append(out, {buf, k}); };
    std::uint64_t ns = 34'200'000'000'000ull, ref = 1, match = 1;
    emit(itch::encode_stock_directory(buf, 1, ns, "AAPL"));
    emit(itch::encode_stock_directory(buf, 2, ns, "MSFT"));
    struct Sym { std::uint16_t loc; const char* name; std::uint32_t mid; std::vector<std::uint64_t> live; std::vector<std::pair<char, std::uint32_t>> px; };
    Sym syms[2] = {{1, "AAPL", 1'500'000, {}, {}}, {2, "MSFT", 4'000'000, {}, {}}};
    while (out.size() < n) {
        Sym& s = syms[rng() % 2];
        ns += 1 + rng() % 20'000;
        int r = int(rng() % 100);
        if (s.live.size() < 50 || r < 50) {
            char side = rng() % 2 ? 'B' : 'S';
            int d = int(rng() % 4);
            std::uint32_t px = side == 'B' ? s.mid - 100 - 100 * std::uint32_t(d) : s.mid + 100 + 100 * std::uint32_t(d);
            emit(itch::encode_add(buf, s.loc, ns, ref, side, 100, s.name, px));
            s.live.push_back(ref); s.px.emplace_back(side, px); ++ref;
        } else if (r < 85) {
            std::size_t i = rng() % s.live.size();
            emit(itch::encode_delete(buf, s.loc, ns, s.live[i]));
            s.live.erase(s.live.begin() + long(i)); s.px.erase(s.px.begin() + long(i));
        } else if (r < 97) {
            std::size_t i = rng() % s.live.size();
            emit(itch::encode_executed(buf, s.loc, ns, s.live[i], 100, match++));
            s.live.erase(s.live.begin() + long(i)); s.px.erase(s.px.begin() + long(i));
        } else {
            s.mid += rng() % 2 ? 100 : std::uint32_t(-100);
        }
    }
    return out;
}

struct Outcome { std::uint64_t hash; Strategy::Counts c; std::int64_t position; std::uint64_t fills; std::uint64_t quoted; };

Outcome run_one_thread(const std::vector<std::uint8_t>& cap, const PathConfig& cfg) {
    FeedSide feed(cfg);
    std::unique_ptr<Strategy> strat;
    StageHistograms h;
    StreamReader rd(cap);
    MarketUpdate u;
    std::uint64_t seq = 0, quoted = 0;
    for (wire::Bytes m = rd.next(); !m.empty(); m = rd.next(), ++seq) {
        if (!feed.on_message(m, bench::ticks(), seq, u)) continue;
        ++quoted;
        if (!strat) { strat = std::make_unique<Strategy>(cfg, u.ev.locate); strat->start(u.itch_ns); }
        strat->on_update(u, u.t_feed_done, h, true);
    }
    return {strat->order_hash(), strat->counts(), strat->gateway().exposure().position, strat->sim().stats().fills, quoted};
}

Outcome run_two_threads(const std::vector<std::uint8_t>& cap, const PathConfig& cfg) {
    FeedSide feed(cfg);
    std::unique_ptr<Strategy> strat;
    StageHistograms h;
    auto ring = std::make_unique<SpscRing<MarketUpdate, 1 << 12>>();
    std::atomic<bool> done{false};
    std::thread t([&] {
        MarketUpdate u;
        for (;;) {
            if (ring->pop(u)) {
                if (!strat) { strat = std::make_unique<Strategy>(cfg, u.ev.locate); strat->start(u.itch_ns); }
                strat->on_update(u, bench::ticks(), h, true);
            } else if (done.load()) {
                if (!ring->pop(u)) break;
                if (!strat) { strat = std::make_unique<Strategy>(cfg, u.ev.locate); strat->start(u.itch_ns); }
                strat->on_update(u, bench::ticks(), h, true);
            }
        }
    });
    StreamReader rd(cap);
    MarketUpdate u;
    std::uint64_t seq = 0, quoted = 0;
    for (wire::Bytes m = rd.next(); !m.empty(); m = rd.next(), ++seq) {
        if (!feed.on_message(m, bench::ticks(), seq, u)) continue;
        ++quoted;
        while (!ring->push(u)) {}
    }
    done.store(true);
    t.join();
    return {strat->order_hash(), strat->counts(), strat->gateway().exposure().position, strat->sim().stats().fills, quoted};
}

}  // namespace

TEST(Path, QuotesGetsFilledAndStaysWithinLimits) {
    auto cap = capture(1, 2'000'000);
    PathConfig cfg;
    cfg.symbol = "AAPL";
    cfg.quoter = {.half_spread_ticks = 1, .quote_qty = 100, .skew_ticks_per_lot = 1, .requote_threshold = 1, .max_position = 300};
    cfg.risk = {.max_order_qty = 100, .collar_ticks = 20, .max_notional = 100'000'000, .max_open_orders = 4, .max_position = 300, .max_msgs_per_sec = 100'000};
    Outcome o = run_one_thread(cap, cfg);
    EXPECT_GT(o.quoted, 1000u);
    EXPECT_GT(o.c.top_changes, 100u);
    EXPECT_GT(o.c.sent_new, 10u);
    EXPECT_GT(o.fills, 5u);
    EXPECT_LE(o.position, 300);
    EXPECT_GE(o.position, -300);
    EXPECT_EQ(o.c.unsendable, 0u);
}

TEST(Path, OneThreadAndTwoThreadsSendIdenticalOrders) {
    auto cap = capture(2, 1'500'000);
    PathConfig cfg;
    cfg.symbol = "MSFT";
    cfg.quoter = {.half_spread_ticks = 1, .quote_qty = 100, .skew_ticks_per_lot = 1, .requote_threshold = 1, .max_position = 300};
    cfg.risk = {.max_order_qty = 100, .collar_ticks = 20, .max_notional = 100'000'000, .max_open_orders = 4, .max_position = 300, .max_msgs_per_sec = 100'000};
    Outcome a = run_one_thread(cap, cfg);
    Outcome b = run_two_threads(cap, cfg);
    EXPECT_EQ(a.hash, b.hash);
    EXPECT_EQ(a.c.sent_new, b.c.sent_new);
    EXPECT_EQ(a.c.sent_replace, b.c.sent_replace);
    EXPECT_EQ(a.position, b.position);
    EXPECT_EQ(a.fills, b.fills);
    EXPECT_GT(a.c.sent_new, 10u);
}

TEST(Path, RiskStopsTheQuoterAtItsLimits) {
    auto cap = capture(3, 1'000'000);
    PathConfig cfg;
    cfg.symbol = "AAPL";
    cfg.quoter = {.half_spread_ticks = 1, .quote_qty = 100, .skew_ticks_per_lot = 0, .requote_threshold = 1, .max_position = 100'000};  // the quoter would go on
    cfg.risk = {.max_order_qty = 100, .collar_ticks = 20, .max_notional = 100'000'000, .max_open_orders = 4, .max_position = 200, .max_msgs_per_sec = 100'000};
    Outcome o = run_one_thread(cap, cfg);
    EXPECT_LE(o.position, 200);
    EXPECT_GE(o.position, -200);
    EXPECT_GT(o.c.refused, 0u);  // risk had to say no
}
