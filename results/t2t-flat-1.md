## Tick-to-trade: data/capture.itch, symbol AAPL, clock cntvct_el0 (41.667 ns/tick), warm-up 100000 messages

### flat out, 1 thread

2000011 messages in 0.16 s (12.2 M msg/s); 250842 for the quoted symbol; top changed 70370 times
orders: 636 new, 2622 replace, 0 cancel (2621 messages produced orders); risk refused 0; fills 634 for 63400 shares; final position 0; mark-to-market P&L 39500 ticks x shares; order hash 1b9d8a0e2b46a35b
fills by kind: queue 634, traded-through 0, marketable 0; risk refusals: 

| stage | count | mean | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| feed: decode + book + snapshot | 1900011 | 54 | 42 | 83 | 125 | 917 | 3500 | 81667 |
| queue: feed thread → strategy thread | 238278 | 42 | 42 | 42 | 42 | 42 | 42 | 42 |
| decide: quoter | 66656 | 44 | 42 | 42 | 83 | 167 | 458 | 13417 |
| risk: pre-trade checks | 3068 | 97 | 42 | 250 | 625 | 875 | 1083 | 1083 |
| send: gateway state + OUCH + SoupBin frame | 3068 | 110 | 42 | 208 | 500 | 792 | 1083 | 1083 |
| exchange: simulated answer, processed | 3068 | 155 | 167 | 208 | 250 | 500 | 1750 | 1750 |
| wire-to-decision | 66656 | 129 | 125 | 167 | 250 | 1083 | 5000 | 26042 |
| **tick-to-trade** | 2466 | 392 | 292 | 625 | 1333 | 4500 | 17333 | 17667 |
