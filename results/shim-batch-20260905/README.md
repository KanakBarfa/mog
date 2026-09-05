# Step 2 notes: shim profile, harness honesty, batch ingest

Date: 2026-09-05. Host: i5-7500T, pinned core 3, repo venv (editable
portable build) unless noted.

## Profile decomposition (canonical race feed, steady state)

| component | ns/op | method |
|---|---|---|
| python loop + branch | ~55 | bare loop over rows |
| `int()` field parsing | ~110 | full vs pre-converted replay |
| nanobind dispatch + arg conversion | ~15-60 | pre-converted replay 279 vs native 265 |
| engine market op (shallow) | ~200-265 | native race arm |
| sim construction (arena 2M) | 65-120 ms one-off | ctor sweep |
| parse + per-message callback + box | 185/msg | counting sink |
| parse + callback + `book.apply` | 351-382/msg | apply sink |
| `parse_apply` fused ingest | 111/msg | new API, parity-proven |

The binding call was never the bottleneck: pre-converted replay runs at
279 ns/op against native's 265. The recorded 12.5x gap was workload and
harness shape, not call overhead. A batch *sim* API would save ~15 ns of
dispatch against hundreds of nanoseconds of engine work: not built,
falsification recorded here instead of a feature.

## Harness divergence (fixed, committed separately)

Native drained once at end of run; Python drained per action and fired
each action one market row late. Identical bytes: 25,077 vs 28,268
fills. Both fixed to fire-after-k-rows plus per-action drain (matching
hftbacktest's per-event loop and `simrun::run`); both arms now agree
per-order exactly (16,123/16,123, exact qty+price). Native record fell
3.79M to 164k ops/s; see `../proof-overhead-20260905/CORRECTION.md`.
Round-3 finding re-confirmed under faithful semantics: 0 common orders
of 16,123/17,634, all 16,123 truth fills passive, theirs 0.

## Shipped: `parse_apply`

Fused parse-plus-apply in one C++ call, GIL released, same per-message
code path: 111 vs 382 ns/msg (3.4x), byte-identical books (consumed,
best bid/ask, full L2 over 100k synthetic messages). Error behavior
matches `parse_itch` (`MogParseError` on truncation).

## Engine opportunities (not this unit, no engine changes)

- `recompute_positions` walks the whole level FIFO per consume:
  measured ~5 ns per resting order at the level (C++ depth probe:
  257 ns at depth 1k, 25 us at 10k, 198 us at 40k, portable).
- Per-mutation conservation scan is 82.7% of Ir on deep workloads
  (CORRECTION.md matrix). Same violations, fewer instructions: the top
  engine target, needs its own RFC and A/B.
- Construction touches ~350 MB at arena 2M (ladder pools, id table,
  tracker): ~20 ms base plus ~40 ns/slot, all first-touch faults and
  zeroing. Sizing guidance added to the Python API docs; lazy
  initialization would be engine work.

## Addendum: shim re-measured post-A1/A2

Three invocations of `--runs 5`, pinned, cooldown, same feed and harness:
590,509 / 590,410 / 582,421 ops/s (28,257 fills, deterministic all runs).
Median 590,410, up 1.8x from 330k: the engine passes flow straight
through the portable shim build. Native-to-shim ratio is now 3.9x
(2.28M vs 590k); the remainder is Python harness overhead (protocol
objects, fills list, digest), not binding dispatch.
