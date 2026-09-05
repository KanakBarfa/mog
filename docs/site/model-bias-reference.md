# Model bias reference

Every known way MOG's execution model is wrong, on one page. Distilled from
`docs/FILLMODEL.md` (the specification) - read that for mechanics and
hand-verified scenarios. Nothing here is hidden in code comments; if you
find a bias missing from this table, file an issue.

## Flow generation

| Assumption | What it means | When it lies to you | Mitigation |
|---|---|---|---|
| Poisson depletion, intensities are **parameters, not fitted values** | `advance_time(dt)` draws per-side quantity at `rate_per_us`; directional pressure splits a budget as `total*(1±bias)/2` | Any absolute fill-rate claim without calibrating to your venue's visible trade rate / average displayed depth | Replay observed flow via `apply_external(side, price, qty)` - bypasses statistics entirely and stays exact |
| Hawkes-style clustering is first-order | Every consumption adds `hawkes_kappa` excitement decaying over `hawkes_decay_ns` | Real bursts have regime shifts and adverse-selection asymmetry this cannot express | Off by default; use only as a stress multiplier, not a forecast |
| Momentum signal is a sign memory | Last mid-move tilts bid/ask split by up to `momentum_gain`, fading linearly | Anything beyond polarity of the most recent move | Keep off unless testing sensitivity |
| Jitter models are uniform / exponential / normal with fixed shape (§5) | Latency pipeline stages draw from the configured kind | Tails differ from any real exchange's latency distribution | Measure with jitter off first; treat jittered runs as perturbation studies |

## Microstructure

| Assumption | Meaning | Boundary |
|---|---|---|
| Icebergs replenish at level tail under price-time priority | Display slice empties → fresh slice re-enters behind existing queue (S10 hand-verified) | Dark pools NOT modeled: venue property, out of scope by design |
| No partial-display granularity | External orders are full-size from entry; nothing materializes mid-queue | Orders appearing inside the visible queue ahead of you will not happen here but do happen live |
| Single venue | Fragmentation, routing, dark-pool interaction deferred to a venue-graph layer above this simulator | Cross-venue strategies get no signal from this layer |

## Queue position

The old "interleaving unresolved" caveat is **resolved**: tracked positions
are exact - recomputed from the real FIFO chain after every touching
mutation, with S9 pinning stacked-order arithmetic by hand. If you see an
estimate-shaped API, it is legacy; truth lives in
`OrderBook::for_each_in_level`.

## Analytics

| Assumption | Meaning | Boundary |
|---|---|---|
| Drawdown needs a positive running peak | `max_drawdown_bps` divides by peak equity from a zero base, so strategies losing from inception report 0 bps (absolute ticks are still exact) | Fund with an explicit capital base before trusting bps; ticks column stays comparable |

## Calibration status

No parameter in this repository claims empirical validity. The evaluation
suite exists to change that; until fits land against historical data, treat
statistical-mode results as *relative* comparisons between strategies under
identical synthetic flow - which is exactly what determinism makes them
good for.

## Verification posture

Hand-computed scenario suite (S1–S14), boolean conservation audit exposed to
harnesses instead of aborting, SHA-256 trace digests pinning every script,
and differential tests against a reference book. See
[Benchmark methodology](benchmark-methodology.md) for how perf claims are
gated separately.
