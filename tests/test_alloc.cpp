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

#include <atomic>
#include <cstdlib>
#include <new>
#include <random>

#include "lob/book_v1.hpp"
#include "lob/book_v2.hpp"

namespace {

std::atomic<std::uint64_t> g_news{0}, g_deletes{0};

}  // namespace

void* operator new(std::size_t n) {
    g_news.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept {
    g_deletes.fetch_add(1, std::memory_order_relaxed);
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
