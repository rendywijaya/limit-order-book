// test_feed.cpp — the feed handler and market book against a reference, over
// random multi-symbol ITCH streams; plus the stream format.
#include <gtest/gtest.h>

#include <map>
#include <random>
#include <unordered_map>
#include <vector>

#include "lob/feed.hpp"

using namespace lob;

namespace {

// A reference that is too simple to be wrong: a map of live orders and, per
// symbol and side, a map from price to (total, count).
struct RefBook {
    struct O { std::uint16_t locate; char side; Price price; Qty qty; };
    std::unordered_map<std::uint64_t, O> orders;
    std::map<Price, std::pair<QtySum, std::uint32_t>> bids[64], asks[64];

    auto& side_map(std::uint16_t loc, char s) { return s == 'B' ? bids[loc] : asks[loc]; }
    void add(std::uint16_t loc, std::uint64_t ref, char s, Price p, Qty q) {
        orders[ref] = {loc, s, p, q};
        auto& lv = side_map(loc, s)[p]; lv.first += q; lv.second += 1;
    }
    void shrink(std::uint64_t ref, Qty q) {
        auto it = orders.find(ref);
        if (it == orders.end()) return;
        O& o = it->second;
        auto& m = side_map(o.locate, o.side);
        auto& lv = m[o.price];
        if (q >= o.qty) { lv.first -= o.qty; lv.second -= 1; if (lv.second == 0) m.erase(o.price); orders.erase(it); }
        else { lv.first -= q; o.qty -= q; }
    }
    void del(std::uint64_t ref) { shrink(ref, UINT32_MAX); }
    void replace(std::uint64_t ref, std::uint64_t nref, Price p, Qty q) {
        auto it = orders.find(ref);
        if (it == orders.end()) return;
        O o = it->second;
        del(ref);
        add(o.locate, nref, o.side, p, q);
    }
    std::vector<LevelView> depth(std::uint16_t loc, char s) {
        std::vector<LevelView> out;
        auto& m = side_map(loc, s);
        if (s == 'B') for (auto it = m.rbegin(); it != m.rend(); ++it) out.push_back({it->first, it->second.first, it->second.second});
        else for (auto& [p, lv] : m) out.push_back({p, lv.first, lv.second});
        return out;
    }
};

// Random ITCH stream over `nsym` symbols. Returns the message-stream bytes.
std::vector<std::uint8_t> random_stream(std::uint64_t seed, std::size_t n, int nsym) {
    std::mt19937_64 rng(seed);
    auto pct = [&] { return int(rng() % 100); };
    std::vector<std::uint8_t> out;
    std::uint8_t buf[64];
    std::vector<std::vector<std::uint64_t>> live(static_cast<std::size_t>(nsym));
    std::uint64_t ref = 1, ns = 34'200'000'000'000ull;
    for (int s = 0; s < nsym; ++s) {
        std::size_t k = itch::encode_stock_directory(buf, std::uint16_t(s + 1), ns, "SYM" + std::to_string(s));
        stream_append(out, {buf, k});
    }
    while (out.size() < n) {
        int s = int(rng() % std::size_t(nsym));
        auto loc = std::uint16_t(s + 1);
        auto& lv = live[std::size_t(s)];
        ns += rng() % 50'000;
        int r = pct();
        std::size_t k;
        if (lv.empty() || r < 45) {
            char side = rng() % 2 ? 'B' : 'S';
            std::uint32_t price = 1'000'000 + 100 * std::uint32_t(rng() % 40) + (pct() < 2 ? 50 : 0);  // 2% sub-penny
            k = itch::encode_add(buf, loc, ns, ref, side, 100 * std::uint32_t(1 + rng() % 5), "SYM" + std::to_string(s), price);
            lv.push_back(ref++);
        } else if (r < 65) {
            std::uint64_t o = lv[rng() % lv.size()];
            k = itch::encode_delete(buf, loc, ns, o);
            lv.erase(std::find(lv.begin(), lv.end(), o));
        } else if (r < 80) {
            std::uint64_t o = lv[rng() % lv.size()];
            k = itch::encode_executed(buf, loc, ns, o, 100 * std::uint32_t(1 + rng() % 3), ref++);
            // may or may not finish the order; the reference decides, so keep it listed
        } else if (r < 90) {
            std::uint64_t o = lv[rng() % lv.size()];
            k = itch::encode_cancel(buf, loc, ns, o, 100 * std::uint32_t(1 + rng() % 2));
        } else if (r < 97) {
            std::uint64_t o = lv[rng() % lv.size()];
            std::uint32_t price = 1'000'000 + 100 * std::uint32_t(rng() % 40);
            k = itch::encode_replace(buf, loc, ns, o, ref, 100 * std::uint32_t(1 + rng() % 5), price);
            lv.erase(std::find(lv.begin(), lv.end(), o));
            lv.push_back(ref++);
        } else if (r < 99) {
            k = itch::encode_trade(buf, loc, ns, 'B', 100, "SYM" + std::to_string(s), 1'001'000, ref++);
        } else {
            k = itch::encode_delete(buf, loc, ns, 999'999'999ull + ref);  // unknown reference
        }
        stream_append(out, {buf, k});
    }
    return out;
}

}  // namespace

TEST(Feed, MatchesTheReferenceAfterEveryMessage) {
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        const int nsym = 3;
        auto bytes = random_stream(seed, 200'000, nsym);
        FeedHandler fh({.expected_orders = 64, .book_levels = 1 << 12});  // tiny reservations: growth paths run
        RefBook ref;
        StreamReader rd(bytes);
        itch::Msg m;
        std::size_t n = 0;
        for (wire::Bytes msg = rd.next(); !msg.empty(); msg = rd.next(), ++n) {
            ASSERT_TRUE(itch::decode(msg, m));
            FeedEvent ev = fh.on_message(msg);
            // Mirror into the reference, applying the same sub-penny rule.
            auto ticks = [](std::uint32_t raw) -> std::optional<Price> { return raw % 100 ? std::nullopt : std::optional(Price(raw / 100)); };
            switch (m.type) {
                case 'A': if (auto p = ticks(m.price)) ref.add(m.locate, m.ref, m.side, *p, m.shares); break;
                case 'E': ref.shrink(m.ref, m.shares); break;
                case 'X': ref.shrink(m.ref, m.shares); break;
                case 'D': ref.del(m.ref); break;
                case 'U': if (auto p = ticks(m.price)) ref.replace(m.ref, m.new_ref, *p, m.shares); break;
                default: break;
            }
            EXPECT_EQ(fh.live_orders(), ref.orders.size()) << "seed " << seed << " msg " << n;
            if (m.type == 'A' || m.type == 'E' || m.type == 'X' || m.type == 'D' || m.type == 'U') {
                const MarketBook* b = fh.book(m.locate);
                if (!b) continue;
                ASSERT_EQ(b->depth(Side::Buy), ref.depth(m.locate, 'B')) << "seed " << seed << " msg " << n << " type " << m.type;
                ASSERT_EQ(b->depth(Side::Sell), ref.depth(m.locate, 'S')) << "seed " << seed << " msg " << n;
                // Top of book agrees with depth.
                auto db = b->depth(Side::Buy, 1), da = b->depth(Side::Sell, 1);
                EXPECT_EQ(b->top().has_bid, !db.empty());
                if (!db.empty()) { EXPECT_EQ(b->top().bid, db[0].price); EXPECT_EQ(b->top().bid_qty, db[0].total_qty); }
                EXPECT_EQ(b->top().has_ask, !da.empty());
                if (!da.empty()) { EXPECT_EQ(b->top().ask, da[0].price); EXPECT_EQ(b->top().ask_qty, da[0].total_qty); }
                (void)ev;
            }
        }
        EXPECT_GT(fh.stats().unknown_ref, 0u);
        EXPECT_GT(fh.stats().bad_price, 0u);
        EXPECT_EQ(fh.stats().books, std::uint64_t(nsym));
    }
}

TEST(Feed, TopChangedIsReportedExactlyWhenTheTopMoves) {
    FeedHandler fh;
    std::uint8_t buf[64];
    auto send = [&](std::size_t k) { return fh.on_message({buf, k}); };
    EXPECT_FALSE(send(itch::encode_add(buf, 1, 1, 10, 'B', 100, "X", 1'000'000)).top_changed == false);  // first bid: top changes
    EXPECT_FALSE(send(itch::encode_add(buf, 1, 2, 11, 'B', 100, "X", 990'000)).top_changed);             // worse bid: no
    EXPECT_TRUE(send(itch::encode_add(buf, 1, 3, 12, 'B', 100, "X", 1'000'000)).top_changed);             // same best price, more size: yes
    EXPECT_TRUE(send(itch::encode_add(buf, 1, 4, 13, 'S', 100, "X", 1'010'000)).top_changed);             // first ask
    EXPECT_FALSE(send(itch::encode_cancel(buf, 1, 5, 11, 50)).top_changed);                               // shrink a worse level: no
    EXPECT_TRUE(send(itch::encode_delete(buf, 1, 6, 10)).top_changed);                                   // best-level size changes: yes
    EXPECT_TRUE(send(itch::encode_delete(buf, 1, 7, 12)).top_changed);                                   // best level gone: bid falls to 99.00
    const MarketBook* b = fh.book(1);
    ASSERT_TRUE(b);
    EXPECT_EQ(b->top().bid, 99'00);
    EXPECT_EQ(b->top().bid_qty, 50u);
    EXPECT_EQ(b->top().ask, 101'00);
    EXPECT_EQ(b->top().mid2(), 200'00);
    EXPECT_EQ(b->top().spread(), 2'00);
}

TEST(Feed, ExecutionsReportTradesAndDirectoryNamesSymbols) {
    FeedHandler fh;
    std::uint8_t buf[64];
    fh.on_message({buf, itch::encode_stock_directory(buf, 7, 1, "AAPL")});
    EXPECT_EQ(fh.locate_of("AAPL"), 7);
    EXPECT_EQ(fh.locate_of("MSFT"), -1);
    fh.on_message({buf, itch::encode_add(buf, 7, 2, 100, 'S', 300, "AAPL", 1'500'000)});
    FeedEvent ev = fh.on_message({buf, itch::encode_executed(buf, 7, 3, 100, 100, 1)});
    EXPECT_TRUE(ev.is_trade);
    EXPECT_EQ(ev.trade_price, 150'00);
    EXPECT_EQ(ev.trade_qty, 100u);
    EXPECT_TRUE(ev.top_changed);
    EXPECT_EQ(fh.book(7)->top().ask_qty, 200u);
    ev = fh.on_message({buf, itch::encode_trade(buf, 7, 4, 'B', 50, "AAPL", 1'499'550, 2)});
    EXPECT_FALSE(ev.is_trade);  // sub-penny print: counted, not representable
    EXPECT_EQ(fh.stats().bad_price, 1u);
}

TEST(Stream, ReaderRoundTripsAndStopsAtTruncation) {
    std::vector<std::uint8_t> s;
    std::uint8_t buf[64];
    stream_append(s, {buf, itch::encode_system_event(buf, 1, 'O')});
    stream_append(s, {buf, itch::encode_add(buf, 1, 2, 5, 'B', 100, "X", 1'000'000)});
    s.push_back(0); s.push_back(36); s.push_back('A');  // a truncated third record
    StreamReader rd(s);
    EXPECT_EQ(rd.next().size(), 12u);
    EXPECT_EQ(rd.next().size(), 36u);
    EXPECT_TRUE(rd.next().empty());
    EXPECT_TRUE(rd.done());
    rd.rewind();
    EXPECT_EQ(rd.next().size(), 12u);
}
