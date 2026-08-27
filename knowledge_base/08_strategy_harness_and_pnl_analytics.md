# Chapter 08: The Strategy Harness, PnL & Execution Quality

**What you will learn:** how trading logic plugs into the engine, how
profit is computed (including fees and rebates), and how "did I trade
well?" is measured with markouts.

---

## 1. The big picture: a form to fill in

A strategy answers four questions: the book moved - react? a trade
printed - react? my order was acknowledged? my order filled? mog's harness
([Strategy.hpp](../include/mog/Strategy.hpp)) hands you those four events
as plain function calls:

```cpp
template <class Derived>
class Strategy {                       // CRTP: see below
public:
    void on_order_book_update(const BookUpdate&) noexcept {}   // book changed
    void on_trade(const TradePrint&) noexcept {}               // a print happened
    void on_order_ack(const OrderAck&) noexcept {}             // exchange accepted/rejected
    void on_order_fill(const OrderFill&) noexcept {}           // you traded
protected:
    Derived& self() { return static_cast<Derived&>(*this); }
};
```

Subclass it, override what you need, and the engine calls you at exactly
the right simulated instants.

### Why CRTP instead of virtual functions?

The normal object-oriented tool for "call user code" is a *virtual
function*: each call looks up the right function in a runtime table
(the vtable) - one indirection, and it blocks the CPU from inlining.
`virtual` costs nanoseconds per call; at millions of events that is real
money.

CRTP ("Curiously Recurring Template Pattern") moves the lookup to compile
time: `MarketMaker` declares itself as `class MarketMaker : public
Strategy<MarketMaker>`, so the engine compiles specialized code that
calls your functions *directly* - zero indirection, full inlining. Same
polymorphism, none of the runtime tax. That is doctrine #6 (boring
interfaces, exotic internals) applied to yourself.

## 2. Counting the money: PnL accounting

Profit-and-loss ([Tearsheet.hpp](../include/mog/Tearsheet.hpp)) splits
into three honest layers:

- **Realized cash**: every trade moves cash - buys subtract
  price x shares, sells add it.
- **Unrealized**: shares still held are valued at the current mid-price;
  profit on paper, not yet in the bank.
- **Equity** = cash + inventory value + accumulated fees.

### Fees: the maker-taker world

Exchanges charge or pay per share traded. **Takers** cross the spread
(consuming liquidity): they pay a fee. **Makers** rest orders others hit
(adding liquidity): many venues pay them a rebate. Rates are quoted in
**basis points** (bps) of notional value - 1 bp = 0.01%, so 10 bps on a
$100,000 trade is $100. mog computes signed fees as

$$\text{fee} = \text{trunc}\left(\frac{\text{notional} \times \text{rate\_bps}}{10{,}000}\right)$$

with truncation toward zero so fee accounting itself stays deterministic,
and rebates enter as negative fees. Bit-identical results at zero rates
are regression-tested (`tests/fees_e2e.cpp`).

## 3. Markouts: did the market agree with you?

Raw profit can come from luck. **Markout** isolates execution quality:
after your fill, did the price move your way?

$$\text{markout}(\Delta t) = \text{side} \times \frac{P_{\text{mid}}(t+\Delta t) - P_{\text{fill}}}{P_{\text{fill}}} \times 10{,}000 \;\text{(bps)}$$

Read it as: N basis points after my buy of size X, the mid-price was this
much above/below where I bought. Positive means the market rewarded you -
you bought before a rise. Consistently negative means adverse selection:
you keep buying right before falls, i.e., better-informed flow is using
you (chapter 07's momentum coupling models exactly this force).

mog computes markouts at several horizons ($\Delta t$ = 1ms, 10ms, 100ms,
1s), because "good fill" depends entirely on how long you hold: a fill
good at +1s may look terrible at +1ms.
