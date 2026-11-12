// test_alloc.cpp — "no allocation on the hot path" is a claim; this makes it a test.
//
// The global operator new/delete are replaced with counting versions. A
// realistic stream of messages (adds near the touch, cancels, modifies,
// aggressive orders) is replayed through each book, and the number of heap
// calls made *during* the replay is counted.
//
//   v2 must make zero.  v1 is expected to make hundreds of thousands.
//
// The count is per message submitted, measured around submit() only, so
// GoogleTest's own allocations do not leak into it.
#include <gtest/gtest.h>

// The replacement operators below pair malloc with free on purpose. GCC's
// -Wmismatched-new-delete sees `new` at a call site and `free` in the inlined
// operator delete and reports a mismatch that is not one.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

#include <atomic>
#include <cstdlib>
#include <memory>
#include <new>
#include <random>
#include <vector>

#include "lob/book_v1.hpp"
#include "lob/book_v2.hpp"

#include <execinfo.h>

namespace {

std::atomic<std::uint64_t> g_news{0}, g_deletes{0};
std::atomic<bool>          g_trace{false};  // print the stack of the next allocation (LOB_TRACE_ALLOC=1)

}  // namespace

void* operator new(std::size_t n) {
    g_news.fetch_add(1, std::memory_order_relaxed);
    if (g_trace.exchange(false, std::memory_order_relaxed)) {
        void* bt[40];
        int depth = backtrace(bt, 40);
        backtrace_symbols_fd(bt, depth, 2);
    }
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept {
    g_deletes.fetch_add(1, std::memory_order_relaxed);
    if (g_trace.exchange(false, std::memory_order_relaxed)) {
        void* bt[40];
        int depth = backtrace(bt, 40);
        backtrace_symbols_fd(bt, depth, 2);
    }
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
    g_deletes.fetch_add(1, std::memory_order_relaxed);
    std::free(p);
}

namespace {

using namespace lob;

// A compact version of bench/gen_day's mix: ~50% passive adds, ~38% cancels
// of live orders, ~6% modifies, ~6% aggressive orders.
std::vector<Command> realistic_day(std::size_t n, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> u(0, 1);
    std::geometric_distribution<int> depth(0.25);
    std::vector<Command> out;
    std::vector<OrderId> live;
    OrderId next = 1;
    const Price mid = 20'000;
    while (out.size() < n) {
        Command c{};
        c.side = u(rng) < 0.5 ? Side::Buy : Side::Sell;
        double r = u(rng);
        if (live.size() < 1000 || r < 0.50) {
            c.type = MsgType::New; c.otype = OrderType::Limit; c.id = next++;
            c.price = c.side == Side::Buy ? mid - 1 - depth(rng) : mid + 1 + depth(rng);
            c.qty = 100;
            live.push_back(c.id);
        } else if (r < 0.56) {
            c.type = MsgType::New; c.id = next++;
            c.otype = u(rng) < 0.5 ? OrderType::Limit : OrderType::IOC;
            c.price = c.side == Side::Buy ? mid + 1 + depth(rng) : mid - 1 - depth(rng);
            c.qty = 100;
        } else if (r < 0.94) {
            c.type = MsgType::Cancel;
            std::size_t i = rng() % live.size();
            c.id = live[i]; live[i] = live.back(); live.pop_back();
        } else {
            c.type = MsgType::Modify;
            c.id = live[rng() % live.size()];
            c.qty = 50; c.price = c.side == Side::Buy ? mid - 1 - depth(rng) : mid + 1 + depth(rng);
        }
        out.push_back(c);
    }
    return out;
}

template <class Book>
std::uint64_t heap_calls_during_replay(const std::vector<Command>& day, const BookConfig& cfg) {
    CountingListener l;
    Book book(l, cfg);  // construction may allocate: that is set-up, not the hot path
    const std::uint64_t n0 = g_news.load(), d0 = g_deletes.load();
    for (const Command& c : day) book.submit(c);
    return (g_news.load() - n0) + (g_deletes.load() - d0);
}

}  // namespace

TEST(Allocation, V2MakesNoHeapCallsOnTheHotPath) {
    auto day = realistic_day(200'000, 7);
    BookConfig cfg{.min_price = 1, .max_price = 1 << 16, .expected_orders = 1 << 16};
    EXPECT_EQ(heap_calls_during_replay<BookV2<CountingListener>>(day, cfg), 0u);
}

TEST(Allocation, V1AllocatesPerMessage) {
    auto day = realistic_day(200'000, 7);
    BookConfig cfg{.min_price = 1, .max_price = 1 << 16, .expected_orders = 1 << 16};
    auto calls = heap_calls_during_replay<BookV1<CountingListener>>(day, cfg);
    // Roughly: one list node + one hash node per resting add, freed on cancel
    // or fill, plus map nodes when levels appear and disappear.
    EXPECT_GT(calls, day.size());
    std::printf("v1 heap calls during %zu messages: %llu (%.2f per message)\n", day.size(),
                (unsigned long long)calls, double(calls) / double(day.size()));
}

TEST(Allocation, V2StillAllocatesWhenItOutgrowsItsReservation) {
    // Honesty check: the guarantee holds only within the configured capacity.
    auto day = realistic_day(200'000, 7);
    BookConfig cfg{.min_price = 1, .max_price = 1 << 16, .expected_orders = 8};
    EXPECT_GT(heap_calls_during_replay<BookV2<CountingListener>>(day, cfg), 0u);
}

// ---- the whole path ---------------------------------------------------------
// Every component between a market-data message and the order bytes must be
// allocation-free in steady state: feed handler, snapshot, quoter, risk,
// gateway, SoupBinTCP framing, the simulated exchange's answers back.
#include "lob/path.hpp"

namespace {
std::vector<std::uint8_t> path_capture(std::size_t n) {
    std::mt19937_64 rng(11);
    std::vector<std::uint8_t> out;
    std::uint8_t buf[64];
    auto emit = [&](std::size_t k) { lob::stream_append(out, {buf, k}); };
    std::uint64_t ns = 34'200'000'000'000ull, ref = 1, match = 1;
    emit(lob::itch::encode_stock_directory(buf, 1, ns, "AAPL"));
    std::vector<std::uint64_t> live;
    std::uint32_t mid = 1'500'000;
    while (out.size() < n) {
        ns += 1 + rng() % 20'000;
        int r = int(rng() % 100);
        if (live.size() < 50 || r < 50) {
            char side = rng() % 2 ? 'B' : 'S';
            std::uint32_t px = side == 'B' ? mid - 100 - 100 * std::uint32_t(rng() % 4) : mid + 100 + 100 * std::uint32_t(rng() % 4);
            emit(lob::itch::encode_add(buf, 1, ns, ref, side, 100, "AAPL", px));
            live.push_back(ref++);
        } else if (r < 85) {
            std::size_t i = rng() % live.size();
            emit(lob::itch::encode_delete(buf, 1, ns, live[i]));
            live.erase(live.begin() + long(i));
        } else if (r < 97) {
            std::size_t i = rng() % live.size();
            emit(lob::itch::encode_executed(buf, 1, ns, live[i], 100, match++));
            live.erase(live.begin() + long(i));
        } else {
            mid += rng() % 2 ? 100 : std::uint32_t(-100);
        }
    }
    return out;
}
}  // namespace

TEST(Allocation, TheWholePathMakesNoHeapCallsAfterWarmUp) {
    auto cap = path_capture(3'000'000);
    lob::PathConfig cfg;
    cfg.symbol = "AAPL";
    cfg.quoter = {.half_spread_ticks = 1, .quote_qty = 100, .skew_ticks_per_lot = 1, .requote_threshold = 1, .max_position = 300};
    cfg.risk = {.max_order_qty = 100, .collar_ticks = 20, .max_notional = 100'000'000, .max_open_orders = 4, .max_position = 300, .max_msgs_per_sec = 1'000'000};
    lob::FeedSide feed(cfg);
    std::unique_ptr<lob::Strategy> strat;
    lob::StageHistograms h;
    lob::StreamReader rd(cap);
    lob::MarketUpdate u;
    std::uint64_t seq = 0, n0 = 0, d0 = 0, orders_after_warmup = 0;
    for (lob::wire::Bytes m = rd.next(); !m.empty(); m = rd.next(), ++seq) {
        if (seq == 20'000) { n0 = g_news.load(); d0 = g_deletes.load(); }  // warm-up done
        if (seq >= 20'000 && std::getenv("LOB_TRACE_ALLOC")) g_trace.store(true);
        bool quoted = feed.on_message(m, 0, seq, u);
        if (!quoted) continue;
        if (!strat) { strat = std::make_unique<lob::Strategy>(cfg, u.ev.locate); strat->start(u.itch_ns); }
        auto before = strat->counts().sent_new + strat->counts().sent_replace + strat->counts().sent_cancel;
        if (seq >= 20'000 && std::getenv("LOB_TRACE_ALLOC")) g_trace.store(true);
        strat->on_update(u, u.t_feed_done, h, true);
        g_trace.store(false);
        if (seq >= 20'000) orders_after_warmup += strat->counts().sent_new + strat->counts().sent_replace + strat->counts().sent_cancel - before;
    }
    // Read the counters BEFORE any assertion: a failing EXPECT allocates its message.
    const std::uint64_t heap_calls = (g_news.load() - n0) + (g_deletes.load() - d0);
    ASSERT_GT(seq, 20'000u);
    EXPECT_GT(orders_after_warmup, 50u) << "the path must actually have been sending orders";
    EXPECT_EQ(heap_calls, 0u) << "heap calls on the message path after warm-up";
}
