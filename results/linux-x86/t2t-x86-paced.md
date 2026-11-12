## Tick-to-trade: capture.itch, symbol AAPL, clock rdtsc (0.385 ns/tick), warm-up 100000 messages

### paced, 1 thread at 10.000000x capture speed

2000011 messages in 5.00 s (0.4 M msg/s); 250299 for the quoted symbol; top changed 69783 times
orders: 600 new, 2634 replace, 0 cancel (2634 messages produced orders); risk refused 0; fills 598 for 59800 shares; final position 0; mark-to-market P&L 36200 ticks x shares; order hash 1da028249bee6a35
fills by kind: queue 598, traded-through 0, marketable 0; risk refusals: 

| stage | count | mean | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| feed: decode + book + snapshot | 1900011 | 202 | 185 | 247 | 518 | 6311 | 14200 | 67545 |
| queue: feed thread → strategy thread | 237719 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| decide: quoter | 66167 | 54 | 49 | 59 | 148 | 641 | 1676 | 23865 |
| risk: pre-trade checks | 3064 | 201 | 40 | 666 | 1331 | 1676 | 1775 | 1813 |
| send: gateway state + OUCH + SoupBin frame | 3064 | 274 | 234 | 382 | 567 | 690 | 838 | 851 |
| exchange: simulated answer, processed | 3064 | 344 | 259 | 518 | 986 | 1676 | 6311 | 6600 |
| wire-to-decision | 66167 | 278 | 259 | 320 | 715 | 6705 | 20510 | 53424 |
| **tick-to-trade** | 2496 | 845 | 592 | 1528 | 2958 | 8283 | 11044 | 11393 |
| lateness vs schedule (paced) | 1900011 | 49 | 10 | 19 | 308 | 6311 | 14200 | 67335 |
