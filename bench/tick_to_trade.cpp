// tick_to_trade.cpp — replay a capture through the whole path and time every stage.
//
//   usage: tick_to_trade [--capture data/capture.itch] [--symbol AAPL]
//                        [--mode flat|paced] [--speed 1.0] [--threads 1|2]
//                        [--warmup 100000] [--json out.json] [--check]
//
//   flat     messages back to back: service time of the path on one or two
//            threads, with nothing queued in front of it.
//   paced    messages at the capture's own pace (ITCH timestamps), divided by
//            --speed: response time. Each message's arrival is its DUE time,
//            so a message that waited because we were still busy with the
//            previous one is charged for the wait (no coordinated omission).
//   threads  1: feed and strategy in one call chain. 2: the feed thread pushes
//            book snapshots through an SPSC ring to the strategy thread.
//   --check  run flat with 1 and 2 threads and require identical order bytes.
//
// Latencies are nanoseconds from the hardware counter (0.385 ns rdtsc on the
// x86 CI runner, 41.67 ns cntvct on Apple Silicon), after `warmup` messages.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

#include "lob/path.hpp"
#include "lob/spsc.hpp"

using namespace lob;

namespace {

struct Result {
    StageHistograms    h;
    Strategy::Counts   c;
    std::uint64_t      hash = 0;
    TopOfBook          final_top;
    std::int64_t       pnl_ticks = 0, position = 0;
    std::uint64_t      messages = 0, quoted = 0;
    double             secs = 0;
    FeedHandler::Stats fs;
    RiskGate::Stats    rs;
    Quoter::Stats      qs;
    ExchangeSim::Stats ss;
    std::uint64_t      gw_sent = 0, gw_answers = 0;
};

std::vector<std::uint8_t> load(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { std::perror(path.c_str()); std::exit(1); }
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<std::uint8_t> v(static_cast<std::size_t>(n));
    if (std::fread(v.data(), 1, v.size(), f) != v.size()) { std::perror("read"); std::exit(1); }
    std::fclose(f);
    return v;
}

void pin(int cpu) {
#if defined(__linux__)
    if (cpu < 0) return;
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(cpu, &set);
    pthread_setaffinity_np(pthread_self(), sizeof set, &set);
#else
    (void)cpu;
#endif
}

// Reads the 6-byte ITCH timestamp without decoding the whole message.
inline std::uint64_t itch_ns_of(wire::Bytes m) { return m.size() >= 11 ? wire::be48(m.data() + 5) : 0; }

Result run(const std::vector<std::uint8_t>& capture, const PathConfig& cfg, bool paced, double speed, int threads,
           std::uint64_t warmup, double ticks_per_ns, int pin_feed, int pin_strat) {
    Result r;
    FeedSide feed(cfg);
    std::unique_ptr<Strategy> strat;
    StageHistograms h;
    auto ensure_strategy = [&](const MarketUpdate& u) {
        if (!strat) { strat = std::make_unique<Strategy>(cfg, u.ev.locate); strat->start(u.itch_ns); }
    };

    StreamReader rd(capture);
    const auto wall0 = std::chrono::steady_clock::now();
    const std::uint64_t t0 = bench::ticks();
    std::uint64_t itch0 = 0, seq = 0;

    // The feed loop, shared by both modes: produces MarketUpdates for the
    // quoted symbol and hands each to `deliver`.
    auto feed_loop = [&](auto&& deliver) {
        pin(pin_feed);
        MarketUpdate u;
        for (wire::Bytes msg = rd.next(); !msg.empty(); msg = rd.next(), ++seq) {
            std::uint64_t t_arrive;
            if (paced) {
                const std::uint64_t ns = itch_ns_of(msg);
                if (!itch0) itch0 = ns;
                const std::uint64_t due = t0 + std::uint64_t(double(ns - itch0) * ticks_per_ns / speed);
                std::uint64_t now = bench::ticks();
                while (now < due) now = bench::ticks();  // spin: arrivals keep the capture's clock
                if (seq >= warmup) h.lateness.record(now - due);
                t_arrive = due;
            } else {
                t_arrive = bench::ticks();
            }
            const bool quoted = feed.on_message(msg, t_arrive, seq, u);
            const std::uint64_t t_done = quoted ? u.t_feed_done : bench::ticks();
            if (seq >= warmup) h.feed.record(t_done - t_arrive);
            if (quoted) { ++r.quoted; deliver(u); }
        }
        r.messages = seq;
    };

    if (threads <= 1) {
        feed_loop([&](const MarketUpdate& u) {
            ensure_strategy(u);
            strat->on_update(u, u.t_feed_done, h, u.seq >= warmup);
        });
    } else {
        auto ring = std::make_unique<SpscRing<MarketUpdate, 1 << 16>>();
        std::atomic<bool> done{false};
        std::thread strategy_thread([&] {
            pin(pin_strat);
            MarketUpdate u;
            for (;;) {
                if (ring->pop(u)) {
                    const std::uint64_t t_pop = bench::ticks();
                    ensure_strategy(u);
                    strat->on_update(u, t_pop, h, u.seq >= warmup);
                } else if (done.load(std::memory_order_acquire)) {
                    if (!ring->pop(u)) break;
                    const std::uint64_t t_pop = bench::ticks();
                    ensure_strategy(u);
                    strat->on_update(u, t_pop, h, u.seq >= warmup);
                }
            }
        });
        feed_loop([&](const MarketUpdate& u) { while (!ring->push(u)) {} });
        done.store(true, std::memory_order_release);
        strategy_thread.join();
    }

    r.secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0).count();
    r.h = h;
    r.fs = feed.feed().stats();
    if (strat) {
        r.c = strat->counts();
        r.hash = strat->order_hash();
        r.rs = strat->risk().stats();
        r.qs = strat->quoter().stats();
        r.ss = strat->sim().stats();
        r.position = strat->gateway().exposure().position;
        const MarketBook* b = feed.locate() >= 0 ? feed.feed().book(std::uint16_t(feed.locate())) : nullptr;
        if (b) { r.final_top = b->top(); r.pnl_ticks = strat->pnl_ticks(b->top()); }
        r.gw_sent = strat->gateway().stats().sent_new + strat->gateway().stats().sent_replace + strat->gateway().stats().sent_cancel;
        r.gw_answers = strat->gateway().stats().accepted + strat->gateway().stats().replaced + strat->gateway().stats().canceled +
                       strat->gateway().stats().executed + strat->gateway().stats().rejected;
    }
    return r;
}

void print_stage(const char* name, const Histogram& hh, double ns_per_tick) {
    if (!hh.count()) return;
    auto ns = [&](double q) { return double(hh.percentile(q)) * ns_per_tick; };
    std::printf("| %s | %llu | %.0f | %.0f | %.0f | %.0f | %.0f | %.0f | %.0f |\n", name, (unsigned long long)hh.count(),
                hh.mean() * ns_per_tick, ns(0.5), ns(0.9), ns(0.99), ns(0.999), ns(0.9999), double(hh.max()) * ns_per_tick);
}

void print(const Result& r, const char* title, double ns_per_tick) {
    std::printf("\n### %s\n\n", title);
    std::printf("%llu messages in %.2f s (%.1f M msg/s); %llu for the quoted symbol; top changed %llu times\n",
                (unsigned long long)r.messages, r.secs, double(r.messages) / r.secs / 1e6, (unsigned long long)r.quoted,
                (unsigned long long)r.c.top_changes);
    std::printf("orders: %llu new, %llu replace, %llu cancel (%llu messages produced orders); risk refused %llu; "
                "fills %llu for %llu shares; final position %lld; mark-to-market P&L %lld ticks x shares; order hash %016llx\n",
                (unsigned long long)r.c.sent_new, (unsigned long long)r.c.sent_replace, (unsigned long long)r.c.sent_cancel,
                (unsigned long long)r.c.messages_with_orders, (unsigned long long)r.c.refused, (unsigned long long)r.ss.fills,
                (unsigned long long)r.ss.filled_qty, (long long)r.position, (long long)r.pnl_ticks, (unsigned long long)r.hash);
    std::printf("fills by kind: queue %llu, traded-through %llu, marketable %llu; risk refusals: ",
                (unsigned long long)r.ss.fills_queue, (unsigned long long)r.ss.fills_through, (unsigned long long)r.ss.fills_marketable);
    for (int i = 1; i < 9; ++i)
        if (r.rs.refused[i]) std::printf("%s %llu, ", to_string(RiskReason(i)), (unsigned long long)r.rs.refused[i]);
    std::printf("\n\n| stage | count | mean | p50 | p90 | p99 | p99.9 | p99.99 | max |\n|---|---:|---:|---:|---:|---:|---:|---:|---:|\n");
    print_stage("feed: decode + book + snapshot", r.h.feed, ns_per_tick);
    print_stage("queue: feed thread → strategy thread", r.h.queue, ns_per_tick);
    print_stage("decide: quoter", r.h.decide, ns_per_tick);
    print_stage("risk: pre-trade checks", r.h.risk, ns_per_tick);
    print_stage("send: gateway state + OUCH + SoupBin frame", r.h.send, ns_per_tick);
    print_stage("exchange: simulated answer, processed", r.h.exchange, ns_per_tick);
    print_stage("wire-to-decision", r.h.wire_to_decision, ns_per_tick);
    print_stage("**tick-to-trade**", r.h.tick_to_trade, ns_per_tick);
    print_stage("lateness vs schedule (paced)", r.h.lateness, ns_per_tick);
}

void json_hist(std::FILE* f, const char* name, const Histogram& hh, double ns_per_tick) {
    std::fprintf(f, "\"%s\":{\"n\":%llu,\"mean\":%.1f,\"p50\":%.1f,\"p90\":%.1f,\"p99\":%.1f,\"p999\":%.1f,\"p9999\":%.1f,\"max\":%.1f}",
                 name, (unsigned long long)hh.count(), hh.mean() * ns_per_tick, double(hh.percentile(0.5)) * ns_per_tick,
                 double(hh.percentile(0.9)) * ns_per_tick, double(hh.percentile(0.99)) * ns_per_tick,
                 double(hh.percentile(0.999)) * ns_per_tick, double(hh.percentile(0.9999)) * ns_per_tick, double(hh.max()) * ns_per_tick);
}

}  // namespace

int main(int argc, char** argv) {
    std::string capture = "data/capture.itch", json;
    PathConfig cfg;
    cfg.quoter = {.half_spread_ticks = 1, .quote_qty = 100, .skew_ticks_per_lot = 1, .requote_threshold = 1, .max_position = 500};
    cfg.risk = {.max_order_qty = 1000, .collar_ticks = 50, .max_notional = 100'000'000, .max_open_orders = 4, .max_position = 2000, .max_msgs_per_sec = 100'000};
    bool paced = false, check = false;
    double speed = 1.0;
    int threads = 1, pin_feed = -1, pin_strat = -1;
    std::uint64_t warmup = 100'000;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&] { if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", a.c_str()); std::exit(2); } return std::string(argv[++i]); };
        if (a == "--capture") capture = val();
        else if (a == "--symbol") cfg.symbol = val();
        else if (a == "--mode") paced = val() == "paced";
        else if (a == "--speed") speed = std::atof(val().c_str());
        else if (a == "--threads") threads = std::atoi(val().c_str());
        else if (a == "--warmup") warmup = std::strtoull(val().c_str(), nullptr, 10);
        else if (a == "--json") json = val();
        else if (a == "--pin") { std::string v = val(); pin_feed = std::atoi(v.c_str()); auto c = v.find(','); if (c != std::string::npos) pin_strat = std::atoi(v.c_str() + c + 1); }
        else if (a == "--check") check = true;
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    const auto bytes = load(capture);
    const double ns_per_tick = bench::calibrate_ns_per_tick();
    std::printf("## Tick-to-trade: %s, symbol %s, clock %s (%.3f ns/tick), warm-up %llu messages\n", capture.c_str(),
                cfg.symbol.c_str(), bench::clock_name(), ns_per_tick, (unsigned long long)warmup);

    if (check) {
        Result a = run(bytes, cfg, false, 1.0, 1, warmup, 1.0 / ns_per_tick, pin_feed, pin_strat);
        Result b = run(bytes, cfg, false, 1.0, 2, warmup, 1.0 / ns_per_tick, pin_feed, pin_strat);
        std::printf("\ndeterminism: one thread %016llx, two threads %016llx: %s (%llu vs %llu orders, position %lld vs %lld)\n",
                    (unsigned long long)a.hash, (unsigned long long)b.hash, a.hash == b.hash ? "IDENTICAL" : "DIFFER",
                    (unsigned long long)a.gw_sent, (unsigned long long)b.gw_sent, (long long)a.position, (long long)b.position);
        return a.hash == b.hash ? 0 : 1;
    }

    char title[128];
    std::snprintf(title, sizeof title, "%s, %d thread%s%s", paced ? "paced" : "flat out", threads, threads > 1 ? "s" : "",
                  paced ? (std::string(" at ") + std::to_string(speed) + "x capture speed").c_str() : "");
    Result r = run(bytes, cfg, paced, speed, threads, warmup, 1.0 / ns_per_tick, pin_feed, pin_strat);
    print(r, title, ns_per_tick);

    if (!json.empty()) {
        std::FILE* f = std::fopen(json.c_str(), "w");
        if (!f) { std::perror(json.c_str()); return 1; }
        std::fprintf(f, "{\"mode\":\"%s\",\"speed\":%.3f,\"threads\":%d,\"clock\":\"%s\",\"ns_per_tick\":%.4f,\"messages\":%llu,"
                        "\"quoted\":%llu,\"secs\":%.3f,\"sent_new\":%llu,\"sent_replace\":%llu,\"sent_cancel\":%llu,\"refused\":%llu,"
                        "\"fills\":%llu,\"position\":%lld,\"pnl_ticks\":%lld,\"hash\":\"%016llx\",",
                     paced ? "paced" : "flat", speed, threads, bench::clock_name(), ns_per_tick, (unsigned long long)r.messages,
                     (unsigned long long)r.quoted, r.secs, (unsigned long long)r.c.sent_new, (unsigned long long)r.c.sent_replace,
                     (unsigned long long)r.c.sent_cancel, (unsigned long long)r.c.refused, (unsigned long long)r.ss.fills,
                     (long long)r.position, (long long)r.pnl_ticks, (unsigned long long)r.hash);
        json_hist(f, "feed", r.h.feed, ns_per_tick); std::fputc(',', f);
        json_hist(f, "queue", r.h.queue, ns_per_tick); std::fputc(',', f);
        json_hist(f, "decide", r.h.decide, ns_per_tick); std::fputc(',', f);
        json_hist(f, "risk", r.h.risk, ns_per_tick); std::fputc(',', f);
        json_hist(f, "send", r.h.send, ns_per_tick); std::fputc(',', f);
        json_hist(f, "exchange", r.h.exchange, ns_per_tick); std::fputc(',', f);
        json_hist(f, "wire_to_decision", r.h.wire_to_decision, ns_per_tick); std::fputc(',', f);
        json_hist(f, "tick_to_trade", r.h.tick_to_trade, ns_per_tick); std::fputc(',', f);
        json_hist(f, "lateness", r.h.lateness, ns_per_tick);
        std::fprintf(f, "}\n");
        std::fclose(f);
    }
    return 0;
}
