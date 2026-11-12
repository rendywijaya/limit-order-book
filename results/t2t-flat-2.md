## Tick-to-trade: data/capture.itch, symbol AAPL, clock cntvct_el0 (41.667 ns/tick), warm-up 100000 messages

### flat out, 2 threads

2000011 messages in 0.16 s (12.2 M msg/s); 250842 for the quoted symbol; top changed 70370 times
orders: 636 new, 2622 replace, 0 cancel (2621 messages produced orders); risk refused 0; fills 634 for 63400 shares; final position 0; mark-to-market P&L 39500 ticks x shares; order hash 1b9d8a0e2b46a35b
fills by kind: queue 634, traded-through 0, marketable 0; risk refusals: 

| stage | count | mean | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| feed: decode + book + snapshot | 1900011 | 53 | 42 | 83 | 125 | 167 | 833 | 44208 |
| queue: feed thread → strategy thread | 238278 | 155 | 83 | 167 | 500 | 10333 | 20000 | 103417 |
| decide: quoter | 66656 | 55 | 42 | 83 | 83 | 167 | 3333 | 11250 |
| risk: pre-trade checks | 3068 | 93 | 42 | 292 | 458 | 625 | 958 | 958 |
| send: gateway state + OUCH + SoupBin frame | 3068 | 89 | 83 | 167 | 208 | 417 | 3167 | 3292 |
| exchange: simulated answer, processed | 3068 | 160 | 167 | 208 | 250 | 583 | 1167 | 1167 |
| wire-to-decision | 66656 | 299 | 250 | 333 | 708 | 10333 | 24000 | 103667 |
| **tick-to-trade** | 2466 | 509 | 458 | 708 | 1250 | 7333 | 16667 | 17208 |
