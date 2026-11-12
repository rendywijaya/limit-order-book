# lob: a limit order book and matching engine in C++20, built twice

A price-time priority matching engine for one instrument, with **Limit,
Market, IOC and FOK** orders, cancels and modifies. It is built twice:

* **v1** uses the standard containers and is simply correct.
* **v2** is rewritten after measuring v1: price levels in arrays, a
  hierarchical bitmap to find the best level, orders in an index-linked pool,
  and an open-addressing hash map. **No allocation on the hot path.**

Both pass the same tests and produce **identical event streams** over a
5-million-message day.

## Results

5,000,000-message synthetic day (50 % passive adds, 38 % cancels, 6.5 %
modifies, 5.6 % aggressive), one thread, Apple M2 Pro:

| | throughput | mean | p50 | p99 | p99.9 |
|---|---:|---:|---:|---:|---:|
| v1, std containers | 16.4 M msg/s | 61 ns | 83 ns | 167 ns | 375 ns |
| **v2** (clang 21) | **71.3 M msg/s** | **14 ns** | **42 ns** | **125 ns** | **167 ns** |
| **v2** (GCC 16) | **90.5 M msg/s** | **11 ns** | 42 ns | 83 ns | 167 ns |

The per-message columns are quantized to the M2's 41.67 ns counter tick (see
[the method](docs/BENCHMARKS.md#what-the-numbers-mean-and-what-they-do-not)).

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
* **ASan + UBSan**, and **GCC and Clang** with `-Wall -Wextra -Wconversion -Werror`, in CI.
* **Day-scale check.** The benchmark refuses to report unless v1's and v2's
  event-stream hashes match.

## Build and run

Requires CMake ≥ 3.20 and a C++20 compiler. GoogleTest is found if installed,
otherwise fetched.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build -j                         # 86 tests

./build/gen_day --messages 5000000 --seed 42 --out data/day.bin
./build/replay --day data/day.bin --runs 7        # verify, throughput, latency

cmake -S . -B build-asan -DLOB_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug   # sanitizers
LOB_PROPERTY_SEEDS=20000 ./build/test_property    # long property run
```

## Layout

```
include/lob/
  types.hpp            Price (ticks), Qty, OrderId, Command (24 bytes), BookConfig
  events.hpp           Listener concept, Trade, recording/counting listeners
  book_v1.hpp          v1: std::map + std::list + std::unordered_map
  book_v2.hpp          v2: level arrays + bitmap + pool + flat hash map
  reference_book.hpp   brute-force oracle for the tests
  flat_hash_map.hpp    open addressing, Fibonacci hashing, backward-shift delete
  level_bitmap.hpp     3-layer bitmap, next/prev set bit via ctz/clz
tests/                 spec (typed), property (differential + shrinking), components
bench/                 gen_day (synthetic day), replay (harness), clock (rdtsc / cntvct)
docs/                  SPEC, DESIGN, BENCHMARKS, LEARNING
results/               raw benchmark output
```

## Documents

* [SPEC.md](docs/SPEC.md): exactly what the engine does.
* [DESIGN.md](docs/DESIGN.md): v1 → v2, the invariants, the trade-offs, how to profile.
* [BENCHMARKS.md](docs/BENCHMARKS.md): the method, the results, the experiment log.
* [LEARNING.md](docs/LEARNING.md): a guided reading order, exercises, and interview questions.

## Next

Module 8 of the plan: an ITCH feed handler that decodes raw packets into this
book, a quoter, pre-trade risk, an OUCH gateway, and tick-to-trade measured
per stage on a pinned Linux core.
