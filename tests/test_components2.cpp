// test_components2.cpp — the histogram against sorted samples; the ring under two threads.
#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <thread>
#include <vector>

#include "lob/histogram.hpp"
#include "lob/spsc.hpp"

using namespace lob;

TEST(Histogram, PercentilesAreWithinSevenPercentOfExact) {
    for (std::uint64_t seed = 1; seed <= 5; ++seed) {
        std::mt19937_64 rng(seed);
        std::lognormal_distribution<double> d(std::log(200.0), 1.2);  // a latency-shaped tail
        Histogram h;
        std::vector<std::uint64_t> v;
        for (int i = 0; i < 200'000; ++i) { auto x = std::uint64_t(d(rng)) + 1; v.push_back(x); h.record(x); }
        std::sort(v.begin(), v.end());
        for (double q : {0.5, 0.9, 0.99, 0.999, 0.9999}) {
            const std::uint64_t exact = v[std::min(v.size() - 1, std::size_t(q * double(v.size())))];
            const std::uint64_t got = h.percentile(q);
            // The bucket's lower bound is at most one bucket width (1/16 of the
            // magnitude) below the exact value and never above it.
            EXPECT_LE(got, exact) << "q " << q;
            EXPECT_GE(double(got), double(exact) * (1.0 - 1.0 / 16.0) - 1) << "q " << q;
        }
        EXPECT_EQ(h.count(), v.size());
        EXPECT_EQ(h.max(), v.back());
        EXPECT_EQ(h.min(), v.front());
    }
}

TEST(Histogram, SmallValuesAndMerge) {
    Histogram a, b;
    for (std::uint64_t x : {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 15u, 16u, 17u, 31u, 32u, 1000u}) a.record(x);
    EXPECT_EQ(a.percentile(0.0), 1u);
    EXPECT_EQ(a.min(), 1u);
    EXPECT_EQ(a.max(), 1000u);
    b.record(5000);
    a.merge(b);
    EXPECT_EQ(a.count(), 15u);
    EXPECT_EQ(a.max(), 5000u);
    EXPECT_EQ(a.percentile(1.0), a.percentile(0.999));
}

TEST(Spsc, FullAndEmpty) {
    SpscRing<int, 4> r;  // 3 usable slots
    int x;
    EXPECT_FALSE(r.pop(x));
    EXPECT_TRUE(r.push(1)); EXPECT_TRUE(r.push(2)); EXPECT_TRUE(r.push(3));
    EXPECT_FALSE(r.push(4));
    EXPECT_TRUE(r.pop(x)); EXPECT_EQ(x, 1);
    EXPECT_TRUE(r.push(4));
    EXPECT_TRUE(r.pop(x)); EXPECT_EQ(x, 2);
    EXPECT_TRUE(r.pop(x)); EXPECT_EQ(x, 3);
    EXPECT_TRUE(r.pop(x)); EXPECT_EQ(x, 4);
    EXPECT_FALSE(r.pop(x));
}

TEST(Spsc, DeliversEveryItemInOrderAcrossThreads) {
    struct Item { std::uint64_t seq; std::uint64_t payload; };
    SpscRing<Item, 1024> r;
    const std::uint64_t n = 2'000'000;
    std::thread producer([&] {
        for (std::uint64_t i = 0; i < n; ++i) { Item it{i, i * 2654435761ull}; while (!r.push(it)) {} }
    });
    std::uint64_t expect = 0, bad = 0;
    Item it;
    while (expect < n) {
        if (!r.pop(it)) continue;
        if (it.seq != expect || it.payload != it.seq * 2654435761ull) ++bad;
        ++expect;
    }
    producer.join();
    EXPECT_EQ(bad, 0u);
    EXPECT_FALSE(r.pop(it));
}
