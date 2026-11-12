# x86-64 Linux results (GitHub Actions `profile` job)

Produced by the CI job in `.github/workflows/ci.yml` on an `ubuntu-24.04`
shared runner (x86-64, GCC 14, `-O3 -fno-omit-frame-pointer`), run 43136693234.

* `replay-x86.md` / `.json`: the full 5,000,000-message day, 5 runs, `rdtsc`
  at 0.385 ns per tick. A shared VM, so absolute throughput is indicative;
  the per-message distributions are at a resolution the M2 cannot give.
* `cachegrind.md`: 300,000 messages through each book under
  `valgrind --tool=cachegrind --cache-sim=yes`. Instruction counts and
  simulated cache misses, independent of machine load. Note that cachegrind
  models no hardware prefetcher, so sequential reads (the day array) count
  as misses that real hardware hides, and the one-time setup (reading the
  7.2 MB day file, zeroing the pool, level arrays and hash table) is included
  in the totals. Per-file attribution follows the debug line info, so code
  inlined from `flat_hash_map.hpp` is charged to that file.

Summary (per message, 300k-message run):

| | v1 | v2 |
|---|---:|---:|
| instructions | 435 | 180 |
| of which in glibc `malloc`/`free` | 36 % | 0 % |
| D1 (L1d) misses, whole run | 1,388,366 | 1,054,138 |
| share of v2's D1 read misses in the id hash map | | 57 % |
