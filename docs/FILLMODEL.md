# Fill model whitepaper

This document is the specification of MOG's execution simulator
(`include/mog/Simulate.hpp`): what it assumes, what it guarantees, where it
is knowingly wrong, and how its numbers were produced. It is a deliverable of
M4, not an appendix.

## 1. Structure

The simulator layers on `OrderBook` without forking its semantics:

- **External resting depth** is ordinary book orders with refs below
  `external_ref_limit`. There is no shadow book: aggressive sweeps, depletion,
  and queue movement all execute through the same FIFO machinery the replay
  engine uses.
- **Strategy orders** are tracked in a parallel array indexed by arena slot,
  giving O(1) classification of any FIFO head as ours vs external.
- **Decisions** are scheduler events (`Scheduler<SimEvent>`), so inbound order
  processing inherits stable `(ts, seq)` ordering from M3.

## 2. Queue position

On rest, a tracked order records `qty_ahead = qty_at(level) - own_qty`: every
unit already at the price level is ahead of it, matching price-time priority.

External flow applied at a level executes real FIFO heads sequentially
(`execute_front`). Afterwards each tracked order at that level has its queue
estimate decayed by the units consumed:

```
qty_ahead -= min(qty_ahead, consumed_at_level)
```

Guarantees:
- Fills happen **only** when flow reaches the order itself (it becomes a FIFO
  head and is reduced). The model never invents quantity.
- Estimates never go negative; decay truncates.
- The tracker mirror equals book truth at every step; this is enforced as a
  boolean component of `audit()` so harnesses can report context instead of
  aborting.

A subtlety that the fuzzer caught and the implementation now encodes: a fully
consumed order leaves the id table *during* the reduction, so tracker sync
must capture the arena index before mutating, not look it up afterwards.

## 3. Depletion process

`advance_time(dt)` draws executed quantity per side from a Poisson
distribution with intensity `rate_per_us * dt_us`. Directional pressure is
expressed by splitting a total budget across sides:

```
bid_intensity = total * (1 + bias) / 2
ask_intensity = total * (1 - bias) / 2        bias in [-1, 1]
```

Positive bias depletes bids faster (downward pressure). Draw sequences are
seeded (`std::mt19937_64`) and therefore reproducible; observed-flow replay
bypasses statistics entirely via `apply_external(side, price, qty)`.

Calibration status: intensities are parameters, not fitted values. Reasonable
starting points come from visible trade rate divided by average displayed
depth at the touch; the eval suite (later milestone) is the intended place to
fit them - including `hawkes_kappa`, decay, and momentum gain - against
historical data. No calibration in this repository claims empirical validity
yet.

## 4. Order types and self-trade prevention

| Type | Crosses | Remainder |
|---|---|---|
| Day limit | fills within bound, then rests | rests at limit |
| Market | sweeps without bound | cancelled (no market rests) |
| IOC | fills within bound | cancelled |
| Post-only | rejected if any touch would cross | rests otherwise |

STP applies per potential match against a tracked resting victim:

| Mode | Behavior |
|---|---|
| none | self-match executes |
| cancel_newest | incoming stops matching; remainder cancelled |
| cancel_oldest | resting victim removed; incoming continues |
| decrement | both sides shrink by the mutual minimum |

Cancel/replace races are inherent: replaces resolve at decision time against
current book state, so a replace naming an order already consumed by
depletion rejects with the same error an exchange would return.

## 5. Latency pipeline

Submission schedules a decision event at `arrival + parse + decision +
U[0, jitter]`; fill reports are stamped `decision + wire + U[0, jitter]`.
Jitter is uniform by choice: no evidence supports a fancier distribution at
this stage, and uniform keeps hand-built expectations computable. Decisions
process strictly in scheduled order regardless of arrival order.

## 6. Known limitations

1. ~~Queue interleaving is unresolved.~~ **Resolved.** Tracked positions are
   now exact, not estimated: `OrderBook::for_each_in_level` walks the real
   FIFO chain and every tracked order's units-ahead is recomputed from it
   after any mutation touching the level. There are no interleaving
   assumptions left in the model; scenario S9 covers stacked tracked orders
   with externals between them and hand-computed positions (30 / 90 / 55 / 0)
   through sequential partial consumptions.
2. **Hidden liquidity: icebergs modeled, dark pools not.** External orders
   may register as icebergs (`seed_iceberg`): a display slice rests in the
   book and when consumed to empty a fresh slice of `display` re-enters at
   the tail of the level, matching exchange replenishment under price-time
   priority. Scenario S10 verifies slice accounting by hand (hidden
   50 -> 30 -> 10 -> exhausted, registry entry dropped when reserves run
   dry). Dark pools remain out of scope: they are a venue property, not an
   order property (see limitation 6).
3. ~~Poisson independence.~~ **Resolved (first order).** Intensities scale by
   `(1 + excitement)` where every external consumption event adds
   `hawkes_kappa` and the state decays exponentially over
   `hawkes_decay_ns`. A momentum signal records the sign of the last
   mid-price move and tilts the bid/ask intensity split by up to
   `momentum_gain`, fading linearly over `momentum_memory_ns`. Both default
   to off, preserving plain-Poisson behavior; scenario S12 pins clustering
   (excited runs consume strictly more than baseline under identical draw
   streams) and signal polarity. Higher-order structure (regime switching,
   adverse-selection asymmetry) remains future work.
4. **Uniform jitter.** See section 5.
5. **No partial-display granularity.** External orders are full-size from
   entry; there is no simulation of orders materializing mid-queue.
6. **Single venue, by design at this layer.** Fragmentation, routing, and
   dark-pool interaction are properties of a venue graph, not of execution
   against one book. They belong to the strategy/routing layer that sits
   above this simulator and are deliberately deferred until that layer
   exists; building them here would couple venue policy into a model that is
   meant to answer one question exactly: given visible depth and flow, what
   fills and when.

## 7. Measured divergence from naive fill

Naive baseline: every crossing submission fills its full quantity instantly
at the touch. From the seeded fuzz corpus (200k ops, seed 20260825, both
depletion rates 0.4/us):

```
naive      = 66496   (assumed)
aggressive = 66402   (realized while crossing)
passive    =   138   (queue reached by later flow)
```

Interpretation: naive overestimates aggressive execution by 0.14% in this
regime because sweeps hit finite depth; passive fills are a separate channel
naive accounting does not model at all. The numbers are regime-specific and
reported per-run by `tests/sim_diff.cpp`; they exist to quantify the gap, not
to claim realism.

## 8. Fees (maker/taker tiers)

Fee settlement is signed basis points of notional, configured per run via
`SimConfig::maker_fee_bps` / `taker_fee_bps`:

- **Taker** applies once per decision to the aggressive leg's exact integer
  notional (`SimDecision::fee`). Partial-then-rest orders pay only on the
  crossed part; the resting remainder later earns maker treatment.
- **Maker** applies per passive fill (`SimFillReport::fee`), computed from
  that fill's price x qty. STP decrement matches settle both ways at once:
  the incoming order pays taker on the mutual minimum while the resting
  victim earns maker credit on the same units.

Sign convention: fees are *cash impact*. A conventional schedule is
`taker_fee_bps > 0` (charge) and `maker_fee_bps < 0` (rebate), but any sign
combination is honored literally - the schedule is yours, the arithmetic is
fixed. Rounding truncates toward zero in integer ticks; the truncation is
part of the contract, not an accident, so third-party recomputation from
logged fills reproduces every number exactly (the M5 accounting doctrine).

Fees never enter the trace digest: they are derived downstream of fills and
decisions, both of which are hashed. Zero rates are the default and produce
bit-identical output to the fee-free engine; the determinism anchors are
therefore untouched by this feature's existence.

Accounting lands in `Metrics::Account::net_fees()`; `on_fill` /
`on_aggressive_fill` accept the signed fee so equity = cash + inventory x
mark already includes settlement. The Strategy API carries the same fields
(`OrderAck::fee`, `OrderFill::fee`) through hooks and columnar logs.

Verified by `tests/fees_e2e.cpp`: exact, truncating, rebate, mixed
lifecycle, and zero-rate-digest-equality cases, all hand-computed.

## 9. Verification

- `tests/sim_scenarios.cpp`: hand-computed expectations for every order type,
  all four STP modes, queue arithmetic, replace-to-tail, pipeline ordering,
  and double-run trace equality.
- `tests/sim_diff.cpp`: 200k-op seeded fuzz under enforce-profile contracts;
  continuous `audit()` (book + wheel + tracker mirror); determinism by
  SHA-256 over decisions and fills:

```
sha256 trace: 5a69a38c071b0b47673d49d250169733c19a1ff9470524b546f1321b4bd2bee0
```

Identical digest under ASan/UBSan builds.
