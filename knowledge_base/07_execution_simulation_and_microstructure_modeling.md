# Chapter 07: Execution Simulation & Microstructure Modeling

**What you will learn:** the heart of the simulator - how it decides
*when your order fills*, how it models real-world delays, and how it
proves its fidelity against official exchange records.

---

## 1. The big picture: will my order trade, and when?

The book (chapter 01) tracks everyone else's orders. The execution
simulator ([Simulate.hpp](../include/mog/Simulate.hpp)) answers the
question a strategy actually cares about: *my* order is resting at $100.05
with 4,300 shares ahead of it - does it fill on the next wave of buyers,
and exactly when?

## 2. Queue position: your place in line

Your limit order joins the FIFO line at its price level. Fills then obey
one rule with no exceptions:

- Every external sell that executes at your price consumes shares from
  the front of the buy side's line.
- Your order fills **only** when every share ahead of you is gone.

mog tracks this exactly - it knows each remaining quantity ahead because
it watched every add/cancel/execute since the line formed (that is what
L3 data buys you). No "assume you get filled if price touches" hand-waving:
queue position is a number, maintained tick by tick.

Three details keep the number both exact and cheap:

- A new rest joins at the FIFO tail, so its ahead-count is the level
  total minus its own quantity - O(1), no line walk.
- Cancels, replacements, and STP-decrement matches re-settle the mates
  left behind, including the resting side's own fill report; the
  conservation audit fails loudly if mirror ever leaves book truth.
- That audit walks every mutation in checking builds and is skipped
  otherwise, so release replays pay one branch instead of a scan.

The rate at which volume ahead of you evaporates (**depletion**) is the
difference between filling in seconds versus never; chapter 07's models
below exist to make that evaporation realistic rather than optimistic.

## 3. Latency: nothing happens instantly

Real trading has travel time. mog models three delay stages on every
strategy action:

1. **Parse**: decoding the market data packet.
2. **Decision**: your strategy computing its response.
3. **Wire**: network round-trip to the exchange and back.

Each stage adds nanoseconds-to-microseconds of delay before the exchange
sees your order - during which the book moved. Delays are not constant;
they jitter. mog offers three statistical shapes
([Simulate.hpp](../include/mog/Simulate.hpp)):

- **uniform**: any value up to a max, equally likely;
- **exponential**: mostly quick, occasionally long-tailed - the classic
  shape of queueing delays;
- **normal** (bell curve): truncated at eight standard deviations so a
  freak sample can never exceed a hard bound (boundedness preserves
  determinism).

Every draw comes from your seeded random generator: same seed, same
delays, same fills (chapter 00).

## 4. Hawkes self-excitation: bursts are contagious

Markets do not emit trades like a metronome; they erupt. A large print
draws attention, algorithms react, their trades trigger more reactions -
excitement breeds excitement. The standard mathematical description is a
**Hawkes process**, where the instantaneous arrival intensity is

$$\lambda(t) = \lambda_0 \Big(1 + \sum_{t_k < t} \kappa\, e^{-(t-t_k)/\tau}\Big)$$

Unpacked in words: base activity $\lambda_0$ runs constantly; each past
trade at time $t_k$ adds excitement proportional to $\kappa$, fading
exponentially with half-life governed by $\tau$. Many recent trades means
a hot market expecting more trades. Relatedly, **momentum coupling**
tilts depletion toward whichever side the recent mid-price trend favors -
capturing adverse selection, where informed flow arrives directionally
and eats patient liquidity.

## 5. Ground truth: grading against the official tape

A simulator could be confidently wrong forever unless checked against
reality. The check: NASDAQ publishes the **Last Sale tape** - the legal
record of every trade that actually happened. mog reconstructs trades
purely from order-book events, then compares the two lists
([TradeDiff.hpp](../include/mog/TradeDiff.hpp),
[tools/nls_diff.py](../tools/nls_diff.py)).

Since the tape carries no IDs linking trades to orders, comparison is a
**multiset diff**: two bags of (symbol, timestamp, price, size) tuples
matched until only unexplained leftovers remain on either side. On a full
trading day mog reconstructs over 18 million prints with 99.957% matching
official records exactly - the residual decomposed into documented causes
(report-time aggregation, off-book liquidity), archived under
`results/ground-truth/`.

## 6. Scripted scenario runs: the lab notebook

Hand-checking a fill means controlling every input.
[SimRun.hpp](../include/mog/SimRun.hpp) reads a CSV script - one row per
event: seed external liquidity (`ext_add`), print an external trade
(`trade`), post or cancel a strategy order (`strat_limit`, `strat_ioc`,
`strat_cancel`), or pause the market (`halt`, `resume`) - and steps the
simulator through it in timestamp order, letting time-based models act on
the gaps between rows. Prints arriving while halted are dropped, not
queued.

Strategy refs live above 2^62 so they can never collide with external
ones; strategy rows take a flags column for hidden (`nd`) and pegged
(`mid`, `bid`, `ask`) orders. Every row drains the decision pipeline,
then appends maker and taker fills to one events log with mid marks -
the same log the tearsheet recomputes from alone, so a script plus its
log is a complete re-runnable experiment.
