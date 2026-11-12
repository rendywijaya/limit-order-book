## Tick-to-trade: data/capture.itch, symbol AAPL, clock cntvct_el0 (41.667 ns/tick), warm-up 100000 messages

### paced, 2 threads at 1.000000x capture speed

2000011 messages in 50.04 s (0.0 M msg/s); 250842 for the quoted symbol; top changed 70370 times
orders: 636 new, 2622 replace, 0 cancel (2621 messages produced orders); risk refused 0; fills 634 for 63400 shares; final position 0; mark-to-market P&L 39500 ticks x shares; order hash 1b9d8a0e2b46a35b
fills by kind: queue 634, traded-through 0, marketable 0; risk refusals: 

| stage | count | mean | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| feed: decode + book + snapshot | 1900011 | 400 | 167 | 417 | 3500 | 11333 | 106667 | 2222625 |
| queue: feed thread → strategy thread | 238278 | 375 | 125 | 375 | 3333 | 10667 | 58667 | 4370499 |
| decide: quoter | 66656 | 73 | 42 | 125 | 333 | 3333 | 9333 | 41542 |
| risk: pre-trade checks | 3068 | 333 | 83 | 875 | 3333 | 4833 | 6000 | 6042 |
| send: gateway state + OUCH + SoupBin frame | 3068 | 360 | 292 | 542 | 2333 | 3500 | 4667 | 4750 |
| exchange: simulated answer, processed | 3068 | 388 | 250 | 792 | 1667 | 4333 | 14667 | 14667 |
| wire-to-decision | 66656 | 812 | 375 | 750 | 4500 | 18667 | 1024000 | 1979666 |
| **tick-to-trade** | 2466 | 1509 | 1000 | 3500 | 7000 | 15333 | 36000 | 36417 |
| lateness vs schedule (paced) | 1900011 | 179 | 42 | 42 | 458 | 10333 | 106667 | 2221583 |
