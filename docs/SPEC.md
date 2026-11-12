# Specification

What the engine must do, exactly. Every rule below has a test in
[`tests/test_spec.cpp`](../tests/test_spec.cpp), and the same test runs against
all three books (v1, v2 and the reference).

## The book

One instrument. Two sides, **bids** (buy orders) and **asks** (sell orders).
Prices are integer **ticks** inside a fixed window `[min_price, max_price]`
(inclusive). Quantities are positive integers.

A resting order sits at a **price level**. The book is never crossed: after
every message, best bid < best ask (or a side is empty).

## Priority: price, then time

An incoming order (the **taker**) trades against resting orders (the
**makers**) on the opposite side:

1. **Better price first.** A buy taker hits the *lowest* ask first; a sell
   taker hits the *highest* bid first.
2. **Earlier first at one price.** At a given level, orders trade in the order
   they arrived (FIFO).
3. **Trades print at the maker's price.** A buy limit at 105 that meets an ask
   at 100 trades at 100. The taker gets the price improvement.

A taker trades while it has quantity left and the best opposite level
**crosses** its limit (`ask <= limit` for a buy, `bid >= limit` for a sell).

## Order types

| Type | Matches | Remainder |
|---|---|---|
| `Limit` | levels that cross its limit | **rests** at its limit price (good-till-cancel) |
| `Market` | any level, price ignored | cancelled, reason `Unfilled`; never rests |
| `IOC` | levels that cross its limit | cancelled, reason `Unfilled` |
| `FOK` | only if the *whole* quantity is available at crossing levels | if not, nothing trades: cancelled, reason `FokNotFilled` |

A fully filled order emits no cancel.

## Messages and validation

Validation happens in this order; the first failure rejects the message and
nothing else happens.

**New order** `add(id, side, type, price, qty)`
1. `id` is a live order → `Reject(DuplicateId)`. (An id whose order was filled
   or cancelled may be reused.)
2. `qty == 0` → `Reject(InvalidQty)`
3. not `Market` and price outside the window → `Reject(InvalidPrice)`

**Cancel** `cancel(id)`
1. `id` not live → `Reject(UnknownOrder)`; otherwise `Cancel(id, remaining, User)`.

**Modify** `modify(id, new_price, new_qty)`
1. `id` not live → `Reject(UnknownOrder)`
2. `new_qty == 0` → `Reject(InvalidQty)`
3. price outside the window → `Reject(InvalidPrice)`
4. **Same price and `new_qty <= remaining`**: the size is reduced in place and
   the order **keeps its time priority**. Emits `Modify`.
5. **Otherwise** (price changed, or size increased): the order **loses its
   priority**. It is removed, `Modify` is emitted, and it re-enters as a new
   `Limit` order with the same id and side — which may trade on the way in.
   Its remainder rests at the back of its new level.

Rule 4 vs 5 is how real venues behave: reducing size cannot hurt anyone queued
behind you, but increasing it would let you jump the queue with the extra.

## Events, and their order

Every listener receives, in this exact order:

```
add      → Reject                                   (invalid)
         → Accept, Trade*, [Cancel(Unfilled)]        (Limit / Market / IOC)
         → Accept, Cancel(FokNotFilled)              (FOK that cannot fill)
         → Accept, Trade+                            (FOK that fills)
cancel   → Cancel(User) | Reject(UnknownOrder)
modify   → Modify | Modify, Trade* | Reject
```

`Trade{taker, maker, price, qty, maker_remaining}` reports the maker's
quantity left after the fill, so a downstream consumer can maintain its own
copy of the book from events alone.

## Not in scope (yet)

Self-trade prevention, multiple instruments, auctions, hidden or iceberg
orders, stop orders, and order expiry other than cancel. Each is a small
addition to this spec; none changes the data structures.
