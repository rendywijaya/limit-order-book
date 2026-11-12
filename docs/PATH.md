# The path from market data to order

Part 2 of this repository grows the matching engine's data structures into
the whole trading path: a market-data feed decoded from the exchange's own
bytes into books, a market-making quoter that decides from the top of book,
a pre-trade risk gate, an order gateway that speaks the exchange's order
protocol inside its session protocol, and a simulated exchange so the loop
closes under replay, with a timestamp at every stage.

```
 ITCH message ──► FeedHandler ──► MarketBook ──► snapshot ──► Quoter ──► RiskGate ──► Gateway ──► bytes
 (bytes)          decode,         per symbol     top + 5      intents    fixed order   OUCH in      to the
                  order table     levels array,  levels                  of integer    SoupBinTCP   exchange
                                  bitmap                                  checks        frames
                                                                                           ▲   │
                                                   ExchangeSim ◄───────────────────────────┘   │
                                                   accepts / replaces / cancels / rejects,      │
                                                   fills by queue position from the feed  ──────┘ answers
```

Every stage is a plain function call on one thread, or, in the two-thread
mode, the feed side publishes book snapshots through an SPSC ring to the
strategy side. Nothing on the per-message path allocates after warm-up; that
is a test (`tests/test_alloc.cpp`, `TheWholePathMakesNoHeapCallsAfterWarmUp`).

## The pieces

| piece | file | what it does |
|---|---|---|
| wire helpers | `wire.hpp` | big-endian integers of 2/4/6/8 bytes, space-padded alpha fields; the only place byte order lives |
| ITCH 5.0 | `itch.hpp` | decode every message type's length; fields for the ones a book needs; encoders for the synthetic capture |
| feed handler | `feed.hpp` | one order table (pool + flat hash map, keyed by the exchange's reference) and one `MarketBook` per stock locate code; applies add / execute / cancel / delete / replace; reports whether the top moved |
| market book | `market_book.hpp` | the exchange's book mirrored: level arrays indexed by price, bitmap for the best, aggregated quantity and count per level; `BookSnapshot` (top + 5 levels) for the strategy |
| quoter | `quoter.hpp` | fair = mid − skew × position; bid/ask a half-spread either side, post-only, requote only on a real move, one request in flight per side; a state machine per side for the races |
| risk | `risk.hpp` | kill switch, reference price, size, collar, notional, open orders, worst-case position, message rate, in that order, in integers; cancels always pass |
| OUCH 4.2 | `ouch.hpp` | Enter / Replace / Cancel out; Accepted / Replaced / Canceled / Executed / Rejected / CancelReject in; byte-exact |
| SoupBinTCP | `soupbin.hpp` | framing, a stream re-assembler that handles any split, the client session: login, sequence numbers for recovery, heartbeats, timeout |
| gateway | `gateway.hpp` | tokens, encoding, the order state machine from the exchange's answers, the `Exposure` risk reads, a cash ledger |
| exchange sim | `exchange_sim.hpp` | the other end of the socket: real bytes in and out; fills resting orders by **queue position** driven by the feed |
| histogram | `histogram.hpp` | 64 × 16 log-linear buckets, O(1) record, 8 KB, percentiles on demand |
| SPSC ring | `spsc.hpp` | the feed-to-strategy hop for the two-thread mode |
| path | `path.hpp` | `FeedSide`, `Strategy`, the stage timestamps |
| harness | `bench/tick_to_trade.cpp` | flat-out or paced replay, one or two threads, determinism check |
| capture | `bench/gen_itch.cpp` | a synthetic ITCH day: 8 symbols, realistic mix, a touch that moves |

## How a fill happens: the queue-position model

Our orders do not appear on the feed (the feed is a recording; we are a shadow
participant), so the simulator has to decide when we would have been filled.
It uses the standard replay model:

1. When our bid rests at price P, it joins the back of the exchange's bid
   queue at P: `queue_ahead` = the quantity the book shows at P.
2. Every execution the feed shows against resting bids at P (ITCH `E` on an
   order the feed handler knows is a bid at P) consumes `queue_ahead` first.
3. Once `queue_ahead` is exhausted, further executions at P fill us.
4. If the market trades through us (best ask falls to P or below), we are
   filled in full: whoever sold through must have hit our order.
5. A new order that crosses the touch takes what is there (fills at the
   touch, liquidity flag `R`), and the remainder rests.
6. A replace at the same price for a smaller size keeps its place; any other
   replace goes to the back of its new level, as on Nasdaq.

This is deterministic: it depends only on the feed and our own orders. It is
optimistic in one way (it assumes no one queues behind us and jumps ahead,
which cannot happen under price-time priority) and pessimistic in another
(executions at our price on hidden or odd-lot orders count against the queue
even if they were not in it). Both are the usual caveats of replay.

## The measurement

`tick_to_trade` reads the capture into memory and replays it through the path.
Clock: the hardware counter (`cntvct_el0`, 41.67 ns per tick on Apple
Silicon; `rdtsc`, 0.385 ns on the x86 CI runner). The first 100,000 messages
are warm-up and not recorded.

| stage | from → to | recorded for |
|---|---|---|
| feed | arrival → decode + book + snapshot done | every message |
| queue | snapshot done → strategy thread picked it up | every update that reached the strategy (0 on one thread, by construction one tick) |
| decide | pick-up → quoter decided | every update with a top change |
| risk | decided → risk answered | every intent |
| send | risk → bytes on the wire (gateway state, OUCH encode, SoupBinTCP frame) | every intent that passed |
| exchange | bytes on the wire → the simulated answer processed | the same; not part of tick-to-trade |
| wire-to-decision | arrival → decided | every update with a top change |
| **tick-to-trade** | arrival → bytes on the wire | every message that produced at least one order |
| lateness | due time → actually picked up | paced mode, every message |

**Arrival** is `now()` in flat-out mode and the message's **due time** in
paced mode (capture time scaled by `--speed`). Charging a message from its
due time, not from when we got to it, means that waiting behind a slow
predecessor counts: that is the response-time measurement, and it is immune
to coordinated omission. Lateness is reported separately so you can see when
the replay itself fell behind.

## Results: Apple M2 Pro, capture of 2,000,000 messages, 8 symbols, 50 s of exchange time, quoting AAPL

From `results/t2t-*.md`. The quoted symbol had 250,842 messages and 70,370 top
changes; the quoter sent **636 new orders and 2,622 replaces**, was filled
**634 times for 63,400 shares**, ended flat, with a mark-to-market of
+39,500 ticks × shares (a toy strategy on synthetic data; the number that
matters is that the loop closes). Latencies in nanoseconds, quantized to the
41.67 ns tick.

### Flat out, one thread (service time)

| stage | count | mean | p50 | p90 | p99 | p99.9 |
|---|---:|---:|---:|---:|---:|---:|
| feed: decode + book + snapshot | 1,900,011 | 54 | 42 | 83 | 125 | 917 |
| decide: quoter | 66,656 | 44 | 42 | 42 | 83 | 167 |
| risk: pre-trade checks | 3,068 | 97 | 42 | 250 | 625 | 875 |
| send: gateway + OUCH + SoupBin | 3,068 | 110 | 42 | 208 | 500 | 792 |
| exchange (simulated, not ours) | 3,068 | 155 | 167 | 208 | 250 | 500 |
| **tick-to-trade** | 2,466 | **392** | **292** | **625** | **1,333** | 4,500 |

Throughput of the whole path: 12.2 M messages/s on one thread.

### Flat out, two threads (feed thread → SPSC → strategy thread)

| stage | mean | p50 | p90 | p99 | p99.9 |
|---|---:|---:|---:|---:|---:|
| queue: the hop | 155 | 83 | 167 | 500 | 10,333 |
| **tick-to-trade** | 509 | **458** | 708 | 1,250 | 7,333 |

The second thread costs about **160 ns at the median** (one cache-line
transfer each way for the snapshot and the index) and gives the tail of a
thread that is not pinned: p99.9 of the hop is 10 µs on macOS, which has no
core isolation. Both replays send **byte-identical orders** (`--check`:
hash `1b9d8a0e2b46a35b` on both), because the strategy sees the same sequence
of snapshots either way.

### At the capture's pace (response time)

| run | feed p50 | tick-to-trade p50 | p90 | p99 | p99.9 | lateness p99 |
|---|---:|---:|---:|---:|---:|---:|
| 10× capture speed (400 k msg/s), 1 thread | 42 | 417 | 917 | 2,833 | 9,000 | 708 |
| 1× capture speed (40 k msg/s), 1 thread | **167** | **708** | 2,083 | 5,667 | 14,000 | 417 |
| 1× capture speed, 2 threads | 167 | 1,000 | 3,500 | 7,000 | 15,333 | 458 |

**The path is slower when the market is slower.** At 40 k messages/s a
message arrives every 25 µs, and in 25 µs the core does other things: the
decoder's code, the book's lines, the branch history and the TLB entries
have been partly evicted when the next message comes, and the CPU may have
dropped its clock. Flat out, everything is hot. This is the single most
important thing the paced replay shows, and it is why production systems
busy-poll and keep the hot path warm with dummy work during quiet periods.
It is also why a benchmark that only runs flat out overstates how a system
behaves in the market.

Lateness p99 under 1 µs at both speeds says the replay kept to the schedule;
the long tails in the stages are the operating system, not the queue
discipline.

### What the numbers do not say

* One instrument quoted, eight tracked. A real system quotes hundreds; the
  decide and risk stages scale with the number of quoted instruments, the
  feed stage with the total message rate.
* The exchange is in the same process and answers synchronously. A real
  exchange answers tens of microseconds later over a wire; the gateway's
  state machine is written for that, and the simulator's synchronous answer
  is the reason the one-thread and two-thread replays can be compared by
  hash.
* No kernel bypass, no network card, no packet framing. The feed enters as a
  message stream (the format of Nasdaq's public sample files). The layer
  below it on the wire, sequenced UDP packets with A/B feed arbitration and
  gap recovery, is the first exercise in LEARNING.md and the next thing to
  add; its cost sits in front of the feed stage.
* macOS, no pinning, no isolation. The x86 Linux CI run (`results/linux-x86/`,
  when present) gives finer resolution; a pinned core on an isolated Linux
  machine is where the tails should be read.

## Reproduce

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/gen_itch --messages 2000000 --symbols 8 --out data/capture.itch
./build/tick_to_trade --capture data/capture.itch --symbol AAPL --check            # determinism
./build/tick_to_trade --capture data/capture.itch --symbol AAPL --threads 1
./build/tick_to_trade --capture data/capture.itch --symbol AAPL --threads 2
./build/tick_to_trade --capture data/capture.itch --symbol AAPL --mode paced --speed 1
```

On Linux add `--pin 2,3` to pin the feed and strategy threads.
