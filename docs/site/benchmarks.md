# Benchmarks: The Race

mog against the two most credible open-source microstructure simulators on
identical workload bytes. Harness: `bench/race/`. Machine: i5-7500T desktop;
medians over warm runs; turbo-cooldown between participants (ordering bias
measured at ~4.6x without it).

## Round 1 - throughput

Canonical 200k-op feed (132k market rows, 44k scripted actions):

| participant | events/sec | self-deterministic |
|---|---:|:---:|
| hftbacktest 2.4.4, numba njit loop | **1,246,000** | yes |
| **mog** native C++ (frontier) | **715,920** | yes |
| hftbacktest 2.4.4, python-driven | 391,000 | yes |
| nautilus_trader 1.231.0 (tick ingest baseline) | 133,000 | yes* |
| mog python shim driver | **61,122** | yes |

Stated plainly: their njit fast path replays faster than our engine -
1.7x - because it does less per tick. Our number carries exact per-op
queue recomputation, conservation audits, decision pipelines, STP checks
and SHA-256 trace hashing inline. Speed-with-proofs is the product; raw
replay alone was never the claim.

## Round 2 - nautilus full loop

Blocked upstream: with quotes driving the L1 book and two-sided scripted
flow, their position engine asserts (`side != PositionSide.FLAT`) inside
`_handle_position_update` regardless of OMS/account configuration. Adapter,
quote synthesis and diagnosis shipped in `bench/race/participants/
nautilus_race.py`; revisit when upstream resolves.

## Round 3 - fill agreement vs ground truth

Per-order comparison on identical bytes, mog as exact-FIFO ground truth:

| metric | hftbacktest-njit vs truth |
|---|---|
| common filled orders | 0 of 10,488 / 17,634 |
| passive (resting limit) fills | truth: ~5,000+ - theirs: **0** |
| total filled units | 1.20M vs 1.39M (+16%, aggressive-only) |

Their supported L3 path (`ADD_ORDER` + `TRADE`, `l3_fifo_queue_model`)
filled every marketable order and **not one passive order**: resting GTC
limits never consumed a unit of the printed flow. Under exact FIFO
reconstruction roughly half of those posts fill - the entire maker side of
a maker-taker strategy would be invisible in such a backtest. Caveat
recorded honestly: upstream may expect a different event composition to
drive passive fills; the finding is stated as configuration-plus-outcome,
and the harness exists to re-run both halves.

## Reproduce

See `bench/race/README.md`. Determinism anchors and all suites were green
at every number above.
