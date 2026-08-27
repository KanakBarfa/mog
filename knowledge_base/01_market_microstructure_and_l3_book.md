# Chapter 01: Market Microstructure & the L3 Order Book

**What you will learn:** what an order book is, what "L3" means, how
exchanges decide who trades with whom, and why mog stores prices as
integers.

---

## 1. The big picture: a market is a set of waiting lines

Imagine a farmers-market stall selling apples. Buyers shout offers ("I'll
pay $1.00 for 10"), sellers do the same ("$1.20 for 10"). The stall owner
keeps two boards: one of outstanding buy offers (**bids**) and one of
outstanding sell offers (**asks**). Together these boards are the **order
book**. A trade happens when the best buyer and best seller agree - or,
in an electronic exchange, when a new order arrives that crosses an
existing one.

An electronic stock market runs exactly this, at millions of messages per
second, with strict rules about who gets served first.

## 2. L1 vs L2 vs L3: three zoom levels

Data vendors sell market visibility at three zoom levels:

- **L1**: just the best bid and best ask right now (the "headline").
- **L2**: total shares waiting at each price level (the board, but each
  price shown as one summed number).
- **L3**: every individual order, with a unique ID, visible from birth to
  death - additions, partial cancellations, executions.

L2 shows *that* 5,000 shares wait at $100.00; L3 shows those are 400
separate orders, who arrived first, and exactly how much sits ahead of
any one of them. That matters because exchanges serve orders at one
price strictly in arrival order, so your expected fill depends on your
place in line - which only L3 can tell you.

mog consumes L3 ([OrderBook.hpp](../include/mog/OrderBook.hpp)), which is
what lets it compute queue positions exactly instead of estimating them.

## 3. Price-time priority: the serving rule

Exchanges match orders by two rules, applied in order:

1. **Price priority**: the best price always wins. The highest bid and
   the lowest ask are at the "front of the market".
2. **Time priority**: among orders at the *same* price, whoever arrived
   first is served first. First come, first served - FIFO (first in,
   first out).

So each price level in the book is a FIFO waiting line, and the whole
book is two ladders of such lines (bids descending, asks ascending).

## 4. The life of an order: six events

NASDAQ's feed reports everything an order can experience. Each event is
one message type:

| Event | Code | Plain meaning |
|-------|------|---------------|
| Add | `A` / `F` | A new order joins a waiting line (`F` also names the member firm). |
| Executed | `E` | Part or all of a resting order traded against incoming flow. |
| Executed with price | `C` | Like `E`, but the print happened at an off-display price. |
| Cancel | `X` | The order shrank; part of its quantity left the line. |
| Delete | `D` | The order vanished entirely. |
| Replace | `U` | Cancel + re-add in one move: same owner, possibly new size/price. |

One subtlety worth knowing (and modeled correctly): a replace that raises
size or changes price goes to the **back** of the line - it is effectively
a new arrival. Only shrinking at the same price keeps your seniority.
Types live in [Types.hpp](../include/mog/Types.hpp); handling in
[OrderBook.hpp](../include/mog/OrderBook.hpp).

## 5. Why prices are stored as integers

Floating-point numbers (the `0.1 + 0.2 != 0.30000000000000004` problem)
have no place in money math: two ways of computing the same value can
disagree in the last decimal, and determinism dies.

So mog never stores dollars-and-cents floats. Prices are 64-bit integers
counting **ticks**, where one tick = $0.0001:

$$\text{PriceTicks} = \text{PriceUSD} \times 10{,}000$$

$100.05 becomes `1000500`. All arithmetic is integer arithmetic, with
saturating overflow (values clamp at their max instead of wrapping around
silently). See [Types.hpp](../include/mog/Types.hpp).
