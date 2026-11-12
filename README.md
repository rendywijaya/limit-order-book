# lob: a limit order book, a matching engine, and the path from market data to order

Two parts, one repository, C++20, no allocation on any hot path.

**Part 1** is a price-time priority matching engine for one instrument, with
**Limit, Market, IOC and FOK** orders, cancels and modifies, built twice:

* **v1** uses the standard containers and is simply correct.
* **v2** is rewritten after measuring v1: price levels in arrays, a
  hierarchical bitmap to find the best level, orders in an index-linked pool,
  and an open-addressing hash map. **No allocation on the hot path.**

Both pass the same tests and produce **identical event streams** over a
5-million-message day.

**Part 2** is the trading path around it: Nasdaq **ITCH 5.0** decoded from raw
bytes into market-data books, a market-making **quoter**, a **pre-trade risk**
gate, an order **gateway** speaking **OUCH 4.2** inside **SoupBinTCP**, and a
**simulated exchange** that fills by queue position, so the whole loop runs
deterministically under replay with a timestamp at every stage. One thread or
two (feed → SPSC ring → strategy); flat out or at the capture's own pace.
[docs/PATH.md](docs/PATH.md) is the write-up.

## Part 2 results: tick-to-trade

2,000,000 synthetic ITCH messages, 8 symbols, 50 s of exchange time, quoting
one. Apple M2 Pro, one thread, flat out; nanoseconds, quantized to the 41.67 ns
counter tick ([method and all runs](docs/PATH.md#the-measurement)):

| stage | p50 | p90 | p99 |
|---|---:|---:|---:|
| feed: decode + book + snapshot | 42 | 83 | 125 |
| decide: quoter | 42 | 42 | 83 |
| risk: 8 checks | 42 | 250 | 625 |
| send: gateway + OUCH + SoupBinTCP frame | 42 | 208 | 500 |
| **tick-to-trade** (arrival → bytes on the wire) | **292** | **625** | **1,333** |

12.2 M messages/s through the whole path. The second thread costs ~160 ns at
the median and both replays send byte-identical orders. **At the capture's
real pace (40 k msg/s) tick-to-trade p50 is 708 ns, not 292**: caches and
predictors cool between messages 25 µs apart. That effect, measured, is the
point of the paced replay. 636 orders, 2,622 replaces, 634 fills.

## Part 1 results

5,000,000-message synthetic day (50 % passive adds, 38 % cancels, 6.5 %
modifies, 5.6 % aggressive), one thread, Apple M2 Pro:

| | throughput | mean | p50 | p99 | p99.9 |
|---|---:|---:|---:|---:|---:|
| v1, std containers | 16.4 M msg/s | 61 ns | 83 ns | 167 ns | 375 ns |
| **v2** (clang 21) | **71.3 M msg/s** | **14 ns** | **42 ns** | **125 ns** | **167 ns** |
| **v2** (GCC 16) | **90.5 M msg/s** | **11 ns** | 42 ns | 83 ns | 167 ns |

The per-message columns are quantized to the M2's 41.67 ns counter tick (see
[the method](docs/BENCHMARKS.md#what-the-numbers-mean-and-what-they-do-not)).
The same day on an x86-64 Linux runner (GitHub Actions, GCC 14, `rdtsc` at
0.385 ns per tick):

| | throughput | mean | p50 | p99 | p99.9 |
|---|---:|---:|---:|---:|---:|
| v1 | 23.4 M msg/s | 43 ns | 50 ns | 120 ns | 190 ns |
| **v2** | **72.7 M msg/s** | **14 ns** | **30 ns** | **60 ns** | **130 ns** |

Cachegrind on that run: **v1 executes 435 instructions per message, v2
executes 180**, and 36 % of v1's are inside glibc's `malloc`/`free`
([docs/BENCHMARKS.md](docs/BENCHMARKS.md#x86-64-linux-github-actions-runner)).

**Where the time goes** (sampling profiler, [docs/DESIGN.md](docs/DESIGN.md#where-v1-spends-its-time-measured)):
49 % of v1's samples are inside `malloc`/`free`; the matching arithmetic is 4 %.
v2 spends 0 % in the allocator and makes zero heap calls on the hot path, which
is a test, not a claim.

**The interesting part:** the first v2 was *slower* than v1 per add. A hash
map sized for 1M orders (32 MB) turned every new order id into a cache miss,
and the throughput number hid it. The isolation and the fix are written up in
[docs/BENCHMARKS.md, experiment 2](docs/BENCHMARKS.md#experiment-log).

## How correctness is checked

* **Spec tests.** Every rule in [docs/SPEC.md](docs/SPEC.md) has a test, and
  each test runs against v1, v2 and a brute-force reference book (78 cases).
* **Differential property tests.** Random collision-heavy message sequences.
  After *every* message the three books must agree on events and on full
  depth. Failures are **shrunk** to a minimal reproduction automatically.
* **Component tests.** The flat hash map against `std::unordered_map`, and the
  bitmap against `std::set`.
* **Allocation test.** Global `operator new`/`delete` are counted during a
  200,000-message replay: **v2 makes zero heap calls**, v1 makes 1.9 per
  message, and v2 allocates again once it outgrows its reservation (the
  guarantee's limit, tested too).
* **ASan + UBSan**, and **GCC and Clang** with `-Wall -Wextra -Wconversion -Werror`, in CI.
  (The allocation test replaces the global allocator, as the sanitizers do, so
  it runs in the ordinary builds only.)
* **Day-scale check.** The benchmark refuses to report unless v1's and v2's
  event-stream hashes match.
* **Part 2:** ITCH, OUCH and SoupBinTCP decoded against hand-built bytes
  from the specifications; the feed handler and market book against a
  map-based reference after every message of random multi-symbol streams;
  every risk check and its order; the quoter's state machine race by race;
  the gateway and simulator loop in bytes, including the exposure invariant
  under 2,000 random operations; the whole path end to end, and one thread
  versus two by hash. **141 tests.**

## Build and run

Requires CMake ≥ 3.20 and a C++20 compiler. GoogleTest is found if installed,
otherwise fetched.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build -j                         # 141 tests

./build/gen_day --messages 5000000 --seed 42 --out data/day.bin
./build/replay --day data/day.bin --runs 7        # verify, throughput, latency

cmake -S . -B build-asan -DLOB_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug   # sanitizers
LOB_PROPERTY_SEEDS=20000 ./build/test_property    # long property run

./build/gen_itch --messages 2000000 --symbols 8 --out data/capture.itch   # part 2
./build/tick_to_trade --capture data/capture.itch --symbol AAPL --check    # 1 vs 2 threads identical
./build/tick_to_trade --capture data/capture.itch --symbol AAPL --threads 1
./build/tick_to_trade --capture data/capture.itch --symbol AAPL --mode paced --speed 1
```

## Layout

```
include/lob/                                   part 1
  types.hpp            Price (ticks), Qty, OrderId, Command (24 bytes), BookConfig
  events.hpp           Listener concept, Trade, recording/counting listeners
  book_v1.hpp          v1: std::map + std::list + std::unordered_map
  book_v2.hpp          v2: level arrays + bitmap + pool + flat hash map
  reference_book.hpp   brute-force oracle for the tests
  flat_hash_map.hpp    open addressing, Fibonacci hashing, backward-shift delete
  level_bitmap.hpp     3-layer bitmap, next/prev set bit via ctz/clz
  clock.hpp            rdtsc / cntvct_el0 with calibration
                                               part 2
  wire.hpp             big-endian fields         itch.hpp         ITCH 5.0 decode/encode
  feed.hpp             feed handler, order table, stream reader
  market_book.hpp      the exchange's book mirrored; BookSnapshot
  quoter.hpp           market maker               risk.hpp         pre-trade checks
  ouch.hpp             OUCH 4.2                   soupbin.hpp      SoupBinTCP framing + session
  gateway.hpp          order state, exposure      exchange_sim.hpp queue-position fills
  histogram.hpp        log-linear latency histogram
  spsc.hpp             single-producer single-consumer ring
  path.hpp             FeedSide, Strategy, stage timestamps
tests/                 15 suites, 141 tests
bench/                 gen_day, replay (part 1); gen_itch, tick_to_trade (part 2)
docs/                  SPEC, DESIGN, BENCHMARKS, LEARNING (part 1); PATH, PROTOCOLS, STRATEGY (part 2)
results/               raw benchmark output, profiles, tick-to-trade runs
```

## Documents

* [SPEC.md](docs/SPEC.md): exactly what the matching engine does.
* [DESIGN.md](docs/DESIGN.md): v1 → v2, the invariants, the trade-offs, the measured profile.
* [BENCHMARKS.md](docs/BENCHMARKS.md): the engine's method, results, and experiment log.
* [PATH.md](docs/PATH.md): the trading path, the queue-position model, the stage measurements.
* [PROTOCOLS.md](docs/PROTOCOLS.md): ITCH 5.0, OUCH 4.2 and SoupBinTCP as implemented, field by field.
* [STRATEGY.md](docs/STRATEGY.md): the quoter's rule and state machine; the risk gate's eight checks.
* [LEARNING.md](docs/LEARNING.md): reading order, exercises, and interview questions for both parts.

## Next

The packet layer under the feed (sequenced UDP, A/B arbitration, gap
recovery), then the same measurement on a pinned, isolated Linux core with
the exchange on the far side of a real socket.
