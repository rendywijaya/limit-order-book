## Tick-to-trade: data/capture.itch, symbol AAPL, clock cntvct_el0 (41.667 ns/tick), warm-up 100000 messages

### paced, 1 thread at 10.000000x capture speed

2000011 messages in 5.00 s (0.4 M msg/s); 250842 for the quoted symbol; top changed 70370 times
orders: 636 new, 2622 replace, 0 cancel (2621 messages produced orders); risk refused 0; fills 634 for 63400 shares; final position 0; mark-to-market P&L 39500 ticks x shares; order hash 1b9d8a0e2b46a35b
fills by kind: queue 634, traded-through 0, marketable 0; risk refusals: 

| stage | count | mean | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| feed: decode + book + snapshot | 1900011 | 171 | 42 | 292 | 1833 | 10667 | 42667 | 187542 |
| queue: feed thread → strategy thread | 238278 | 42 | 42 | 42 | 42 | 42 | 42 | 42 |
| decide: quoter | 66656 | 44 | 42 | 42 | 83 | 333 | 3833 | 15667 |
| risk: pre-trade checks | 3068 | 136 | 42 | 417 | 667 | 3167 | 5667 | 5750 |
| send: gateway state + OUCH + SoupBin frame | 3068 | 220 | 167 | 417 | 458 | 3333 | 4667 | 4667 |
| exchange: simulated answer, processed | 3068 | 164 | 167 | 208 | 417 | 1417 | 5000 | 5083 |
| wire-to-decision | 66656 | 218 | 125 | 292 | 1833 | 10333 | 38667 | 117500 |
| **tick-to-trade** | 2466 | 591 | 417 | 917 | 2833 | 9000 | 10000 | 10292 |
| lateness vs schedule (paced) | 1900011 | 98 | 42 | 42 | 708 | 10000 | 41333 | 186333 |
