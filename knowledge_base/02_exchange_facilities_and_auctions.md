# Chapter 02: Order Types, Self-Trade Prevention & Auctions

**What you will learn:** the special instructions a trader can attach to
an order, how an exchange stops you from trading with yourself, and what
those mysterious opening/closing "auction crosses" are.

---

## 1. The big picture: orders carry conditions

"Buy 100 shares at $100.05" is the plainest order. Real venues support
orders with fine print, and each kind of fine print exists to solve one
practical problem. Implemented in
[Simulate.hpp](../include/mog/Simulate.hpp).

### Day Limit
The default. Rest on the book at your price until it trades or you cancel
it ("day" = it expires when the session ends).

### Market
"Buy now, price be damned." Crosses the spread immediately, consuming as
much resting volume as needed. Guaranteed fill, unguaranteed price.

### Immediate-Or-Cancel (IOC)
"Fill whatever is available this instant; tear up the rest." Never rests.
Useful when getting *some* size now beats waiting for all of it.

### Post-Only
"Add liquidity or do nothing." If this order would immediately trade
against a resting order (i.e., cross the spread), the exchange **rejects**
it instead (`rejected_would_cross`). Liquidity providers earn rebates on
resting orders (chapter 08), so this type guarantees you stay on the
rebate side of every trade.

### Iceberg
Show only the tip of a large order. You want to buy 100,000 shares, but
displaying that much would move the market - so you display 1,000 and
keep 99,000 hidden. Each time the visible peak is eaten, another slice
reloads from the reserve, joining the back of the FIFO line (a fresh
arrival loses time priority). Hence "iceberg": mostly underwater.

### Pegged
Not displayed at all; its effective price shadows a moving reference -
the midpoint, best bid, or best ask. When the reference moves, the order
re-pegs automatically while keeping its relative seniority. Think of it
as cruising in another car's slipstream.

### Odd-lot facility
Orders under the standard 100-share round lot. They trade but play by
different display rules; NASDAQ flags them with the `nd` indicator, which
mog models.

## 2. Self-Trade Prevention (STP)

One participant can have several strategies running at once. Without
protection, strategy A's buy could execute against strategy B's sell -
your own desk trading with itself, paying fees for the privilege.

Venues offer three ways to resolve an incoming match against your own
resting order ([Simulate.hpp](../include/mog/Simulate.hpp)):

| Mode | What happens |
|------|--------------|
| `cancel_newest` | The incoming order is cancelled; the resting survivor keeps its place. |
| `cancel_oldest` | The resting order is removed; matching continues with the rest of the line. |
| `decrement` | Both sides shrink by the would-be match quantity; nobody "trades". |

## 3. Sessions and auction crosses

Trading days have phases ([Session.hpp](../include/mog/Session.hpp)):

- **Pre-market**: order entry builds up; nothing continuous-trades yet.
- **Opening cross**: instead of trading continuously at the open, the
  exchange runs one batch auction - it collects all interest, finds the
  single price maximizing matched volume, and executes everything at once
  at that clearing price. One bell, one price, many trades.
- **Regular trading hours**: the continuous FIFO world of chapter 01.
- **Closing cross**: same batch idea at the close.
- **NOII** (Net Order Imbalance Indicator): periodic broadcasts before
  open/close revealing indicative clearing price and how imbalanced the
  batch would be - a weather forecast for the coming cross.
- **Halts**: regulatory pauses (news pending, volatility limits), carried
  by Trading Action messages; mog gates matching during them.

mog replays all phases faithfully because strategies behave very
differently around opens, closes, and halts.
