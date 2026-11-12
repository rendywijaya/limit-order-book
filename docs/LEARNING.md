# Learning guide: own this project

This repository is the Module 5 project of the HFT roadmap. Code you cannot
explain counts against you in an interview, because the interviewer will push
on whatever is on your CV until you stop knowing the answer. This guide gets
you to the point where every line is yours: read it, break it, rebuild it,
defend it.

## Why this project matters for low-latency SWE roles

Trading firms hire low-latency engineers to work on exactly this kind of code:
matching engines, feed handlers, order gateways. This repository shows the
four things those interviews probe:

1. **Data structures under a latency budget.** Trees vs arrays, node-based vs
   flat containers, intrusive lists, hashing, and *why* each is fast or slow
   on real hardware (cache lines, allocation, pointer chasing).
2. **Correctness under pressure.** A precise spec, tests at the rule level,
   and randomized differential testing against an oracle, with shrinking.
3. **Measurement discipline.** Distributions, not averages. Clock resolution,
   timer overhead, service time vs response time, and an experiment that
   overturned the first result (BENCHMARKS.md, experiment 2).
4. **Engineering hygiene.** CMake, warnings as errors, sanitizers, CI, a README
   an engineer can check.

"I built an order book" is common. "My first rewrite was slower per message,
here is how I found out it was a 32 MB hash table and proved it" is rare, and
it is the conversation you want.

## Reading order (about 10 sessions, mapped to the roadmap lessons)

| # | Read | Roadmap lesson | You can answer… |
|---|---|---|---|
| 1 | `CMakeLists.txt`, `.github/workflows/ci.yml` | M5 L01 | Why `-Werror`? What do ASan and UBSan catch? Why test on two compilers? |
| 2 | `docs/SPEC.md`, `include/lob/types.hpp` | M5 L02 | Why integer ticks? What is price-time priority? Why does modify-up lose priority? |
| 3 | `include/lob/book_v1.hpp` | M5 L03 | Walk through `add()` for an aggressive order. Why store a list iterator in the index? |
| 4 | `include/lob/reference_book.hpp`, `tests/test_spec.cpp` | M5 L04 | Why is a slow reference useful? What is a typed test? |
| 5 | `tests/test_property.cpp` | M5 L04 | How does shrinking work? Why do the generators *aim* for collisions? |
| 6 | `bench/gen_day.cpp`, `bench/replay.cpp`, `bench/clock.hpp` | M5 L05 | Why a fixed seed? Why `isb`/`lfence` around the counter? What is coordinated omission, and why does it not apply here? |
| 7 | `docs/DESIGN.md` § "Where v1 spends its time", then reproduce it | M5 L06 | Where does v1 spend its time? Quote the numbers (49 % allocator, 4 % matching), then show a run of your own. |
| 8 | `flat_hash_map.hpp`, `level_bitmap.hpp`, `book_v2.hpp` | M5 L07 | Backward-shift deletion: why no tombstones? How does `next_ge` work across layers? |
| 9 | `docs/BENCHMARKS.md` | M5 L08 | Explain experiment 2 end to end, without notes. |
| 10 | This file's interview section | M5 L10 | All of it, out loud, in under two minutes per answer. |

## Exercises: make it yours

Do these in your own branch. Each one is a real change with a test.

1. **Re-implement v2 from a blank file** with only SPEC.md and the tests. When
   the property test passes, compare your version with this one. This is the
   most valuable exercise here.
2. **Break it on purpose.** Plant one bug in each of v2, `FlatHashMap` and
   `LevelBitmap`. Watch which test catches each, and read the shrunk
   reproduction. Then add one `std::vector` push to v2's `add()` and watch
   `test_alloc` fail.
3. **Self-trade prevention.** Add an owner id to orders. When a taker would
   trade with the same owner's resting order, cancel the resting order (one
   common policy). Update the spec, all three books, and the generator.
4. **One shared level array.** The book is never crossed, so bids and asks
   can share one `Level` array. Implement it and measure it. Is it faster?
   Why or why not?
5. **Identity hash.** Replace Fibonacci hashing with `key & mask`. Measure.
   Then generate a day with random (non-sequential) ids and measure again.
   What does this tell you about choosing a hash for exchange-assigned ids?
6. **Paced replay.** Add `--rate N` to `replay`: messages are due at
   `i / N` seconds, and latency is measured from the *due* time, not the send
   time. Push N close to throughput and watch p99 explode. That is
   coordinated omission made visible.
7. **Linux run.** On any Linux box (a cloud VM is fine), run with `--pin`,
   then with `isolcpus` and `nohz_full`. Compare tails (roadmap M6 L10).
8. **Attack the hash map.** Cachegrind says 57 % of v2's L1 read misses are
   id lookups. Try `__builtin_prefetch` on the home slot at the top of
   `submit()`, before the switch; try a smaller table; try storing the pool
   index for recently seen ids in a tiny direct-mapped cache in front of the
   map. Measure each on the M2 and in the CI job. Keep only what the
   distributions justify.

## Interview questions you will be asked, with the answers you should give

**"Walk me through your order book."**
> Price-time priority for one instrument, with four order types, cancels and
> modifies, built twice. v1 uses std::map of levels, std::list per level and
> an unordered_map from id to list iterator: correct and readable. I profiled
> it, saw that allocation and pointer chasing dominated, and rewrote the data
> structures for v2: levels in an array indexed by price, a 3-layer bitmap to
> find the next best level, orders in a pool linked by 32-bit index, and an
> open-addressing hash map. The same tests pass on both, plus a differential
> test against a brute-force reference after every random message. On a 5M
> message day v2 is about 4–5× the throughput (14 ns vs 61 ns per message on
> an M2 Pro), and p99.9 drops from 375 to 167 ns.

**"Why is std::map slow here? It's O(log n)."**
> Big-O counts comparisons, but the cost here is memory. Every node is a
> separate allocation, so a lookup chases pointers to unpredictable addresses,
> and each hop can be a cache miss of around 100 ns. Insert and erase also
> call the allocator. I measured it: 49 % of v1's samples are inside malloc
> and free, and the matching arithmetic is 4 %. An array indexed by price is
> one address calculation and usually one cache line, and v2 makes zero heap
> calls on the hot path, which is a test in the repo.

**"What happens when the price leaves your array window?"**
> In this version the order is rejected, and all three books enforce the same
> window so the tests agree. In production you would re-centre the window
> around the market (prices move slowly compared with message rates) or send
> far-away prices to a small sorted fallback structure. Those orders are
> rarely near the touch, so they are rarely on the hot path.

**"How do you find the next best level after one empties?"**
> A hierarchical bitmap: one bit per level, plus summary words above it. To
> find the next set bit above i, I mask the word, count trailing zeros, and if
> the word is empty I go up a layer and come back down. That is at most a few
> word loads and bit-scan instructions, not a walk over empty prices.

**"Why open addressing instead of std::unordered_map?"**
> unordered_map is a vector of linked lists, so a lookup is a bucket load and
> then a pointer chase to a node. Linear probing keeps keys and values in one
> array, so a lookup is almost always one cache line. I keep the load factor
> at 1/2 or below and delete by backward shift, which moves later entries back
> instead of leaving tombstones, so probe lengths don't grow over the day.

**"How do you know v2 is correct?"**
> Three layers. Typed spec tests run the same test bodies against all books.
> A differential property test compares v2, v1 and a brute-force reference
> after every message on random collision-heavy sequences, and shrinks any
> failure to a few messages. I checked that it catches bugs by planting one: a
> `>=` changed to `>` in the fill-or-kill check was caught and shrunk to six
> messages. Over a full 5M-message day, v1's and v2's event streams hash
> identically.

**"How did you measure latency? Can you trust it?"**
> I timestamp every message with the hardware counter, rdtsc on x86 and
> cntvct_el0 on ARM, with a serializing instruction so the read can't be
> reordered with the work. I report the timer overhead and the resolution: on
> Apple Silicon it's 41.67 ns per tick, so p50 there means "about one tick".
> I report percentiles per message type, not averages. It's service time from
> a back-to-back replay, so coordinated omission doesn't apply; response time
> under load would need a paced replay. Throughput comes from a separate pass
> with no per-message clock reads.

**"Tell me about something that surprised you."**
> My first v2 was 1.8× faster in throughput but *slower* than v1 per add. I
> had sized the hash map for a million orders, 32 MB, and Fibonacci hashing
> sends every new sequential id to a cold cache line. Throughput hid it
> because the out-of-order core overlaps those misses. I isolated it by
> changing only the table size (29 to 72 M msg/s) and only the pool size (no
> change), then sized for the working set. v1 had dodged the problem by
> accident: its identity hash maps sequential ids to sequential buckets.

**"What would you do next?"**
> Put it on the real data path. Decode ITCH from raw packets into the book,
> add a quoter, pre-trade risk checks and an OUCH gateway, and measure
> tick-to-trade per stage on a pinned, isolated Linux core with a paced
> replay. That is Module 8 of my plan.

## Words to know cold
tick, touch, spread, level, depth (L2 / L3), maker / taker, price-time
priority, IOC, FOK, queue position, cache line, false sharing, TLB, prefetch,
branch misprediction, allocation on the hot path, intrusive container,
open addressing, load factor, tombstone, percentiles, coordinated omission,
service time vs response time, rdtsc / invariant TSC, core pinning,
`isolcpus`.
