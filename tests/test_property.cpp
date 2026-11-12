// test_property.cpp — millions of random messages, three books, one answer.
//
// After every message, version 1, version 2 and the reference book must have
// produced identical events and must hold identical books (every level, both
// sides). When they disagree, the failing sequence is *shrunk*: chunks of
// messages are removed for as long as the disagreement survives, and what is
// left — usually a handful of messages — is printed as a reproduction.
//
// Set LOB_PROPERTY_SEEDS (default 200) to run longer, e.g. overnight.
#include <gtest/gtest.h>

#include <cstdlib>
#include <random>
#include <sstream>

#include "lob/book_v1.hpp"
#include "lob/book_v2.hpp"
#include "lob/reference_book.hpp"

using namespace lob;

namespace {

struct Profile {
    const char* name;
    BookConfig  cfg;
    Price       mid;
    Price       spread;  // prices drawn from mid +- spread
};

// Generates commands that collide often: few ids, narrow prices, small sizes.
std::vector<Command> generate(std::uint64_t seed, std::size_t n, const Profile& p) {
    std::mt19937_64 rng(seed);
    auto pct = [&] { return std::uniform_int_distribution<int>(0, 99)(rng); };
    auto between = [&](auto lo, auto hi) { return std::uniform_int_distribution<decltype(lo)>(lo, hi)(rng); };
    std::vector<Command> out;
    OrderId next_id = 1;
    auto some_id = [&]() -> OrderId { return between(OrderId{1}, next_id); };
    auto price = [&]() -> Price {
        int r = pct();
        if (r == 0) return p.cfg.min_price - 1;  // out of range
        if (r == 1) return p.cfg.max_price + 1;
        if (r == 2) return p.cfg.min_price;      // edges of the window
        if (r == 3) return p.cfg.max_price;
        return std::clamp<Price>(p.mid + between(-p.spread, p.spread), p.cfg.min_price, p.cfg.max_price);
    };
    auto qty = [&]() -> Qty { return pct() < 2 ? 0 : between(Qty{1}, Qty{20}); };

    for (std::size_t i = 0; i < n; ++i) {
        int r = pct();
        Command c{};
        c.side = pct() < 50 ? Side::Buy : Side::Sell;
        if (r < 50) {
            c.type = MsgType::New;
            c.id = pct() < 3 ? some_id() : next_id++;  // occasionally a duplicate
            int t = pct();
            c.otype = t < 80 ? OrderType::Limit : t < 88 ? OrderType::IOC : t < 94 ? OrderType::Market : OrderType::FOK;
            c.price = price();
            c.qty = qty();
        } else if (r < 78) {
            c.type = MsgType::Cancel;
            c.id = some_id();
        } else {
            c.type = MsgType::Modify;
            c.id = some_id();
            c.price = price();
            c.qty = qty();
        }
        out.push_back(c);
    }
    return out;
}

std::string describe(const Command& c) {
    std::ostringstream os;
    switch (c.type) {
        case MsgType::New:
            os << "add(" << c.id << ", " << to_string(c.side) << ", " << to_string(c.otype) << ", " << c.price
               << ", " << c.qty << ")";
            break;
        case MsgType::Cancel: os << "cancel(" << c.id << ")"; break;
        case MsgType::Modify: os << "modify(" << c.id << ", " << c.price << ", " << c.qty << ")"; break;
    }
    return os.str();
}

// Runs the sequence through all three books. Returns the index of the first
// message after which they disagree (with a reason), or -1 if they never do.
struct Mismatch {
    long        at = -1;
    std::string why;
};

Mismatch run(const std::vector<Command>& cmds, const BookConfig& cfg) {
    RecordingListener e1, e2, er;
    BookV1 v1(e1, cfg);
    BookV2 v2(e2, cfg);
    ReferenceBook ref(er, cfg);
    for (std::size_t i = 0; i < cmds.size(); ++i) {
        v1.submit(cmds[i]);
        v2.submit(cmds[i]);
        ref.submit(cmds[i]);
        std::ostringstream why;
        auto dump = [&](const char* name, const std::vector<Event>& ev) {
            why << "  " << name << ":";
            for (auto& e : ev) why << " " << e;
            why << "\n";
        };
        if (e1.events != er.events || e2.events != er.events) {
            why << "events differ\n";
            dump("ref", er.events);
            dump("v1 ", e1.events);
            dump("v2 ", e2.events);
            return {long(i), why.str()};
        }
        for (Side s : {Side::Buy, Side::Sell}) {
            auto dr = ref.depth(s);
            if (v1.depth(s) != dr || v2.depth(s) != dr) {
                why << "depth differs on " << to_string(s) << " side";
                return {long(i), why.str()};
            }
        }
        if (v1.order_count() != ref.order_count() || v2.order_count() != ref.order_count())
            return {long(i), "order count differs"};
        e1.events.clear();
        e2.events.clear();
        er.events.clear();
    }
    return {};
}

// Delta debugging: remove ever smaller chunks while the failure persists.
// Repeats until a full pass removes nothing: removing one message can make
// another one removable.
std::vector<Command> shrink(std::vector<Command> cmds, const BookConfig& cfg) {
    for (std::size_t before = 0; before != cmds.size();) {
        before = cmds.size();
        for (std::size_t chunk = std::max<std::size_t>(1, cmds.size() / 2); chunk >= 1; chunk /= 2) {
            for (std::size_t start = 0; start < cmds.size();) {
                std::vector<Command> trial;
                trial.reserve(cmds.size());
                trial.insert(trial.end(), cmds.begin(), cmds.begin() + long(start));
                trial.insert(trial.end(), cmds.begin() + long(std::min(cmds.size(), start + chunk)), cmds.end());
                if (run(trial, cfg).at >= 0) cmds = std::move(trial);  // still fails: keep it smaller
                else start += chunk;
            }
        }
    }
    return cmds;
}

std::size_t seeds() {
    const char* s = std::getenv("LOB_PROPERTY_SEEDS");
    return s ? std::strtoull(s, nullptr, 10) : 200;
}

void check_profile(const Profile& p, std::size_t steps) {
    for (std::uint64_t seed = 1; seed <= seeds(); ++seed) {
        auto cmds = generate(seed, steps, p);
        Mismatch m = run(cmds, p.cfg);
        if (m.at < 0) continue;
        cmds.resize(std::size_t(m.at) + 1);
        auto small = shrink(cmds, p.cfg);
        std::ostringstream os;
        os << "profile " << p.name << ", seed " << seed << ": books disagree after message " << m.at
           << ". Shrunk to " << small.size() << " messages:\n";
        for (auto& c : small) os << "  " << describe(c) << "\n";
        os << run(small, p.cfg).why;
        FAIL() << os.str();
    }
}

}  // namespace

// Prices crowded around one mid: lots of crossing, partial fills, deep queues.
TEST(Property, DenseBookAgreesWithReference) {
    check_profile({"dense", {.min_price = 1, .max_price = 2000, .expected_orders = 256}, 1000, 6}, 3000);
}

// A small window that spans several 64-bit bitmap words, with prices spread
// across all of it: exercises the bitmap search across word boundaries.
TEST(Property, SparseBookAcrossBitmapWordsAgrees) {
    check_profile({"sparse", {.min_price = 1, .max_price = 300, .expected_orders = 16}, 150, 149}, 3000);
}

// Deliberately tiny pool and hash-map reservations, so growth paths run too.
TEST(Property, GrowthPathsAgree) {
    check_profile({"growth", {.min_price = 1, .max_price = 5000, .expected_orders = 1}, 2500, 40}, 2000);
}

// The shrinker itself: a property that fails must shrink to a short witness.
TEST(Property, ShrinkerFindsMinimalSequence) {
    // A fake "bug": pretend any sequence containing both id 3 and id 7 fails.
    std::vector<Command> cmds;
    for (OrderId i = 1; i <= 64; ++i) cmds.push_back({MsgType::New, Side::Buy, OrderType::Limit, i, 100, 1});
    auto fails = [](const std::vector<Command>& v) {
        bool a = false, b = false;
        for (auto& c : v) { a |= c.id == 3; b |= c.id == 7; }
        return a && b;
    };
    for (std::size_t chunk = cmds.size() / 2; chunk >= 1; chunk /= 2)
        for (std::size_t start = 0; start < cmds.size();) {
            std::vector<Command> trial(cmds.begin(), cmds.begin() + long(start));
            trial.insert(trial.end(), cmds.begin() + long(std::min(cmds.size(), start + chunk)), cmds.end());
            if (fails(trial)) cmds = trial; else start += chunk;
        }
    ASSERT_EQ(cmds.size(), 2u);
    EXPECT_EQ(cmds[0].id, 3u);
    EXPECT_EQ(cmds[1].id, 7u);
}
