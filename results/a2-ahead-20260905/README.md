# Phase A2: incremental queue-ahead

Date: 2026-09-05. RFC-003 phase A2. Host: i5-7500T, pinned core 3.
Baseline: post-A1 binary (622k ops/s, 4,492 Ir/op).

## Problem

A no-op probe of `recompute_positions` cut Ir/op from 4,492 to 1,336:
the per-consume level walk is 70% of remaining instructions (~24 Ir per
visited order: payload reads plus guards and dispatch). Counters on the
faithful workload: 100,045 walks, 22.8M iterations (129 avg), 25% hitting
live tracked orders.

## Change

Maintain `qty_ahead` incrementally instead of re-walking the level:
consumes subtract take from tracked members (exact by FIFO: survivors
lose exactly take, anything reached further is victim-synced), cancels
and replaces keep their exact walks (rare paths, unchanged), sweeps
accumulate per-level takes and subtract once per level skipping
mid-sweep synced victims. Membership is a sparse sim-side map
(price to intrusive member list, 4096 entries); levels past capacity
degrade to exact walks. `recompute_positions` stays as the audit twin
(`audit()` re-derives, then verifies) and backs a public
`recompute_all_tracked()` used by the differential test.

## Results

Wall medians, 3 invocations of `--runs 5`, golden+enforce:

| round | A1 | A2 |
|---|---|---|
| 1 | 616,063 | 2,280,829 |
| 2 | 613,289 | 2,241,758 |
| 3 | 629,802 | 2,305,340 |
| median | 622,241 | 2,280,829 (+267%, 3.7x) |

Ir/op: 4,492 down to 1,643 (-63%). Combined program so far from the
164k faithful baseline: 13.9x with outputs bit-identical.

Behavior proof: per-order agreement A1 vs A2 is 16,123/16,123 exact;
fills 28,257 both; golden anchors green on both profiles plus
sanitizers; S14 exact-ahead asserts pass unchanged; new differential
test (15 randomized seeds incl. cancel/replace/IOC/depletion vs the
twin) and map-fallback test (5,000 distinct levels) green.
