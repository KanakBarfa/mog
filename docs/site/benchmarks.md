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

Stated plainly for Round 1: their njit fast path replayed faster than our
engine - 1.7x - because it does less per tick. Our number carries exact
per-op queue recomputation, conservation audits, decision pipelines, STP
checks and SHA-256 trace hashing inline. Speed-with-proofs is the product;
raw replay alone was never the claim. (Outcome superseded: Round 5 re-runs
this workload under faithful drain cadence.)

## Round 2 - nautilus full loop

Blocked upstream: with quotes driving the L1 book and two-sided scripted
flow, their position engine asserts (`side != PositionSide.FLAT`) inside
`_handle_position_update` regardless of OMS/account configuration. Adapter,
quote synthesis and diagnosis shipped in `bench/race/participants/
nautilus_race.py`; revisit when upstream resolves.

## Round 3 - fill agreement vs ground truth

Per-order comparison on identical bytes, mog as exact-FIFO ground truth
(truth arm re-run with faithful per-action drain; their arm unchanged):

| metric | hftbacktest-njit vs truth |
|---|---|
| common filled orders | 0 of 16,123 / 17,634 |
| passive (resting limit) fills | truth: 16,123 (every filled order) - theirs: **0** |
| total filled units | 1.39M vs 1.85M (theirs under-fills by 24%) |

Their supported L3 path (`ADD_ORDER` + `TRADE`, `l3_fifo_queue_model`)
filled every marketable order and **not one passive order**: resting GTC
limits never consumed a unit of the printed flow. Under exact FIFO
reconstruction roughly half of those posts fill - the entire maker side of
a maker-taker strategy would be invisible in such a backtest. Caveat
recorded honestly: upstream may expect a different event composition to
drive passive fills; the finding is stated as configuration-plus-outcome,
and the harness exists to re-run both halves.

## Round 4 - re-run after the optimization pass (SUPERSEDED)

Same 176,096-op canonical feed, same i5-7500T host class, median of 5 with
cooldown, commit `02b171a` (competitors not re-run):

| participant | events/sec | self-deterministic |
|---|---:|:---:|
| **mog** native C++ (frontier) | **3,765,650** | yes (25,077 fills, identical) |
| mog native C++, pre-optimization control (`51f4faa` rebuilt here) | 698,421 | yes |
| hftbacktest 2.4.4, numba njit loop (Round 1) | 1,246,000 | yes |
| mog python shim driver | 300,401 | yes |

The control reproduces Round 1 within 2%, so the 5.4x native gain and the
~3x lead over njit are engine work, not the machine. Fills identical.

Superseded by Round 5: the native harness drained once at end of run, so
strategy quotes never rested intraday and the workload did less per tick
than every other arm. The numbers above are preserved as-run; do not quote
them. See `results/proof-overhead-20260905/CORRECTION.md`.

## Round 5 - faithful drain cadence

Same feed, per-action drain in both mog harnesses (a quote posted at row
k is hittable at row k+1), medians over repeated runs with cooldown:

| participant | events/sec | self-deterministic |
|---|---:|:---:|
| **mog** native C++, golden digest, audits on (record config) | **2,281,000** | yes (28,257 fills) |
| hftbacktest 2.4.4, numba njit loop (Round 1, per-event) | 1,246,000 | yes |
| mog python shim driver | 590,000 | yes (28,257 fills) |

Both mog arms agree per-order exactly: 16,123/16,123 orders with exact
quantity and price via `bench/race/agree.py`. The record config climbed
from 164k to 2.28M via two internal passes (incremental conservation,
incremental queue-ahead: RFC-003 A1/A2) with outputs bit-identical at
every step.

## Reproduce

See `bench/race/README.md`. Determinism anchors and all suites were green
at every number above.
