// gen_day.cpp — generate a synthetic but realistic trading day for one symbol.
//
// The shape follows what a real equity feed (e.g. Nasdaq ITCH) looks like for
// a liquid stock: most messages are passive orders placed near the touch and
// cancelled soon after; only a few percent trade. The generator runs version 1
// of the book as it goes, so cancels and modifies target orders that are
// really live — the same mix of hits and misses the engine would see.
//
//   usage: gen_day [--messages N] [--seed S] [--out day.bin]
//
// The same seed always produces the same file, so every benchmark run, on any
// machine, replays exactly the same day.
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <unordered_map>

#include "day_file.hpp"
#include "lob/book_v1.hpp"

using namespace lob;

namespace {

// Live order ids, with O(1) insert, erase and uniform random pick.
class LiveSet {
public:
    void add(OrderId id) {
        pos_[id] = ids_.size();
        ids_.push_back(id);
    }
    void remove(OrderId id) {
        auto it = pos_.find(id);
        if (it == pos_.end()) return;
        std::size_t i = it->second;
        ids_[i] = ids_.back();
        pos_[ids_[i]] = i;
        ids_.pop_back();
        pos_.erase(it);
    }
    bool empty() const { return ids_.empty(); }
    std::size_t size() const { return ids_.size(); }
    template <class Rng>
    OrderId pick(Rng& rng) const { return ids_[rng() % ids_.size()]; }

private:
    std::vector<OrderId>                         ids_;
    std::unordered_map<OrderId, std::size_t>     pos_;
};

// Removes makers that were filled completely.
struct TrackingListener {
    LiveSet* live;
    void on_accept(OrderId) {}
    void on_reject(OrderId, RejectReason) {}
    void on_trade(const Trade& t) { if (t.maker_remaining == 0) live->remove(t.maker); }
    void on_cancel(OrderId, Qty, CancelReason) {}
    void on_modify(OrderId, Price, Qty) {}
};

}  // namespace

int main(int argc, char** argv) {
    std::uint64_t n = 5'000'000, seed = 42;
    std::string out = "day.bin";
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string a = argv[i];
        if (a == "--messages") n = std::strtoull(argv[i + 1], nullptr, 10);
        else if (a == "--seed") seed = std::strtoull(argv[i + 1], nullptr, 10);
        else if (a == "--out") out = argv[i + 1];
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }

    // Size for the live working set (a few thousand orders rest at once), not
    // for every order of the day: docs/BENCHMARKS.md, experiment 2, shows a
    // table sized for 1M orders costing v2 more than half its throughput.
    BookConfig cfg{.min_price = 1, .max_price = 1 << 16, .expected_orders = 1 << 16};
    LiveSet live;
    TrackingListener tl{&live};
    BookV1 book(tl, cfg);
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> u(0, 1);
    std::geometric_distribution<int> depth(0.25);  // ticks behind the touch: mostly 0..5
    std::geometric_distribution<int> lots(0.45);   // size in round lots of 100

    Price mid = 20'000;  // $200.00 at a one-cent tick
    OrderId next_id = 1;
    std::vector<Command> cmds;
    cmds.reserve(n);
    std::uint64_t stats[6] = {};  // passive, aggressive, cancel, modify, stale-cancel, stale-modify

    auto emit = [&](const Command& c) {
        cmds.push_back(c);
        book.submit(c);
        if (c.type == MsgType::New && book.contains(c.id)) live.add(c.id);
        if (c.type == MsgType::Cancel) live.remove(c.id);
        if (c.type == MsgType::Modify && !book.contains(c.id)) live.remove(c.id);  // filled on re-entry
    };

    while (cmds.size() < n) {
        // The mid wanders: a slow random walk, one tick at a time.
        if (u(rng) < 0.002) mid += u(rng) < 0.5 ? -1 : 1;
        if (auto bb = book.best_bid(), ba = book.best_ask(); bb && ba && *ba - *bb >= 2) mid = (*bb + *ba) / 2;

        double r = u(rng);
        bool warmup = live.size() < 2'000;
        Command c{};
        c.side = u(rng) < 0.5 ? Side::Buy : Side::Sell;

        if (warmup || r < 0.46) {  // passive add near the touch
            c.type = MsgType::New;
            c.otype = OrderType::Limit;
            c.id = next_id++;
            int d = depth(rng);
            c.price = c.side == Side::Buy ? mid - 1 - d : mid + 1 + d;
            c.qty = Qty(100 * (1 + lots(rng)));
            ++stats[0];
        } else if (r < 0.52) {  // aggressive: crosses the spread
            c.type = MsgType::New;
            c.id = next_id++;
            double t = u(rng);
            c.otype = t < 0.6 ? OrderType::Limit : t < 0.85 ? OrderType::IOC : t < 0.95 ? OrderType::Market
                                                                                       : OrderType::FOK;
            int through = depth(rng);  // how many ticks through the touch
            c.price = c.side == Side::Buy ? mid + 1 + through : mid - 1 - through;
            c.qty = Qty(100 * (1 + lots(rng)));
            ++stats[1];
        } else if (r < 0.93) {  // cancel: almost always a live order
            c.type = MsgType::Cancel;
            bool stale = u(rng) < 0.01 || live.empty();
            c.id = stale ? 1 + rng() % next_id : live.pick(rng);
            ++stats[stale ? 4 : 2];
        } else {  // modify: mostly size down (keeps priority), some re-price
            c.type = MsgType::Modify;
            bool stale = u(rng) < 0.01 || live.empty();
            c.id = stale ? 1 + rng() % next_id : live.pick(rng);
            c.qty = Qty(100 * (1 + lots(rng)));
            int d = depth(rng);
            // We do not know the order's side here without asking the book, so
            // re-price around the mid on a random side; some of these cross.
            c.price = u(rng) < 0.5 ? mid - 1 - d : mid + 1 + d;
            ++stats[stale ? 5 : 3];
        }
        emit(c);
    }

    bench::DayHeader h{};
    std::memcpy(h.magic, "LOBDAY01", 8);
    h.count = cmds.size();
    h.seed = seed;
    h.cfg = cfg;
    bench::write_day(out, h, cmds);

    auto pct = [&](std::uint64_t x) { return 100.0 * double(x) / double(cmds.size()); };
    std::printf("wrote %s: %zu messages, seed %llu\n", out.c_str(), cmds.size(), (unsigned long long)seed);
    std::printf("  passive adds %5.1f%%   aggressive %4.1f%%   cancels %5.1f%%   modifies %4.1f%%\n",
                pct(stats[0]), pct(stats[1]), pct(stats[2] + stats[4]), pct(stats[3] + stats[5]));
    std::printf("  stale cancels/modifies (unknown id) %.2f%%   orders resting at end: %zu\n",
                pct(stats[4] + stats[5]), book.order_count());
    return 0;
}
