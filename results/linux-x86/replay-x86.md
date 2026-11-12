## Replay of day.bin: 5000000 messages (seed 42)

clock rdtsc: 0.385 ns per tick; timestamp-pair overhead p50 10.0 ns, p99 20.0 ns
books sized for 65536 orders
latency columns are nanoseconds and include that overhead
verify: v1 and v2 event streams IDENTICAL (hash cebf4c705bfef8ee vs cebf4c705bfef8ee)

### v1 (std containers) — 23.4 M msg/s flat out (42.7 ns/msg mean)

| messages | count | mean | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| **all** | 5000000 | 52 | 50 | 70 | 120 | 190 | 1212 | 33991 |
| add (no trade) | 2489579 | 42 | 40 | 60 | 70 | 110 | 381 | 23445 |
| add (trades) | 278385 | 53 | 50 | 90 | 160 | 230 | 441 | 9965 |
| cancel | 1906905 | 58 | 60 | 70 | 80 | 130 | 1282 | 33991 |
| modify | 325131 | 93 | 90 | 120 | 180 | 260 | 5719 | 13200 |

### v2 (arrays, pool, flat map) — 72.7 M msg/s flat out (13.8 ns/msg mean)

| messages | count | mean | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| **all** | 5000000 | 27 | 30 | 40 | 60 | 130 | 260 | 15864 |
| add (no trade) | 2489579 | 24 | 20 | 30 | 40 | 140 | 270 | 15864 |
| add (trades) | 278385 | 33 | 30 | 40 | 60 | 140 | 341 | 11988 |
| cancel | 1906905 | 27 | 30 | 40 | 40 | 60 | 190 | 14632 |
| modify | 325131 | 43 | 40 | 60 | 70 | 120 | 310 | 11477 |

v2 vs v1: 3.10x throughput, p50 50 -> 30 ns, p99 120 -> 60 ns, p99.9 190 -> 130 ns
