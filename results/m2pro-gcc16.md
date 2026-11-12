## Replay of data/day.bin: 5000000 messages (seed 42)

clock cntvct_el0: 41.667 ns per tick; timestamp-pair overhead p50 0.0 ns, p99 41.7 ns
books sized for 65536 orders
latency columns are nanoseconds and include that overhead
verify: v1 and v2 event streams IDENTICAL (hash 5f2b263ab67effe9 vs 5f2b263ab67effe9)

### v1 (std containers) — 17.0 M msg/s flat out (58.8 ns/msg mean)

| messages | count | mean | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| **all** | 5000000 | 73 | 83 | 83 | 167 | 250 | 4375 | 60917 |
| add (no trade) | 2489566 | 67 | 83 | 83 | 125 | 208 | 3958 | 60917 |
| add (trades) | 279321 | 61 | 42 | 125 | 167 | 292 | 3875 | 32458 |
| cancel | 1905888 | 76 | 83 | 83 | 125 | 292 | 4583 | 14542 |
| modify | 325225 | 120 | 125 | 167 | 208 | 417 | 5250 | 30000 |

### v2 (arrays, pool, flat map) — 90.5 M msg/s flat out (11.0 ns/msg mean)

| messages | count | mean | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| **all** | 5000000 | 32 | 42 | 42 | 83 | 167 | 3292 | 24125 |
| add (no trade) | 2489566 | 29 | 42 | 42 | 83 | 167 | 3292 | 24125 |
| add (trades) | 279321 | 38 | 42 | 42 | 125 | 167 | 3458 | 20792 |
| cancel | 1905888 | 33 | 42 | 42 | 42 | 125 | 2417 | 23583 |
| modify | 325225 | 47 | 42 | 83 | 83 | 208 | 3792 | 11458 |

v2 vs v1: 5.33x throughput, p50 83 -> 42 ns, p99 167 -> 83 ns, p99.9 250 -> 167 ns
