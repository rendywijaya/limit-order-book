// test_components.cpp — the building blocks of version 2, each against a
// standard-library model of itself.
#include <gtest/gtest.h>

#include <random>
#include <set>
#include <unordered_map>

#include "lob/flat_hash_map.hpp"
#include "lob/level_bitmap.hpp"

using namespace lob;

TEST(FlatHashMap, BasicInsertFindErase) {
    FlatHashMap m(4);
    EXPECT_EQ(m.find(1), FlatHashMap::kEmpty);
    m.insert(1, 10);
    m.insert(2, 20);
    EXPECT_EQ(m.find(1), 10u);
    EXPECT_EQ(m.find(2), 20u);
    m.erase(1);
    EXPECT_FALSE(m.contains(1));
    EXPECT_EQ(m.find(2), 20u);
    m.erase(99);  // erasing an absent key is a no-op
    EXPECT_EQ(m.size(), 1u);
}

TEST(FlatHashMap, AgreesWithUnorderedMapUnderChurn) {
    // Small key space + tiny table = long probe chains and many wrap-arounds,
    // which is where backward-shift deletion goes wrong if it is going to.
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        std::mt19937_64 rng(seed);
        FlatHashMap m(2);
        std::unordered_map<std::uint64_t, std::uint32_t> model;
        for (int i = 0; i < 50'000; ++i) {
            std::uint64_t k = rng() % 512;
            if (rng() % 3 && !model.contains(k)) {
                auto v = std::uint32_t(rng() % 1'000'000);
                m.insert(k, v);
                model[k] = v;
            } else {
                m.erase(k);
                model.erase(k);
            }
            ASSERT_EQ(m.size(), model.size());
        }
        for (std::uint64_t k = 0; k < 512; ++k) {
            auto it = model.find(k);
            ASSERT_EQ(m.find(k), it == model.end() ? FlatHashMap::kEmpty : it->second) << "key " << k;
        }
    }
}

TEST(LevelBitmap, NextAndPrevAcrossWordsAndLayers) {
    LevelBitmap b(300'000);
    EXPECT_EQ(b.next_ge(0), LevelBitmap::kNone);
    EXPECT_EQ(b.prev_le(299'999), LevelBitmap::kNone);
    b.set(5);
    b.set(64);
    b.set(4096);
    b.set(299'999);
    EXPECT_EQ(b.next_ge(0), 5);
    EXPECT_EQ(b.next_ge(6), 64);
    EXPECT_EQ(b.next_ge(65), 4096);
    EXPECT_EQ(b.next_ge(4097), 299'999);
    EXPECT_EQ(b.prev_le(299'998), 4096);
    EXPECT_EQ(b.prev_le(4095), 64);
    EXPECT_EQ(b.prev_le(4), LevelBitmap::kNone);
    b.clear(64);
    EXPECT_EQ(b.next_ge(6), 4096);
    EXPECT_EQ(b.prev_le(4095), 5);
}

TEST(LevelBitmap, AgreesWithStdSet) {
    for (std::size_t n : {1u, 63u, 64u, 65u, 4096u, 5000u, 300'000u}) {
        std::mt19937_64 rng(n);
        LevelBitmap b(n);
        std::set<std::int64_t> model;
        for (int i = 0; i < 20'000; ++i) {
            auto k = std::int64_t(rng() % n);
            if (rng() % 2) { b.set(std::size_t(k)); model.insert(k); }
            else { b.clear(std::size_t(k)); model.erase(k); }
            auto q = std::int64_t(rng() % n);
            auto ge = model.lower_bound(q);
            ASSERT_EQ(b.next_ge(q), ge == model.end() ? LevelBitmap::kNone : *ge) << "n=" << n << " q=" << q;
            auto le = model.upper_bound(q);
            ASSERT_EQ(b.prev_le(q), le == model.begin() ? LevelBitmap::kNone : *std::prev(le));
        }
    }
}
