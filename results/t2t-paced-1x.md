## Tick-to-trade: data/capture.itch, symbol AAPL, clock cntvct_el0 (41.667 ns/tick), warm-up 100000 messages

### paced, 1 thread at 1.000000x capture speed

2000011 messages in 50.04 s (0.0 M msg/s); 250842 for the quoted symbol; top changed 70370 times
orders: 636 new, 2622 replace, 0 cancel (2621 messages produced orders); risk refused 0; fills 634 for 63400 shares; final position 0; mark-to-market P&L 39500 ticks x shares; order hash 1b9d8a0e2b46a35b
fills by kind: queue 634, traded-through 0, marketable 0; risk refusals: 

| stage | count | mean | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| feed: decode + book + snapshot | 1900011 | 298 | 167 | 417 | 3333 | 10000 | 33333 | 1004708 |
| queue: feed thread → strategy thread | 238278 | 42 | 42 | 42 | 42 | 42 | 42 | 42 |
| decide: quoter | 66656 | 51 | 42 | 42 | 167 | 1583 | 4667 | 15250 |
| risk: pre-trade checks | 3068 | 285 | 83 | 792 | 3000 | 4500 | 4667 | 4750 |
| send: gateway state + OUCH + SoupBin frame | 3068 | 347 | 292 | 458 | 2667 | 3333 | 9333 | 9583 |
| exchange: simulated answer, processed | 3068 | 316 | 208 | 542 | 1500 | 3333 | 5667 | 5750 |
| wire-to-decision | 66656 | 335 | 167 | 458 | 3500 | 10333 | 30667 | 209417 |
| **tick-to-trade** | 2466 | 1107 | 708 | 2083 | 5667 | 14000 | 53333 | 54542 |
| lateness vs schedule (paced) | 1900011 | 103 | 42 | 42 | 417 | 9333 | 32000 | 1003292 |
