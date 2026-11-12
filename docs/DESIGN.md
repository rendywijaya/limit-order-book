# Design

Two books implement the same [spec](SPEC.md) behind the same interface:

```cpp
template <Listener L> class BookV1;   // include/lob/book_v1.hpp
template <Listener L> class BookV2;   // include/lob/book_v2.hpp
template <Listener L> class ReferenceBook;   // the test oracle

book.add(id, side, type, price, qty);  book.cancel(id);  book.modify(id, price, qty);
book.submit(const Command&);           // the same three, from a 24-byte record
```

## Why a template on the listener, not a virtual interface

The book reports trades and acknowledgements through `L&`. Because `L` is a
template parameter, the compiler sees the concrete listener at the call site.
It inlines `on_trade`, and a listener that only counts costs one add. A
`virtual` interface would cost an indirect call per event and block inlining.
The C++20 `concept Listener` gives the error messages and documentation that
an abstract base class would have given.

## Version 1: standard containers

| Need | Structure | Cost per operation |
|---|---|---|
| levels sorted by price, best first | `std::map<Price, Level>` (red-black tree), `greater` for bids | O(log L); a heap node per level |
| FIFO at a level, O(1) removal from the middle | `std::list<Order>` | a heap node per order |
| find an order by id (cancel / modify) | `std::unordered_map<OrderId, Loc>` storing the list iterator | O(1) average; a heap node per entry |

It is clearly correct and easy to read, and it is slow for reasons you can
name:
* **Allocation on the hot path.** Adding a resting order allocates a list node
  and a hash node, plus a tree node when the level is new. Freeing them later
  costs as much again.
* **Pointer chasing.** Tree nodes, list nodes and hash nodes sit wherever
  `malloc` put them. Each hop can be a cache miss, and the CPU cannot prefetch
  what it cannot predict.
* **The best level costs a tree walk.** `begin()` on a red-black tree is
  cached, but erasing the best level rebalances the tree.

## Version 2: the rewrite

| v1 | v2 | Why |
|---|---|---|
| `std::map` of levels | **`std::vector<Level>` indexed by `price - min_price`**, one per side | A price is its own index: O(1), and nothing is allocated |
| `map.begin()` | **cached best index + `LevelBitmap`** | When the best level empties, the next one is found with `ctz` / `clz` over 3 layers of 64-bit words |
| `std::list<Order>` | **intrusive doubly-linked list through a pool**: `Node{id, price, qty, prev, next, side}`, 32 bytes, linked by `uint32_t` index | No allocation, two nodes per cache line, and indices half the size of pointers |
| `new` / `delete` | **pool with a LIFO free list** threaded through `next` | The node reused next is the one freed last, which is still in cache |
| `std::unordered_map` | **`FlatHashMap`**: open addressing, linear probing, power-of-two capacity, Fibonacci hashing, backward-shift deletion | Key and value in one array: one cache line per lookup, no nodes, no tombstones |

### Invariants worth stating in an interview
* A level's `total` equals the sum of its nodes' `qty`, and `count` equals
  their number. Both are checked indirectly after every random message by the
  property test, which compares `depth()` with the reference.
* A bit is set in `LevelBitmap` **iff** that level's list is non-empty.
* `best_[side]` is the extreme set bit of that side, or `kNone`.
* The book is never crossed, so a price can only hold bids or asks, never both.
  (v2 still keeps two level arrays, for clarity; a single shared array is a
  valid further optimization.)
* Every live order is in the hash map exactly once, and no dead one is.

### The bitmap search, concretely
`LevelBitmap` keeps one bit per price level (layer 0). Each bit of layer *k+1*
says "word *w* of layer *k* is non-zero". To find the next ask above index *i*:
mask the current layer-0 word above *i* and take `countr_zero`. If that word is
empty, go up a layer, search there for the next non-empty word, and come back
down. That is at most 3 words and 3 bit-scans for 2^18 levels, instead of
walking every empty price.

### Trade-offs v2 accepted
* **Bounded price window.** Array indexing needs `[min_price, max_price]`.
  Real engines re-centre the window or keep a fallback map for far-away
  prices. Here, orders outside the window are rejected, and v1 and the
  reference enforce the same rule so all three books agree.
* **Memory proportional to the window.** 65,536 levels × 24 bytes × 2 sides
  is 3 MB. That is fine for one instrument; with thousands of instruments you
  would size each window from its price and volatility.
* **Sizing matters.** See [BENCHMARKS.md, experiment 2](BENCHMARKS.md#experiment-2--the-first-v2-was-slower-per-message-than-v1-on-adds):
  a hash map sized for the worst case cost v2 more than half its throughput.

## Testing strategy

1. **Spec tests** (`test_spec.cpp`): one test per rule in SPEC.md. They are
   GoogleTest *typed tests*, so the same body runs against v1, v2 and the
   reference: 26 rules × 3 books.
2. **Property tests** (`test_property.cpp`): random sequences built to collide
   (few ids, narrow prices, small sizes, duplicates, zero sizes, edge and
   out-of-window prices). After **every** message, all three books must have
   emitted identical events and must hold identical depth on both sides. On a
   failure, the sequence is **shrunk** by delta debugging and printed as a
   short reproduction. Three profiles: dense, sparse across bitmap words, and
   tiny reservations that force the growth paths.
3. **Component tests** (`test_components.cpp`): `FlatHashMap` against
   `std::unordered_map` under heavy churn in a tiny table (long probe chains
   and wrap-around, where backward-shift deletion breaks if it is wrong), and
   `LevelBitmap` against `std::set` at sizes around word and layer boundaries.
4. **Sanitizers.** The whole suite also runs under ASan and UBSan in CI.
5. **Day-scale verification.** `replay` hashes v1's and v2's event streams over
   5M messages and refuses to report numbers unless the two are identical.

**Does the property test catch real bugs?** Checked by mutation: changing
`avail >= qty` to `avail > qty` in v2's fill-or-kill check fails on seed 1
within 545 messages, and the shrinker reduces it to a 6-message reproduction:

```
add(241, Buy, Limit, 1004, 19)   add(244, Buy, Limit, 1003, 10)
add(247, Sell, Limit, 997, 11)   add(248, Sell, Limit, 1002, 18)
add(254, Sell, Limit, 999, 4)    add(256, Buy, FOK, 1002, 4)
  ref: Accept{256} Trade{taker=256 maker=254 px=999 qty=4 maker_left=0}
  v2 : Accept{256} Cancel{256 left=4 reason=FokNotFilled}
```

## Where v1 spends its time (measured)

Sampling profile (`sample`, 1 ms interval, 6 s) of `replay --book v1
--skip-latency` on the 5M-message day. Build: `-O2 -g -fno-omit-frame-pointer`,
Apple M2 Pro. Self time by top-of-stack symbol, grouped. Raw output is in
`results/profile/sample-v1.txt` and `sample-v2.txt`.

| where the samples land | v1 | v2 |
|---|---:|---:|
| **allocator**: `malloc`, `free`, `operator new`, the allocator's `memset` | **49 %** | **0 %** |
| the book's own methods (the tree, list and hash walks inline into these) | 37 % | 70 % |
| libc++ `__hash_table` / `__tree` internals that did not inline | 6 % | 0 % |
| replay loop, `submit()` dispatch, counting listener | 6 % | 24 % |
| other / unresolved | 2 % | 6 % |

Largest single symbols in v1: `BookV1::unlink` 15 % (erase from the list,
and erase the map node when a level empties), `_xzm_free` 14 %,
`_xzm_xzone_malloc_tiny` 12 %, `BookV1::rest` 10 % (list `push_back`, map
`operator[]`, hash `emplace`), `BookV1::add` 5 %, hash `emplace` 3 %, hash
`remove` 2.5 %, the two `match` instantiations 4 % together.

Three things this profile says:

1. **Half of v1's time is memory management that has nothing to do with
   matching.** Every resting order costs a list node and a hash node on the
   way in, and both again on the way out; every new or emptied level costs a
   tree node. The matching arithmetic itself (`match`) is 4 %.
2. **The structural operations dominate the rest.** `unlink` and `rest`, which
   change the containers, are 25 %. They are where the pointer chasing is.
3. **v2 removed the allocator entirely**, as `tests/test_alloc.cpp` confirms
   (zero heap calls over 200k messages). Its biggest single item is now the
   replay loop and dispatch (24 %), which means the book is approaching the
   cost of merely iterating over the messages. Within the book: `add` 19 %,
   `match` 18 %, `rest` 12 %, `cancel` 9 %, pool `acquire` 7 %.

The Linux CI job `profile` adds two machine-independent views, reported in
[BENCHMARKS.md](BENCHMARKS.md#x86-64-linux-github-actions-runner):
**cachegrind** counts 435 instructions per message for v1 (36 % of them in
glibc `malloc`/`free`) and 180 for v2, and shows that 57 % of v2's remaining
L1 data-read misses are in the id hash map, which is the next thing to
optimize; and an **x86-64 `rdtsc` replay** at 0.385 ns resolution puts v2 at
p50 30 ns, p99 60 ns against v1's 50 and 120.

### Reproduce it

```sh
cmake -S . -B build-prof -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS=-fno-omit-frame-pointer -DLOB_TESTS=OFF
cmake --build build-prof -j
# macOS
./build-prof/replay --day data/day.bin --book v1 --runs 60 --skip-latency & sleep 1; sample $! 6 -file v1.txt
# Linux
perf stat -e cycles,instructions,cache-misses,branch-misses ./build-prof/replay --day data/day.bin --book v1 --runs 1 --skip-latency
perf record -g ./build-prof/replay --day data/day.bin --book v1 --runs 1 --skip-latency && perf report
valgrind --tool=cachegrind --cache-sim=yes ./build-prof/replay --day small.bin --book v1 --runs 1 --skip-latency
```

## What comes next (roadmap Module 8, lessons 12–16)

Grow this repository into the full path from packet to order: an ITCH feed
handler that decodes raw bytes into this book, a quoter that decides from the
book, pre-trade risk checks, an OUCH order gateway, and tick-to-trade latency
measured at every stage, deterministic under replay.
