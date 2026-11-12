# Protocols, as implemented

Three Nasdaq protocols, decoded and encoded byte for byte from their public
specifications. Everything is big-endian; alphanumeric fields are
left-justified and space-padded; prices carry four implied decimals
(1234500 = $123.45); timestamps are nanoseconds since midnight. The tests in
`tests/test_itch.cpp`, `test_ouch.cpp` and `test_soupbin.cpp` build messages
by hand from the field tables below, so the decoders are checked against the
specification and not against the encoders that share their assumptions.

## TotalView-ITCH 5.0 (market data)

Every message: `type (1) | stock locate (2) | tracking number (2) | timestamp (6)`
then the fields. Lengths for every type in the specification are in
`itch::length_of`, so a message the book does not need is skipped by its
correct length. Fields decoded:

| type | name | length | fields decoded (offset) |
|---|---|---:|---|
| `S` | System Event | 12 | event code (11): `O` start, `S` system hours, `Q` market open, `M` market close, `E` end |
| `R` | Stock Directory | 39 | stock (11), round lot size (21) |
| `H` | Stock Trading Action | 25 | stock (11), trading state (19) |
| `A` | Add Order | 36 | order ref (11), side (19), shares (20), stock (24), price (32) |
| `F` | Add Order with MPID | 40 | as `A`, attribution (36) |
| `E` | Order Executed | 31 | order ref (11), executed shares (19), match number (23) |
| `C` | Order Executed with Price | 36 | as `E`, printable (31), execution price (32) |
| `X` | Order Cancel | 23 | order ref (11), cancelled shares (19) |
| `D` | Order Delete | 19 | order ref (11) |
| `U` | Order Replace | 35 | original ref (11), new ref (19), shares (27), price (31) |
| `P` | Trade (non-cross) | 44 | order ref (11), side (19), shares (20), stock (24), price (32), match (36) |
| `Q` | Cross Trade | 40 | shares (11, 8 bytes), stock (19), cross price (27), match (31), cross type (39) |
| `B` | Broken Trade | 19 | match number (11) |

How the feed handler applies them: `A`/`F` insert into the order table and
add to the level; `E`/`C` reduce (and remove at zero); `X` reduces; `D`
removes; `U` removes the original and inserts the new reference at the new
price and size (priority is lost, which the book does not model since it
aggregates by level); `P`/`Q` change nothing in the book and are reported as
prints. Prices are converted to ticks of one cent; a sub-penny price (not a
multiple of 100) is counted and the message ignored, because the book is
indexed by cent.

**Message-stream format.** The capture files are `[2-byte length][message]`
records, the format of Nasdaq's public sample files (`*.NASDAQ_ITCH50`).
`StreamReader` walks them; `gen_itch` writes them.

## OUCH 4.2 (order entry)

### Inbound (to the exchange)

| type | name | length | layout |
|---|---|---:|---|
| `O` | Enter Order | 49 | token (1, 14 bytes) · side (15) · shares (16) · stock (20) · price (28) · time in force (32) · firm (36) · display (40) · capacity (41) · intermarket sweep (42) · min qty (43) · cross type (47) · customer type (48) |
| `U` | Replace Order | 47 | existing token (1) · replacement token (15) · shares (29) · price (33) · time in force (37) · display (41) · intermarket sweep (42) · min qty (43) |
| `X` | Cancel Order | 19 | token (1) · shares (15): the new intended total, 0 = cancel all |

The gateway sends `side` `B`/`S`, time in force 99998 (day), display `Y`,
capacity `P` (principal), no sweep, no minimum, cross type `N`. Tokens are
14 ASCII digits from a counter, unique for the session.

### Outbound (from the exchange)

| type | name | length | fields decoded (offset) |
|---|---|---:|---|
| `S` | System Event | 10 | timestamp (1), event code (9) |
| `A` | Accepted | 66 | token (9), side (23), shares (24), stock (28), price (36), tif (40), order reference (49), order state (64): `L` live / `D` dead |
| `U` | Replaced | 80 | as `A` for the replacement token, previous token (65) |
| `C` | Canceled | 28 | token (9), decrement shares (23), reason (27) |
| `E` | Executed | 40 | token (9), executed shares (23), execution price (27), liquidity flag (31), match number (32) |
| `J` | Rejected | 24 | token (9), reason (23) |
| `I` | Cancel Reject | 23 | token (9) |

Replace semantics the gateway and the simulator both implement: the
replacement's `shares` is the new **total**; the exchange rests
`shares − already executed on the original`; the original dies when the
replacement is acknowledged; a rejected replacement leaves the original
live; a replace at the same price for a smaller size keeps queue priority,
any other replace loses it.

## SoupBinTCP 3.0 (session)

Packet: `length (2) | type (1) | payload`, where length counts the type byte
and the payload. Types:

| direction | type | name | payload |
|---|---|---|---|
| client → server | `L` | Login Request | username (6), password (10), requested session (10), requested sequence number (20, ASCII, right-justified) |
| | `U` | Unsequenced Data | an OUCH inbound message |
| | `R` | Client Heartbeat | none |
| | `O` | Logout Request | none |
| server → client | `A` | Login Accepted | session (10), sequence number (20) of the next Sequenced Data packet |
| | `J` | Login Rejected | reason (1): `A` not authorized, `S` session not available |
| | `S` | Sequenced Data | an OUCH outbound message; implicitly numbered |
| | `H` | Server Heartbeat | none |
| | `Z` | End of Session | none |
| either | `+` | Debug | text |

`soup::Framer` re-assembles packets from any sequence of byte chunks (the
test splits a stream at every size from 1 byte up). `soup::ClientSession`
tracks login state and the next expected sequence number, which is what a
reconnecting client sends in its Login Request so the server can replay what
was missed; it sends a heartbeat after one quiet second and declares the
server gone after fifteen. Time is passed in, never read, so sessions are
deterministic in tests and replays.

## Not implemented here

The packet layer under ITCH on the real wire (sequenced UDP datagrams carrying
message blocks, sent on two multicast groups, with retransmission requests
for gaps) is not in this version; the feed enters as a message stream. It is
the first exercise in [LEARNING.md](LEARNING.md). Order types beyond day
limits, OUCH's modify and mass-cancel messages, and the ITCH imbalance and
auction messages are decoded for length only.
