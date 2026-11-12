## Replay of data/day.bin: 5000000 messages (seed 42)

clock cntvct_el0: 41.667 ns per tick; timestamp-pair overhead p50 0.0 ns, p99 41.7 ns
books sized for 65536 orders
latency columns are nanoseconds and include that overhead
verify: v1 and v2 event streams IDENTICAL (hash 5f2b263ab67effe9 vs 5f2b263ab67effe9)

### v1 (std containers) — 16.4 M msg/s flat out (61.0 ns/msg mean)

| messages | count | mean | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| **all** | 5000000 | 76 | 83 | 83 | 167 | 375 | 4958 | 803292 |
| add (no trade) | 2489566 | 73 | 83 | 83 | 125 | 208 | 4208 | 803292 |
| add (trades) | 279321 | 65 | 42 | 125 | 208 | 417 | 4750 | 44458 |
| cancel | 1905888 | 74 | 83 | 83 | 167 | 458 | 5208 | 51833 |
| modify | 325225 | 121 | 125 | 167 | 250 | 667 | 5833 | 28125 |

### v2 (arrays, pool, flat map) — 71.3 M msg/s flat out (14.0 ns/msg mean)

| messages | count | mean | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| **all** | 5000000 | 33 | 42 | 42 | 125 | 167 | 3208 | 77583 |
| add (no trade) | 2489566 | 30 | 42 | 42 | 125 | 167 | 3583 | 57250 |
| add (trades) | 279321 | 41 | 42 | 42 | 125 | 208 | 3458 | 13042 |
| cancel | 1905888 | 34 | 42 | 42 | 83 | 167 | 833 | 44708 |
| modify | 325225 | 46 | 42 | 83 | 125 | 208 | 3833 | 77583 |

v2 vs v1: 4.35x throughput, p50 83 -> 42 ns, p99 167 -> 125 ns, p99.9 375 -> 167 ns
