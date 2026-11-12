// replay.cpp — replay a day through each book and report what it cost.
//
//   usage: replay [--day day.bin] [--book v1|v2|both] [--runs 5] [--pin CPU] [--json out.json]
//                 [--expected N]   (override the pool / hash-map sizing stored in the day file)
//                 [--skip-latency] (throughput pass only: for running under a profiler)
//
// Three passes per book, each on a fresh book:
//   1. verify     : (with --book both) v1 and v2 must produce the same event
//                   stream for the whole day — compared by a running hash.
//   2. throughput : the whole day, flat out, timed once around the loop.
//                   No per-message clock reads, so no timer overhead.
//                   Repeated --runs times; the median run is reported.
//   3. latency    : a timestamp before and after every message. Reported as a
//                   distribution per kind of message, never as an average alone.
//
// What this measures, and what it does not (docs/BENCHMARKS.md):
//   * Service time of the book on one thread with warm caches: the messages are
//     replayed back to back, as fast as the book takes them. It is not
//     response time under a paced arrival rate, so coordinated omission does
//     not apply here — there is no queue in front of the book to hide in.
//   * Timer overhead is measured and printed; on Apple M-series the counter
//     ticks every 41.67 ns, so per-message numbers below that are quantised.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

#include "clock.hpp"
#include "day_file.hpp"
#include "lob/book_v1.hpp"
#include "lob/book_v2.hpp"

using namespace lob;
using namespace lob::bench;

namespace {

// Folds every event into one 64-bit hash: equal hashes over five million
// messages means the two books said the same thing, in the same order.
struct HashingListener {
    std::uint64_t h = 1469598103934665603ull;
    void mix(std::uint64_t v) noexcept { h = (h ^ v) * 1099511628211ull; }
    void on_accept(OrderId id) noexcept { mix(1); mix(id); }
    void on_reject(OrderId id, RejectReason r) noexcept { mix(2); mix(id); mix(std::uint64_t(r)); }
    void on_trade(const Trade& t) noexcept {
        mix(3); mix(t.taker); mix(t.maker); mix(std::uint64_t(t.price)); mix(t.qty); mix(t.maker_remaining);
    }
    void on_cancel(OrderId id, Qty q, CancelReason r) noexcept { mix(4); mix(id); mix(q); mix(std::uint64_t(r)); }
    void on_modify(OrderId id, Price p, Qty q) noexcept { mix(5); mix(id); mix(std::uint64_t(p)); mix(q); }
};

enum Kind : std::uint8_t { AddRests, AddTrades, Cancel, Modify, kKinds };
const char* kKindName[] = {"add (no trade)", "add (trades)", "cancel", "modify"};

struct Dist {
    std::size_t n = 0;
    double mean = 0, p50 = 0, p90 = 0, p99 = 0, p999 = 0, p9999 = 0, max = 0;
};

Dist summarise(std::vector<std::uint32_t>& v, double ns_per_tick) {
    Dist d;
    d.n = v.size();
    if (v.empty()) return d;
    std::sort(v.begin(), v.end());
    auto at = [&](double q) { return double(v[std::min(v.size() - 1, std::size_t(q * double(v.size())))]) * ns_per_tick; };
    double sum = 0;
    for (auto x : v) sum += x;
    d.mean = sum / double(v.size()) * ns_per_tick;
    d.p50 = at(0.5); d.p90 = at(0.9); d.p99 = at(0.99); d.p999 = at(0.999); d.p9999 = at(0.9999);
    d.max = double(v.back()) * ns_per_tick;
    return d;
}

struct Result {
    std::string book;
    double      throughput_mps = 0;   // million messages per second (median run)
    double      ns_per_msg = 0;       // 1e3 / throughput
    Dist        all, kind[kKinds];
};

template <template <class> class Book>
std::uint64_t verify_hash(const std::vector<Command>& day, const BookConfig& cfg) {
    HashingListener l;
    Book<HashingListener> b(l, cfg);
    for (const auto& c : day) b.submit(c);
    return l.h;
}

template <template <class> class Book>
Result measure(const char* name, const std::vector<Command>& day, const BookConfig& cfg, int runs,
               double ns_per_tick, bool skip_latency) {
    Result r;
    r.book = name;

    // Throughput: the median of `runs` flat-out replays.
    std::vector<double> secs;
    std::uint64_t sink = 0;
    for (int i = 0; i < runs; ++i) {
        CountingListener l;
        Book<CountingListener> b(l, cfg);
        auto t0 = std::chrono::steady_clock::now();
        for (const auto& c : day) b.submit(c);
        auto t1 = std::chrono::steady_clock::now();
        secs.push_back(std::chrono::duration<double>(t1 - t0).count());
        sink += l.trades;
    }
    std::sort(secs.begin(), secs.end());
    double med = secs[secs.size() / 2];
    r.throughput_mps = double(day.size()) / med / 1e6;
    r.ns_per_msg = med * 1e9 / double(day.size());
    if (skip_latency) return r;

    // Latency: one timestamp pair per message, stored in memory touched in advance.
    std::vector<std::uint32_t> all(day.size());
    std::vector<std::uint8_t> kind(day.size());
    {
        CountingListener l;
        Book<CountingListener> b(l, cfg);
        for (std::size_t i = 0; i < day.size(); ++i) {
            const Command& c = day[i];
            const std::uint64_t trades_before = l.trades;
            const std::uint64_t t0 = ticks();
            b.submit(c);
            const std::uint64_t t1 = ticks();
            all[i] = std::uint32_t(std::min<std::uint64_t>(t1 - t0, UINT32_MAX));
            kind[i] = c.type == MsgType::Cancel ? Cancel
                    : c.type == MsgType::Modify ? Modify
                    : l.trades != trades_before ? AddTrades : AddRests;
        }
        sink += l.trades;
    }
    std::vector<std::uint32_t> per[kKinds];
    for (std::size_t i = 0; i < all.size(); ++i) per[kind[i]].push_back(all[i]);
    for (int k = 0; k < kKinds; ++k) r.kind[k] = summarise(per[k], ns_per_tick);
    r.all = summarise(all, ns_per_tick);
    if (sink == 42) std::puts("");  // keep `sink` observable
    return r;
}

void print(const Result& r) {
    std::printf("\n### %s — %.1f M msg/s flat out (%.1f ns/msg mean)\n\n", r.book.c_str(), r.throughput_mps,
                r.ns_per_msg);
    std::printf("| messages | count | mean | p50 | p90 | p99 | p99.9 | p99.99 | max |\n");
    std::printf("|---|---:|---:|---:|---:|---:|---:|---:|---:|\n");
    auto row = [](const char* name, const Dist& d) {
        if (!d.n) return;
        std::printf("| %s | %zu | %.0f | %.0f | %.0f | %.0f | %.0f | %.0f | %.0f |\n", name, d.n, d.mean, d.p50,
                    d.p90, d.p99, d.p999, d.p9999, d.max);
    };
    row("**all**", r.all);
    for (int k = 0; k < kKinds; ++k) row(kKindName[k], r.kind[k]);
}

void json_dist(std::FILE* f, const Dist& d) {
    std::fprintf(f, "{\"n\":%zu,\"mean\":%.2f,\"p50\":%.2f,\"p90\":%.2f,\"p99\":%.2f,\"p999\":%.2f,\"p9999\":%.2f,\"max\":%.2f}",
                 d.n, d.mean, d.p50, d.p90, d.p99, d.p999, d.p9999, d.max);
}

}  // namespace

int main(int argc, char** argv) {
    std::string path = "day.bin", which = "both", json;
    int runs = 5, pin = -1;
    long long expected = -1;
    bool skip_latency = false;
    for (int i = 1; i < argc; i += 2) {
        std::string a = argv[i];
        if (a == "--skip-latency") { skip_latency = true; --i; continue; }
        if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", a.c_str()); return 2; }
        if (a == "--day") path = argv[i + 1];
        else if (a == "--book") which = argv[i + 1];
        else if (a == "--runs") runs = std::atoi(argv[i + 1]);
        else if (a == "--pin") pin = std::atoi(argv[i + 1]);
        else if (a == "--json") json = argv[i + 1];
        else if (a == "--expected") expected = std::atoll(argv[i + 1]);
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }

    if (pin >= 0) {
#if defined(__linux__)
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(pin, &set);
        if (pthread_setaffinity_np(pthread_self(), sizeof set, &set) != 0) std::perror("pthread_setaffinity_np");
        else std::printf("pinned to CPU %d\n", pin);
#else
        std::printf("note: --pin ignored; this OS has no thread-to-core pinning API\n");
#endif
    }

    DayHeader h{};
    std::vector<Command> day;
    try {
        day = read_day(path, h);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
    if (expected > 0) h.cfg.expected_orders = std::uint32_t(expected);

    const double ns_per_tick = calibrate_ns_per_tick();
    // The cost of the measurement itself: back-to-back timestamp pairs.
    std::vector<std::uint32_t> empty(skip_latency ? 1 : 1'000'000);
    for (auto& e : empty) {
        std::uint64_t a = ticks();
        std::uint64_t b = ticks();
        e = std::uint32_t(b - a);
    }
    Dist overhead = summarise(empty, ns_per_tick);

    std::printf("## Replay of %s: %zu messages (seed %llu)\n\n", path.c_str(), day.size(),
                (unsigned long long)h.seed);
    std::printf("clock %s: %.3f ns per tick; timestamp-pair overhead p50 %.1f ns, p99 %.1f ns\n", clock_name(),
                ns_per_tick, overhead.p50, overhead.p99);
    std::printf("books sized for %u orders\n", h.cfg.expected_orders);
    std::printf("latency columns are nanoseconds and include that overhead\n");

    if (which == "both") {
        std::uint64_t a = verify_hash<BookV1>(day, h.cfg), b = verify_hash<BookV2>(day, h.cfg);
        std::printf("verify: v1 and v2 event streams %s (hash %016llx vs %016llx)\n", a == b ? "IDENTICAL" : "DIFFER",
                    (unsigned long long)a, (unsigned long long)b);
        if (a != b) return 1;
    }

    std::vector<Result> results;
    if (which == "v1" || which == "both") results.push_back(measure<BookV1>("v1 (std containers)", day, h.cfg, runs, ns_per_tick, skip_latency));
    if (which == "v2" || which == "both") results.push_back(measure<BookV2>("v2 (arrays, pool, flat map)", day, h.cfg, runs, ns_per_tick, skip_latency));
    for (const auto& r : results) print(r);
    if (results.size() == 2 && !skip_latency)
        std::printf("\nv2 vs v1: %.2fx throughput, p50 %.0f -> %.0f ns, p99 %.0f -> %.0f ns, p99.9 %.0f -> %.0f ns\n",
                    results[1].throughput_mps / results[0].throughput_mps, results[0].all.p50, results[1].all.p50,
                    results[0].all.p99, results[1].all.p99, results[0].all.p999, results[1].all.p999);

    if (!json.empty()) {
        std::FILE* f = std::fopen(json.c_str(), "w");
        if (!f) { std::perror(json.c_str()); return 1; }
        std::fprintf(f, "{\"messages\":%zu,\"seed\":%llu,\"clock\":\"%s\",\"ns_per_tick\":%.4f,\"overhead\":", day.size(),
                     (unsigned long long)h.seed, clock_name(), ns_per_tick);
        json_dist(f, overhead);
        std::fprintf(f, ",\"books\":[");
        for (std::size_t i = 0; i < results.size(); ++i) {
            const auto& r = results[i];
            std::fprintf(f, "%s{\"book\":\"%s\",\"throughput_mps\":%.3f,\"ns_per_msg\":%.2f,\"all\":", i ? "," : "",
                         r.book.c_str(), r.throughput_mps, r.ns_per_msg);
            json_dist(f, r.all);
            for (int k = 0; k < kKinds; ++k) {
                std::fprintf(f, ",\"%s\":", kKindName[k]);
                json_dist(f, r.kind[k]);
            }
            std::fprintf(f, "}");
        }
        std::fprintf(f, "]}\n");
        std::fclose(f);
    }
    return 0;
}
