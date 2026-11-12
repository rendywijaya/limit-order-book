## Replay of small.bin: 300000 messages (seed 42)

clock rdtsc: 0.385 ns per tick; timestamp-pair overhead p50 30.1 ns, p99 30.1 ns
books sized for 65536 orders
latency columns are nanoseconds and include that overhead

### v1 (std containers) — 0.9 M msg/s flat out (1149.0 ns/msg mean)

| messages | count | mean | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
=== v1 summary
==2697== Cachegrind, a high-precision tracing profiler
==2697== Copyright (C) 2002-2017, and GNU GPL'd, by Nicholas Nethercote et al.
==2697== Using Valgrind-3.22.0 and LibVEX; rerun with -h for copyright info
==2697== Command: ./build/replay --day small.bin --book v1 --runs 1 --skip-latency
==2697== 
--2697-- warning: L3 cache found, using its data for the LL simulation.
--2697-- warning: specified LL cache: line_size 64  assoc 1  total_size 402,653,184
--2697-- warning: simulated LL cache: line_size 64  assoc 2  total_size 536,870,912
==2697== 
==2697== I refs:        130,496,179
==2697== I1  misses:          2,836
==2697== LLi misses:          2,553
==2697== I1  miss rate:        0.00%
==2697== LLi miss rate:        0.00%
==2697== 
==2697== D refs:         57,774,327  (37,986,170 rd   + 19,788,157 wr)
==2697== D1  misses:      1,388,366  (   946,558 rd   +    441,808 wr)
==2697== LLd misses:        133,224  (     7,736 rd   +    125,488 wr)
==2697== D1  miss rate:         2.4% (       2.5%     +        2.2%  )
==2697== LLd miss rate:         0.2% (       0.0%     +        0.6%  )
==2697== 
==2697== LL refs:         1,391,202  (   949,394 rd   +    441,808 wr)
==2697== LL misses:         135,777  (    10,289 rd   +    125,488 wr)
==2697== LL miss rate:          0.1% (       0.0%     +        0.6%  )
=== v1 per function
--------------------------------------------------------------------------------
-- Metadata
--------------------------------------------------------------------------------
Invocation:       /usr/bin/cg_annotate cg.v1
I1 cache:         32768 B, 64 B, 8-way associative
D1 cache:         49152 B, 64 B, 12-way associative
LL cache:         536870912 B, 64 B, 2-way associative
Command:          ./build/replay --day small.bin --book v1 --runs 1 --skip-latency
Events recorded:  Ir I1mr ILmr Dr D1mr DLmr Dw D1mw DLmw
Events shown:     Ir I1mr ILmr Dr D1mr DLmr Dw D1mw DLmw
Event sort order: Ir I1mr ILmr Dr D1mr DLmr Dw D1mw DLmw
Threshold:        0.1%
Annotation:       on

--------------------------------------------------------------------------------
-- Summary
--------------------------------------------------------------------------------
Ir__________________ I1mr__________ ILmr__________ Dr_________________ D1mr____________ DLmr__________ Dw_________________ D1mw____________ DLmw____________ 

130,496,179 (100.0%) 2,836 (100.0%) 2,553 (100.0%) 37,986,170 (100.0%) 946,558 (100.0%) 7,736 (100.0%) 19,788,157 (100.0%) 441,808 (100.0%) 125,488 (100.0%)  PROGRAM TOTALS

--------------------------------------------------------------------------------
-- File:function summary
--------------------------------------------------------------------------------
  Ir_______________________ I1mr______________ ILmr______________ Dr_______________________ D1mr__________________ DLmr________________ Dw______________________ D1mw__________________ DLmw__________________  file:function

< 47,123,910 (36.1%, 36.1%) 103  (3.6%,  3.6%)  83  (3.3%,  3.3%) 13,213,838 (34.8%, 34.8%) 103,970 (11.0%, 11.0%)     3  (0.0%,  0.0%) 7,221,412 (36.5%, 36.5%)   3,205  (0.7%,  0.7%)   3,177  (2.5%,  2.5%)  ./malloc/./malloc/malloc.c:
  19,770,522 (15.2%)         12  (0.4%)          8  (0.3%)         5,268,622 (13.9%)         28,532  (3.0%)            1  (0.0%)        2,428,481 (12.3%)              3  (0.0%)              0                   _int_free
  14,345,173 (11.0%)          7  (0.2%)          7  (0.3%)         3,825,381 (10.1%)            373  (0.0%)            1  (0.0%)        2,193,705 (11.1%)              3  (0.0%)              0                   malloc
   8,925,856  (6.8%)          6  (0.2%)          5  (0.2%)         3,187,800  (8.4%)         61,416  (6.5%)            0                1,912,684  (9.7%)              3  (0.0%)              0                   free
   4,064,618  (3.1%)         55  (1.9%)         42  (1.6%)           923,616  (2.4%)         13,642  (1.4%)            1  (0.0%)          686,375  (3.5%)          3,160  (0.7%)          3,142  (2.5%)           _int_malloc

< 21,547,422 (16.5%, 52.6%)  28  (1.0%,  4.6%)  28  (1.1%,  4.3%)  7,120,671 (18.7%, 53.5%) 230,516 (24.4%, 35.3%)     0  (0.0%,  0.0%) 3,486,511 (17.6%, 54.1%)       0  (0.0%,  0.7%)       0  (0.0%,  2.5%)  /home/runner/work/limit-order-book/limit-order-book/include/lob/book_v1.hpp:
  10,003,250  (7.7%)         12  (0.4%)         12  (0.5%)         2,686,057  (7.1%)              2  (0.0%)            0                1,032,354  (5.2%)              0                      0                   lob::BookV1<lob::CountingListener>::add(unsigned long, lob::Side, lob::OrderType, int, unsigned int)
   3,978,425  (3.0%)          2  (0.1%)          2  (0.1%)         1,113,959  (2.9%)              0                    0                1,273,096  (6.4%)              0                      0                   lob::BookV1<lob::CountingListener>::rest(unsigned long, lob::Side, int, unsigned int)
   3,549,932  (2.7%)          3  (0.1%)          3  (0.1%)         1,604,371  (4.2%)         98,481 (10.4%)            0                  298,970  (1.5%)              0                      0                   main
   2,752,659  (2.1%)          1  (0.0%)          1  (0.0%)         1,310,790  (3.5%)        127,020 (13.4%)            0                  655,395  (3.3%)              0                      0                   lob::BookV1<lob::CountingListener>::unlink(lob::BookV1<lob::CountingListener>::Loc const&)
   1,263,156  (1.0%)         10  (0.4%)         10  (0.4%)           405,494  (1.1%)          5,013  (0.5%)            0                  226,696  (1.1%)              0                      0                   lob::BookV1<lob::CountingListener>::modify(unsigned long, int, unsigned int)

< 15,512,218 (11.9%, 64.5%) 639 (22.5%, 27.2%) 599 (23.5%, 27.8%)  4,315,352 (11.4%, 64.9%)  27,916  (2.9%, 38.3%)    31  (0.4%,  0.4%) 1,918,432  (9.7%, 63.8%) 247,574 (56.0%, 56.8%)     131  (0.1%,  2.6%)  ???:
   5,100,448  (3.9%)          2  (0.1%)          2  (0.1%)           956,334  (2.5%)              0                    0                  956,334  (4.8%)              1  (0.0%)              0                   operator new(unsigned long)
   4,144,857  (3.2%)        284 (10.0%)        245  (9.6%)         2,072,175  (5.5%)            533  (0.1%)           10  (0.1%)              439  (0.0%)             21  (0.0%)             13  (0.0%)           ???
   2,546,192  (2.0%)          2  (0.1%)          2  (0.1%)           477,411  (1.3%)              0                    0                  159,137  (0.8%)              0                      0                   std::__detail::_Prime_rehash_policy::_M_need_rehash(unsigned long, unsigned long, unsigned long) const
   1,432,233  (1.1%)          1  (0.0%)          1  (0.0%)           318,274  (0.8%)          1,635  (0.2%)            0                  477,411  (2.4%)         13,270  (3.0%)              0                   std::__detail::_List_node_base::_M_hook(std::__detail::_List_node_base*)
     942,690  (0.7%)          1  (0.0%)          1  (0.0%)           471,345  (1.2%)         24,847  (2.6%)            0                  314,230  (1.6%)        233,825 (52.9%)              0                   std::__detail::_List_node_base::_M_unhook()
## Replay of small.bin: 300000 messages (seed 42)

clock rdtsc: 0.385 ns per tick; timestamp-pair overhead p50 30.1 ns, p99 30.1 ns
books sized for 65536 orders
latency columns are nanoseconds and include that overhead

### v2 (arrays, pool, flat map) — 2.5 M msg/s flat out (395.9 ns/msg mean)

| messages | count | mean | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
=== v2 summary
==2842== Cachegrind, a high-precision tracing profiler
==2842== Copyright (C) 2002-2017, and GNU GPL'd, by Nicholas Nethercote et al.
==2842== Using Valgrind-3.22.0 and LibVEX; rerun with -h for copyright info
==2842== Command: ./build/replay --day small.bin --book v2 --runs 1 --skip-latency
==2842== 
--2842-- warning: L3 cache found, using its data for the LL simulation.
--2842-- warning: specified LL cache: line_size 64  assoc 1  total_size 402,653,184
--2842-- warning: simulated LL cache: line_size 64  assoc 2  total_size 536,870,912
==2842== 
==2842== I refs:        54,101,702
==2842== I1  misses:         2,730
==2842== LLi misses:         2,510
==2842== I1  miss rate:       0.01%
==2842== LLi miss rate:       0.00%
==2842== 
==2842== D refs:        24,372,465  (14,899,407 rd   + 9,473,058 wr)
==2842== D1  misses:     1,054,138  (   628,493 rd   +   425,645 wr)
==2842== LLd misses:       236,609  (     7,731 rd   +   228,878 wr)
==2842== D1  miss rate:        4.3% (       4.2%     +       4.5%  )
==2842== LLd miss rate:        1.0% (       0.1%     +       2.4%  )
==2842== 
==2842== LL refs:        1,056,868  (   631,223 rd   +   425,645 wr)
==2842== LL misses:        239,119  (    10,241 rd   +   228,878 wr)
==2842== LL miss rate:         0.3% (       0.0%     +       2.4%  )
=== v2 per function
--------------------------------------------------------------------------------
-- Metadata
--------------------------------------------------------------------------------
Invocation:       /usr/bin/cg_annotate cg.v2
I1 cache:         32768 B, 64 B, 8-way associative
D1 cache:         49152 B, 64 B, 12-way associative
LL cache:         536870912 B, 64 B, 2-way associative
Command:          ./build/replay --day small.bin --book v2 --runs 1 --skip-latency
Events recorded:  Ir I1mr ILmr Dr D1mr DLmr Dw D1mw DLmw
Events shown:     Ir I1mr ILmr Dr D1mr DLmr Dw D1mw DLmw
Event sort order: Ir I1mr ILmr Dr D1mr DLmr Dw D1mw DLmw
Threshold:        0.1%
Annotation:       on

--------------------------------------------------------------------------------
-- Summary
--------------------------------------------------------------------------------
Ir_________________ I1mr__________ ILmr__________ Dr_________________ D1mr____________ DLmr__________ Dw________________ D1mw____________ DLmw____________ 

54,101,702 (100.0%) 2,730 (100.0%) 2,510 (100.0%) 14,899,407 (100.0%) 628,493 (100.0%) 7,731 (100.0%) 9,473,058 (100.0%) 425,645 (100.0%) 228,878 (100.0%)  PROGRAM TOTALS

--------------------------------------------------------------------------------
-- File:function summary
--------------------------------------------------------------------------------
  Ir_______________________ I1mr_____________ ILmr____________ Dr______________________ D1mr__________________ DLmr________________ Dw______________________ D1mw__________________ DLmw__________________  file:function

< 33,709,600 (62.3%, 62.3%)  42 (1.5%,  1.5%) 42 (1.7%,  1.7%) 9,931,221 (66.7%, 66.7%) 247,947 (39.5%, 39.5%)     0  (0.0%,  0.0%) 7,089,942 (74.8%, 74.8%) 194,801 (45.8%, 45.8%)       0  (0.0%,  0.0%)  /home/runner/work/limit-order-book/limit-order-book/include/lob/book_v2.hpp:
   9,652,680 (17.8%)          8 (0.3%)         8 (0.3%)        1,852,076 (12.4%)              1  (0.0%)            0                1,670,282 (17.6%)              0                      0                   lob::BookV2<lob::CountingListener>::add(unsigned long, lob::Side, lob::OrderType, int, unsigned int)
   8,435,692 (15.6%)          5 (0.2%)         5 (0.2%)        2,226,860 (14.9%)          5,866  (0.9%)            0                2,225,867 (23.5%)          8,243  (1.9%)              0                   lob::BookV2<lob::CountingListener>::rest(unsigned long, lob::Side, int, unsigned int)
   6,924,715 (12.8%)          8 (0.3%)         8 (0.3%)        3,065,668 (20.6%)        223,152 (35.5%)            0                1,088,236 (11.5%)        144,809 (34.0%)              0                   main
   3,562,203  (6.6%)          4 (0.1%)         4 (0.2%)        1,167,452  (7.8%)             15  (0.0%)            0                  828,059  (8.7%)          9,098  (2.1%)              0                   unsigned int lob::BookV2<lob::CountingListener>::match<(lob::Side)0>(unsigned long, int, bool, unsigned int) [clone .isra.0]
   3,549,549  (6.6%)          5 (0.2%)         5 (0.2%)        1,163,384  (7.8%)             17  (0.0%)            0                  825,263  (8.7%)          9,040  (2.1%)              0                   unsigned int lob::BookV2<lob::CountingListener>::match<(lob::Side)1>(unsigned long, int, bool, unsigned int) [clone .isra.0]
   1,453,646  (2.7%)          9 (0.3%)         9 (0.4%)          455,771  (3.1%)         18,892  (3.0%)            0                  321,148  (3.4%)         23,610  (5.5%)              0                   lob::BookV2<lob::CountingListener>::modify(unsigned long, int, unsigned int)
     131,115  (0.2%)          3 (0.1%)         3 (0.1%)               10  (0.0%)              4  (0.0%)            0                  131,087  (1.4%)              1  (0.0%)              0                   lob::BookV2<lob::CountingListener>::BookV2(lob::CountingListener&, lob::BookConfig)

<  7,321,503 (13.5%, 75.8%)  20 (0.7%,  2.3%) 20 (0.8%,  2.5%) 1,889,826 (12.7%, 79.3%) 357,565 (56.9%, 96.3%)     0  (0.0%,  0.0%)   660,575  (7.0%, 81.8%)       3  (0.0%, 45.8%)       0  (0.0%,  0.0%)  /home/runner/work/limit-order-book/limit-order-book/include/lob/flat_hash_map.hpp:
   2,825,819  (5.2%)          4 (0.1%)         4 (0.2%)          790,984  (5.3%)        135,340 (21.5%)            0                  112,697  (1.2%)              0                      0                   main
   2,228,416  (4.1%)          2 (0.1%)         2 (0.1%)          477,411  (3.2%)              0                    0                  477,411  (5.0%)              0                      0                   lob::BookV2<lob::CountingListener>::rest(unsigned long, lob::Side, int, unsigned int)
   1,001,682  (1.9%)          2 (0.1%)         2 (0.1%)          333,894  (2.2%)        166,943 (26.6%)            0                        0                      0                      0                   lob::BookV2<lob::CountingListener>::add(unsigned long, lob::Side, lob::OrderType, int, unsigned int)
     507,424  (0.9%)          3 (0.1%)         3 (0.1%)          131,311  (0.9%)         22,747  (3.6%)            0                   18,382  (0.2%)              0                      0                   lob::BookV2<lob::CountingListener>::modify(unsigned long, int, unsigned int)
     379,230  (0.7%)          4 (0.1%)         4 (0.2%)           78,108  (0.5%)         16,268  (2.6%)            0                   26,036  (0.3%)              0                      0                   unsigned int lob::BookV2<lob::CountingListener>::match<(lob::Side)0>(unsigned long, int, bool, unsigned int) [clone .isra.0]
     378,891  (0.7%)          3 (0.1%)         3 (0.1%)           78,108  (0.5%)         16,264  (2.6%)            0                   26,036  (0.3%)              1  (0.0%)              0                   unsigned int lob::BookV2<lob::CountingListener>::match<(lob::Side)1>(unsigned long, int, bool, unsigned int) [clone .isra.0]

<  5,578,272 (10.3%, 86.2%)  32 (1.2%,  3.4%) 31 (1.2%,  3.7%) 1,095,212  (7.4%, 86.7%)     356  (0.1%, 96.4%)     0  (0.0%,  0.0%)   263,747  (2.8%, 84.6%)      10  (0.0%, 45.8%)       0  (0.0%,  0.0%)  /usr/include/c++/14/bits/stl_vector.h:
   2,076,503  (3.8%)          0                0                 477,931  (3.2%)            181  (0.0%)            0                        0                      0                      0                   lob::BookV2<lob::CountingListener>::rest(unsigned long, lob::Side, int, unsigned int)
