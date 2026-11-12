# Benchmarks: method, results, and the experiment log

## The day

`gen_day` writes a synthetic day of **5,000,000 messages** for one symbol
(seed 42, so it is bit-for-bit reproducible). The mix follows what a public
equity feed looks like for a liquid name. Most messages are passive orders near
the touch that get cancelled, and only a few percent trade:

| | share |
|---|---:|
| passive limit orders (rest) | 49.8 % |
| cancels | 38.1 % |
| modifies (size down, or re-price) | 6.5 % |
| aggressive orders (Limit / IOC / Market / FOK that cross) | 5.6 % |
| cancels / modifies of an id that is no longer live | 0.45 % |

Passive prices are drawn a geometric number of ticks behind the touch, so most
orders sit within a few ticks of the mid. The mid follows a slow random walk.
The generator runs the v1 book as it goes, so cancels and modifies target
orders that are really live. About 2,000 orders rest at any moment.

## The harness (`replay`)

Each book gets three passes, and every pass starts from a fresh book:

1. **Verify.** v1 and v2 replay the whole day into a hashing listener. The two
   64-bit hashes of their event streams must match, or the run stops. On this
   day both print `5f2b263ab67effe9`.
2. **Throughput.** The day runs flat out, with one clock read around the whole
   loop and none per message. Repeated 7 times; the median run is reported.
3. **Latency.** A timestamp is taken before and after every message, and the
   result is reported as a distribution per kind of message.

### What the numbers mean, and what they do not

* **This is service time, not response time.** Messages are replayed back to
  back. Nothing queues in front of the book, so *coordinated omission* (a slow
  message hiding the wait of the messages behind it) does not arise. A paced
  replay at a fixed arrival rate would measure response time; that is the next
  step, in Module 8 of the roadmap (tick-to-trade).
* **One core, warm caches, listener that only counts.** No I/O and no
  allocation in the listener. A real gateway would add serialization.
* **Clock resolution matters.** On Apple Silicon the user-space counter
  (`cntvct_el0`) ticks at 24 MHz: **one tick = 41.67 ns**. A message that
  takes 15 ns reads as 0 or 1 tick, so values like *42 / 83 / 125* are
  multiples of one tick and the p50 column says "about one tick". The mean of
  the per-message column is still unbiased because the start phase is random,
  and the throughput number has no such limit. On x86 the harness uses
  `rdtsc` (~0.3 ns).
* **Tails on a laptop are noisy.** p99.99 and max include interrupts, the
  scheduler and whatever else macOS is doing. Compare them between runs
  carefully; they are honest but not stable. A pinned core on an isolated
  Linux machine is the right place to chase them (roadmap Module 6).

## Results: Apple M2 Pro, macOS, Apple clang 21, `-O3`

From `results/m2pro.md`. Latency columns are nanoseconds, including the
timestamp overhead.

| book | throughput | ns/msg (mean) | p50 | p90 | p99 | p99.9 |
|---|---:|---:|---:|---:|---:|---:|
| v1, std containers | 16.4 M msg/s | 61.0 | 83 | 83 | 167 | 375 |
| **v2, arrays + pool + flat map** | **71.3 M msg/s** | **14.0** | **42** | **42** | **125** | **167** |
| | **4.35×** | | | | | |

Same machine and same day, built with **GCC 16** (`results/m2pro-gcc16.md`):
v1 17.0 M msg/s, v2 **90.5 M msg/s (11.0 ns/msg), 5.33×**, v2 p99 83 ns. v1
barely changes between compilers, while v2 gains about 25 %. Once the memory
behaviour is fixed, code generation starts to matter. Comparing the two
compilers' output for `match()` with `objdump -d` is a good exercise.

Per kind of message, v2 (mean ns): passive add 30, aggressive add 41, cancel
34, modify 46. For v1: 73, 65, 74, 121. Modify is v1's most expensive message.
It does two tree lookups (one to find the level, one to drop it when emptied)
and frees and allocates a list node when the order re-enters.

## x86-64, Linux (GitHub Actions runner)

The CI job `profile` replays the same day on an `ubuntu-24.04` runner (x86-64,
GCC 14, `-O3`). It is a shared virtual machine, so the throughput is
indicative, but `rdtsc` ticks every **0.385 ns** there, so the per-message
distributions are at a resolution the M2's counter cannot give. Timestamp-pair
overhead on that machine: p50 10 ns, p99 20 ns, included in the columns.
Raw output: `results/linux-x86/`.

| book | throughput | mean ns/msg | p50 | p90 | p99 | p99.9 | p99.99 |
|---|---:|---:|---:|---:|---:|---:|---:|
| v1 | 23.4 M msg/s | 42.7 | 50 | 70 | 120 | 190 | 1212 |
| **v2** | **72.7 M msg/s** | **13.8** | **30** | **40** | **60** | **130** | 260 |
| | **3.10×** | | | | | | |

Per kind, v2 mean: passive add 24 ns, aggressive add 33, cancel 27, modify 43.
v1: 42, 53, 58, 93. v1 is faster on this machine than on the M2 (glibc's
allocator is cheaper than macOS's), so the ratio is 3.1× rather than 4.4×; v2
costs the same 14 ns on both.

### Cachegrind: instructions and cache misses, independent of the machine

`valgrind --tool=cachegrind --cache-sim=yes` on 300,000 messages, same job.
Cachegrind counts every instruction and simulates the caches, so the numbers do
not depend on what else the runner was doing.

| per message | v1 | v2 |
|---|---:|---:|
| instructions executed | **435** | **180** |
| instructions inside glibc `malloc` / `free` | 157 (36 %) | 0 |
| instructions inside the book's own code | 72 (17 %) | 112 (62 %) |

Where v2's L1 data-read misses land: **57 % in the id hash map**
(`flat_hash_map.hpp`), 40 % in `book_v2.hpp` (levels and pool). The table is
2 MB, which fits the last-level cache but not the 48 KB L1, so every id lookup
is an L1 miss served from L2. That is the largest remaining memory cost in v2
and the natural next target (a smaller table, or prefetching the slot as soon
as the id is known, before the rest of the message is decoded).

Two cautions when reading cachegrind: it models no hardware prefetcher, so
sequential reads of the day array count as misses that real hardware hides;
and the one-time setup (reading the 7.2 MB day file, zeroing the pool, level
arrays and hash table) is inside the totals, which is why v2's last-level
*write* misses exceed v1's.

## Experiment log

### Experiment 1 — baseline v1
16–17 M msg/s across runs. Each message pays for at least one heap node
(`std::map` level, `std::list` order, `std::unordered_map` entry) and a
pointer chase through each structure.

### Experiment 2 — the first v2 was *slower* per message than v1 on adds

The first v2 was sized for the worst case: `expected_orders = 1 << 20`, which
gives a 2M-slot, **32 MB** id hash map. It reached only **28.9–30.2 M msg/s**.
Measured one message at a time, its adds were *slower* than v1's (mean 128 ns
against 73 ns; p99.99 ~4.5 µs).

**Hypothesis:** the hash map. Order ids are sequential, and Fibonacci hashing
scatters them on purpose, so every *new* id lands on a random line of a 32 MB
table, far bigger than the cache. That is one cache miss per add. In the
flat-out pass the out-of-order core overlaps misses from neighbouring
messages, which hides most of the cost. With a serializing timestamp around
each message, the whole miss shows up.

**Isolation:**

| v2 configuration | throughput |
|---|---:|
| pool 1M, hash map sized for 1M (32 MB) | 28.9 M msg/s |
| pool 1M, hash map sized for 64K (2 MB) | 72.3 M msg/s |
| pool 64K, hash map 64K | 72.2 M msg/s |
| pool 8K, hash map 8K (grows on demand) | 71.5 M msg/s |

The pool's size is irrelevant: its free list is LIFO, so the node reused next
is the one freed last, which is still in cache. **All of the effect was the
hash table's size.** v1 barely moves (17.0 vs 16.2 M msg/s) because
`std::unordered_map` with an integer key uses the identity hash. Sequential ids
land in sequential buckets, so v1 hits the cache by accident.

**Fix:** size for the working set (the orders resting *at once*, a few
thousand), not for the day. The table still grows if a burst needs it.
`BookConfig::expected_orders` now documents this and defaults to 65,536.

**Lesson:** a throughput number alone would have hidden this. Per-message
distributions exposed it, and changing one variable at a time explained it.

## Reproduce

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/gen_day --messages 5000000 --seed 42 --out data/day.bin
./build/replay  --day data/day.bin --runs 7 --json results/mine.json
./build/replay  --day data/day.bin --book v2 --expected 1048576   # experiment 2
```

On Linux, add `--pin 3` to pin the thread to a core, and see
[DESIGN.md](DESIGN.md#reproduce-it) for profiler commands. Every push also
runs the `profile` CI job and uploads `replay-x86.md` and `cachegrind.md` as a
build artifact.
