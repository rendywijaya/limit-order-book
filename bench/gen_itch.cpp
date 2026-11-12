// gen_itch.cpp — a synthetic ITCH 5.0 capture, in the message-stream format.
//
//   usage: gen_itch [--messages N] [--symbols K] [--seed S] [--rate MSG_PER_SEC] [--out capture.itch]
//
// Several symbols, each with its own slowly wandering mid, a population of
// resting orders near the touch, and the message mix of a liquid equity:
// mostly adds and deletes near the touch, some partial cancels and replaces,
// a few executions at the touch. Timestamps are nanoseconds since midnight
// starting at 09:30, with exponential inter-arrival at the given mean rate,
// so a paced replay has a real clock to follow. The generator applies every
// message to a FeedHandler as it goes, so deletes and executions always name
// live orders and executions happen at the real best price.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "lob/feed.hpp"

using namespace lob;

namespace {
struct Live {  // live order refs for one symbol, with O(1) random pick and removal
    std::vector<std::uint64_t> ids;
    std::unordered_map<std::uint64_t, std::size_t> pos;
    void add(std::uint64_t r) { pos[r] = ids.size(); ids.push_back(r); }
    void remove(std::uint64_t r) {
        auto it = pos.find(r);
        if (it == pos.end()) return;
        std::size_t i = it->second;
        ids[i] = ids.back(); pos[ids[i]] = i; ids.pop_back(); pos.erase(it);
    }
    template <class R> std::uint64_t pick(R& rng) const { return ids[rng() % ids.size()]; }
};
}  // namespace

int main(int argc, char** argv) {
    std::size_t n = 2'000'000;
    int nsym = 8;
    std::uint64_t seed = 42;
    double rate = 40'000;  // messages per second across all symbols
    std::string out = "capture.itch";
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string a = argv[i];
        if (a == "--messages") n = std::strtoull(argv[i + 1], nullptr, 10);
        else if (a == "--symbols") nsym = std::atoi(argv[i + 1]);
        else if (a == "--seed") seed = std::strtoull(argv[i + 1], nullptr, 10);
        else if (a == "--rate") rate = std::atof(argv[i + 1]);
        else if (a == "--out") out = argv[i + 1];
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }

    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> u(0, 1);
    std::exponential_distribution<double> gap(rate / 1e9);  // ns between messages
    std::geometric_distribution<int> depth(0.25);
    std::geometric_distribution<int> lots(0.45);

    std::vector<std::uint8_t> stream;
    stream.reserve(n * 30);
    std::uint8_t buf[64];
    auto emit = [&](std::size_t k) { stream_append(stream, {buf, k}); };

    FeedHandler fh({.expected_orders = 1 << 18, .book_levels = 1 << 16});
    std::vector<Live> live(static_cast<std::size_t>(nsym));
    std::vector<std::string> names;
    std::vector<std::uint32_t> mid;  // raw price, 4 decimals
    std::uint64_t ns = 34'200'000'000'000ull;  // 09:30:00
    std::uint64_t ref = 1, match = 1;
    const char* tickers[] = {"AAPL", "MSFT", "NVDA", "AMZN", "META", "GOOG", "TSLA", "AVGO", "AMD", "NFLX", "INTC", "CSCO"};

    emit(itch::encode_system_event(buf, ns, 'O'));  // start of messages
    for (int s = 0; s < nsym; ++s) {
        names.push_back(s < 12 ? tickers[s] : "SYM" + std::to_string(s));
        mid.push_back(std::uint32_t(500'000 + 100 * (rng() % 40'000)));  // $50 .. $450, on a cent
        emit(itch::encode_stock_directory(buf, std::uint16_t(s + 1), ns, names.back()));
    }
    emit(itch::encode_system_event(buf, ns, 'Q'));  // market open
    std::size_t header_msgs = std::size_t(nsym) + 2;

    std::uint64_t counts[6] = {};  // add, delete, cancel, replace, execute, trade
    FeedHandler::LiveOrder lo_probe;
    std::vector<int> drift(static_cast<std::size_t>(nsym), 0);  // +1: the mid stepped up, pull the asks at the touch; -1: bids
    std::vector<Price> pull_price(static_cast<std::size_t>(nsym), 0);
    std::uint64_t touch_moves = 0;
    std::uint64_t crossed = 0;
    std::size_t msgs = 0;
    while (msgs < n) {
        int s = int(rng() % std::size_t(nsym));
        auto loc = std::uint16_t(s + 1);
        Live& lv = live[std::size_t(s)];
        ns += std::uint64_t(gap(rng));
        const MarketBook* b = fh.book(loc);
        // The price moves one tick: the mid for new adds steps at once, and the
        // makers at the level the market is leaving pull their orders, one
        // message at a time, until that level is empty.
        if (drift[std::size_t(s)] == 0 && u(rng) < 0.004 && b && b->top().two_sided()) {
            drift[std::size_t(s)] = u(rng) < 0.5 ? -1 : 1;
            pull_price[std::size_t(s)] = drift[std::size_t(s)] > 0 ? b->top().ask : b->top().bid;
            mid[std::size_t(s)] += drift[std::size_t(s)] > 0 ? 100u : std::uint32_t(-100);
            ++touch_moves;
        }
        if (drift[std::size_t(s)] != 0) {
            const Side pull_side = drift[std::size_t(s)] > 0 ? Side::Sell : Side::Buy;
            std::uint64_t victim = 0;
            for (std::uint64_t id : lv.ids)
                if (fh.find(id, lo_probe) && lo_probe.side == pull_side && lo_probe.price == pull_price[std::size_t(s)]) { victim = id; break; }
            if (victim) {
                std::size_t k = itch::encode_delete(buf, loc, ns, victim);
                lv.remove(victim);
                ++counts[1];
                wire::Bytes msg{buf, k};
                fh.on_message(msg);
                emit(k);
                ++msgs;
                continue;
            }
            drift[std::size_t(s)] = 0;  // the level is gone; the book's mid takes over again
        }
        auto bid_raw = [&]() -> std::uint32_t { return b && b->top().has_bid ? std::uint32_t(b->top().bid) * 100 : mid[std::size_t(s)] - 100; };
        auto ask_raw = [&]() -> std::uint32_t { return b && b->top().has_ask ? std::uint32_t(b->top().ask) * 100 : mid[std::size_t(s)] + 100; };
        // The mid follows the book once it exists, so adds stay near the real
        // touch; not while a move is in progress, or the move would never finish.
        if (drift[std::size_t(s)] == 0 && b && b->top().two_sided()) mid[std::size_t(s)] = (bid_raw() + ask_raw()) / 2 / 100 * 100;

        double r = u(rng);
        bool warm = lv.ids.size() < 400;
        std::size_t k;
        if (warm || r < 0.44) {  // add near the touch
            char side = u(rng) < 0.5 ? 'B' : 'S';
            int d = depth(rng);
            std::uint32_t px = side == 'B' ? mid[std::size_t(s)] - 100 - 100 * std::uint32_t(d) : mid[std::size_t(s)] + 100 + 100 * std::uint32_t(d);
            // A passive add never crosses or locks the touch: an order that
            // would has executed instead, and ITCH shows that as executions
            // against the resting side (the E branch below), not as an add.
            if (side == 'B' && b && b->top().has_ask && px >= ask_raw()) px = ask_raw() - 100;
            if (side == 'S' && b && b->top().has_bid && px <= bid_raw()) px = bid_raw() + 100;
            k = itch::encode_add(buf, loc, ns, ref, side, 100 * std::uint32_t(1 + lots(rng)), names[std::size_t(s)], px);
            lv.add(ref++);
            ++counts[0];
        } else if (r < 0.84) {  // delete a live order, preferring ones near the touch
            // Quotes at the touch are the most actively managed, so they are
            // cancelled most: sample a few live orders and delete the one
            // closest to the best price on its side.
            std::uint64_t o = lv.pick(rng);
            Price best_dist = 1 << 30;
            FeedHandler::LiveOrder cand;
            for (int t = 0; t < 4; ++t) {
                std::uint64_t c = lv.pick(rng);
                if (!fh.find(c, cand) || !b) continue;
                Price d = cand.side == Side::Buy ? (b->top().has_bid ? b->top().bid - cand.price : 0)
                                                 : (b->top().has_ask ? cand.price - b->top().ask : 0);
                if (d < best_dist) { best_dist = d; o = c; }
            }
            k = itch::encode_delete(buf, loc, ns, o);
            lv.remove(o);
            ++counts[1];
        } else if (r < 0.87) {  // partial cancel
            k = itch::encode_cancel(buf, loc, ns, lv.pick(rng), 100);
            ++counts[2];
        } else if (r < 0.92) {  // replace: new price near the touch, new ref
            std::uint64_t o = lv.pick(rng);
            int d = depth(rng);
            FeedHandler::LiveOrder cur;
            bool is_bid = fh.find(o, cur) ? cur.side == Side::Buy : u(rng) < 0.5;
            std::uint32_t px = is_bid ? mid[std::size_t(s)] - 100 - 100 * std::uint32_t(d) : mid[std::size_t(s)] + 100 + 100 * std::uint32_t(d);
            if (is_bid && b && b->top().has_ask && px >= ask_raw()) px = ask_raw() - 100;
            if (!is_bid && b && b->top().has_bid && px <= bid_raw()) px = bid_raw() + 100;
            k = itch::encode_replace(buf, loc, ns, o, ref, 100 * std::uint32_t(1 + lots(rng)), px);
            lv.remove(o);
            lv.add(ref++);
            ++counts[3];
        } else if (r < 0.985) {  // execution: prefer a resting order at the touch
            std::uint64_t victim = lv.pick(rng);
            FeedHandler::LiveOrder lo;
            for (int tries = 0; tries < 12; ++tries) {
                std::uint64_t o = lv.pick(rng);
                if (fh.find(o, lo) && b && ((lo.side == Side::Buy && b->top().has_bid && lo.price == b->top().bid) ||
                                            (lo.side == Side::Sell && b->top().has_ask && lo.price == b->top().ask))) {
                    victim = o;
                    break;
                }
            }
            k = itch::encode_executed(buf, loc, ns, victim, 100, match++);
            ++counts[4];
        } else {  // non-displayed trade print at the mid
            k = itch::encode_trade(buf, loc, ns, u(rng) < 0.5 ? 'B' : 'S', 100, names[std::size_t(s)],
                                   (bid_raw() + ask_raw()) / 2 / 100 * 100, match++);
            ++counts[5];
        }
        wire::Bytes msg{buf, k};
        itch::Msg m;
        itch::decode(msg, m);
        fh.on_message(msg);
        if (const MarketBook* bb = fh.book(loc); bb && bb->top().two_sided() && bb->top().bid >= bb->top().ask) ++crossed;
        emit(k);
        ++msgs;
        // An execution or cancel may have used up the order: keep the live set honest.
        if ((m.type == 'E' || m.type == 'X') && !fh.find(m.ref, lo_probe)) lv.remove(m.ref);
    }
    emit(itch::encode_system_event(buf, ns, 'M'));  // market close

    std::FILE* f = std::fopen(out.c_str(), "wb");
    if (!f) { std::perror(out.c_str()); return 1; }
    std::fwrite(stream.data(), 1, stream.size(), f);
    std::fclose(f);

    double secs = double(ns - 34'200'000'000'000ull) / 1e9;
    std::printf("wrote %s: %zu messages (+%zu header), %d symbols, %.1f s of exchange time (%.0f msg/s), %.1f MB\n",
                out.c_str(), msgs, header_msgs, nsym, secs, double(msgs) / secs, double(stream.size()) / 1e6);
    auto pct = [&](std::uint64_t c) { return 100.0 * double(c) / double(msgs); };
    std::printf("  adds %.1f%%  deletes %.1f%%  cancels %.1f%%  replaces %.1f%%  executions %.1f%%  trade prints %.1f%%\n",
                pct(counts[0]), pct(counts[1]), pct(counts[2]), pct(counts[3]), pct(counts[4]), pct(counts[5]));
    std::printf("  live orders at end: %zu; feed handler: unknown refs %llu, sub-penny %llu; messages after which a book was crossed: %llu; touch moves %llu (%.1f per symbol-second)\n",
                fh.live_orders(), (unsigned long long)fh.stats().unknown_ref, (unsigned long long)fh.stats().bad_price,
                (unsigned long long)crossed, (unsigned long long)touch_moves, double(touch_moves) / secs / nsym);
    for (int s = 0; s < nsym; ++s) {
        const MarketBook* b = fh.book(std::uint16_t(s + 1));
        if (b && b->top().two_sided())
            std::printf("  %-5s bid %u.%02u x %llu   ask %u.%02u x %llu\n", names[std::size_t(s)].c_str(), unsigned(b->top().bid / 100),
                        unsigned(b->top().bid % 100), (unsigned long long)b->top().bid_qty, unsigned(b->top().ask / 100),
                        unsigned(b->top().ask % 100), (unsigned long long)b->top().ask_qty);
    }
    return 0;
}
