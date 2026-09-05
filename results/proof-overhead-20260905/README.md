# Proof overhead: what determinism proofs cost on the race workload

Date: 2026-09-05. Host: i5-7500T, 4C, g++-15. Branch work, commit `6ad4ba9`
plus uncommitted harness A/B flags (committed alongside this report).

## Question

How much replay throughput do the inline proofs cost: (a) SHA-256 trace
streaming vs the FastDigest-128 tier, (b) the conservation/contract audit
path? Pre-registered rule: a user-facing fast mode needs a measured win
above ~10%; below that the flag is complexity theater. Prior expectation
(small, single digits) is recorded so the miss is visible.

## Method

No engine changes. One binary built from `bench/race/participants/mog_native.cpp`
with two new default-preserving flags (`--digest golden|fast`,
`--contracts enforce|ignore`) reusing the existing `SimConfig::digest_mode`
knob and the `contracts::set_mode()` API. Four cells, same binary:

- A `golden,enforce`: the record config, baseline.
- B `fast,enforce`: isolates digest cost.
- C `golden,ignore`: isolates audit cost (`conservation_ok` early-out).
- D `fast,ignore`: floor.

Build (note: `-std=c++26`; the race README recipe still says c++23, which no
longer compiles since the saturating-arithmetic migration):

    g++-15 -std=c++26 -O2 -march=native -DMOG_PROFILE_FRONTIER=1 \
        -I include -I build/frontier/generated \
        bench/race/participants/mog_native.cpp -o /tmp/opencode/proof/mog_ab

Wall time: `taskset -c 2`, interleaved rounds, 3 invocations per cell of
`--runs 5` with 5 s cooldown; reported value is the median of the three
per-invocation medians. Ir: same source plus `-DMOG_RACE_CALLGRIND`
(callgrind toggles around the timed region only; CSV load/JSON dump
excluded), `callgrind --collect-atstart=no`, PROGRAM TOTALS / 176,096 ops.
Feed: `bench/race/out/feed.csv` (176,096 ops, the Round-4 canonical feed).

## Results

Wall throughput (ops/s, fills and self-determinism in parentheses):

| cell | r1 | r2 | r3 | median | vs A |
|---|---|---|---|---|---|
| A golden,enforce | 3,781,183 | 3,788,607 | 3,832,759 | 3,788,607 | - |
| B fast,enforce | 4,405,651 | 4,314,634 | 4,326,757 | 4,326,757 | +14.2% |
| C golden,ignore | 3,917,218 | 3,874,041 | 3,850,718 | 3,874,041 | +2.3% |
| D fast,ignore | 4,475,687 | 4,399,393 | 4,488,175 | 4,475,687 | +18.1% |

Instructions retired, timed region only (Ir/op = total / 176,096):

| cell | Ir total | Ir/op | vs A |
|---|---|---|---|
| A golden,enforce | 249,681,717 | 1417.7 | - |
| B fast,enforce | 182,411,347 | 1035.7 | -26.9% |
| C golden,ignore | 236,573,044 | 1343.3 | -5.2% |
| D fast,ignore | 169,302,674 | 961.4 | -32.2% |

Invariants held in all 12 invocations: 25,077 fills, deterministic=1.
Context: sim-level Ir/op (~1418) vs book-level Ir/op (193) means the book
mutation is ~14% of a simulated op; the rest is scheduler, tracking,
digest, STP, and decision pipeline.

## Reading

- The prior (proofs are cheap) was wrong. SHA-256 streaming is the single
  largest sim-layer cost: -26.9% Ir, +14.2% wall. The per-event `update()`
  calls hash a few dozen bytes each; block compression never amortizes.
- The audit path is cheap: -5.2% Ir, +2.3% wall. Conservation scans track
  live orders, not capacity, and the relaxed-mode gate is one load.
- Wall gains understate Ir gains (memory/branch/turbo effects), but both
  agree on ranking and both clear the bar for the digest tier only.

## Verdict

- A tainted fast-digest mode is justified by the numbers and may proceed to
  RFC. Taint is non-negotiable: outputs must record the proof level, golden
  anchors and the CI gate stay on full-proof mode.
- No contracts-ignore mode: +2.3% wall is under the bar. Audits stay
  unconditional; they are correctness guards, not a performance lever.
- Blocker found for the fast-mode RFC: in fast mode `trace_digest()`
  returns SHA-256 of nothing (the sha half is never fed), so `SimRun`
  would stamp every fast run with the same constant digest. Digest
  accessors must become mode-aware (or the golden accessor must refuse in
  fast mode) before fast mode is user-visible.

## Follow-ups (not this unit)

- Race README build recipe (`-std=c++23`) is stale; needs a docs touch.
- `live_tracked_` grows unboundedly within a run under contracts-ignore
  (pruning lives inside `check_conservation`); harmless at 176k ops, worth
  a note if ignore mode is ever used for long sweeps.
