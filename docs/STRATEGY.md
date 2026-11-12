# The quoter and the risk gate

## The quoter

A market maker keeps a bid and an ask in the book and earns the spread when
both fill. The one here is deliberately small, so its latency can be
reasoned about and every decision can be reproduced from the inputs.

### The decision

Inputs: the top of book (bid, ask, sizes) and the quoter's own position.
Everything is in integer ticks; the mid is kept in half-ticks (`bid + ask`)
so it is exact.

```
fair      = (bid + ask) − 2 × skew × ⌊position / quote_qty⌋        (half-ticks)
bid_px    = ⌊(fair − 2 × half_spread) / 2⌋                           (rounds away from the mid)
ask_px    = ⌈(fair + 2 × half_spread) / 2⌉
post-only : bid_px < best ask, ask_px > best bid, ask_px > bid_px
pull      : if position ≥ +max_position no bid; if ≤ −max_position no ask
requote   : only if |desired − live| ≥ requote_threshold or the size changed
```

Skew is the inventory control: long one lot, both quotes move down a tick,
so the ask is more likely to fill and the bid less. Post-only means the
quoter never takes liquidity (never pays the spread, never crosses an order
it is tracking). The threshold stops it from sending a replace for every
flicker of the mid: each message to the exchange costs a message-rate token
and a risk check, and a replace loses queue priority.

### One request in flight per side

Each side is a state machine:

```
Idle ──New sent──► PendingNew ──Accepted──► Live ──Replace sent──► PendingReplace ──Replaced──► Live
                        │Rejected              │Cancel sent                 │Rejected (original stays)
                        ▼                      ▼                            ▼
                      Idle               PendingCancel ──Canceled──► Idle   Live
```

While a side is pending, the quoter does nothing on that side, whatever the
market does, because the exchange's answer may change what the right action
is. The races and how they resolve (each is a test in `test_quoter.cpp`):

| race | what happens |
|---|---|
| fill arrives while a cancel is pending | position updates immediately; the side stays pending; the exchange's `Canceled` with decrement 0 (nothing was left) returns it to Idle |
| fill arrives while a replace is pending | position updates; `Replaced` reports what rests after the fill; if nothing, Idle |
| replace rejected | the original is still live at its old price; the side returns to Live |
| new order rejected | Idle |
| risk refuses an intent before it is sent | the side returns to what it was |
| market disappears (one side empty) | pull both quotes |

The quoter never reads a clock and never allocates. Its stats count
decisions, news, replaces, cancels, fills, rejects and pulls.

## The risk gate

Every order and every replace passes eight checks in a fixed order, in
integers, against limits fixed before the session; the first failure is the
reason reported. The order matters: a reviewer or an auditor can predict
which reason a bad order gets.

| # | check | refuses when |
|---|---|---|
| 1 | kill switch | the switch is thrown (everything except cancels) |
| 2 | reference price | there is no two-sided market to check the price against |
| 3 | order size | `qty == 0` or `qty > max_order_qty` |
| 4 | price collar | `|price − mid| > collar_ticks` (computed in half-ticks, exact) |
| 5 | notional | `price × qty > max_notional` |
| 6 | open orders | a new order when `open_orders ≥ max_open_orders` (a replace does not add one) |
| 7 | worst-case position | buy: `position + open_buy_qty + qty > max_position`; sell: the mirror; a replace releases its own open quantity first |
| 8 | message rate | more than `max_msgs_per_sec` messages in the current one-second window |

**Cancels always pass**, including under the kill switch and over the message
rate (they are counted, never refused): a cancel can only reduce exposure,
and the moment you most need one out is the moment something else is wrong.

**Worst case** means: assume every open order on that side fills this
instant. A strategy that has four bids of 100 resting and a position of 100
is treated as long 500 for the purpose of placing another bid. This is what
stops a feed glitch or a runaway loop from accumulating a position the limits
never contemplated.

The `Exposure` the gate reads is maintained by the gateway from the
exchange's answers, not by the strategy: position from `Executed`, open
quantities from `Accepted` / `Replaced` / `Canceled` / `Executed`, and the
open-order count. Risk never trusts the quoter's own idea of what is out
there. The gate costs tens of nanoseconds (`risk` row in the measurements: p50
one 41.67 ns tick on the M2; the mean of ~100 ns includes the first-time cold
runs after each quiet period).

## What a real desk would add

Per-symbol and per-strategy limits, a global notional and P&L limit, a
heartbeat-based auto-kill if the feed or the gateway goes quiet, self-trade
prevention across strategies, fat-finger checks against the day's price
range, and a second, independent implementation of the gate on the gateway
side so that no single bug can disable it. None of these changes the shape
of what is here; each is another check in the sequence, or another counter
in `Exposure`.
