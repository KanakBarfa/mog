# RFC 0003: Internal-only optimization program (beat njit, keep proofs)

- **Status:** accepted
- **Created:** 2026-09-05
- **Requires:** none (evidence: `results/shim-batch-20260905/`,
  `results/proof-overhead-20260905/CORRECTION.md`)

## Problem

On the faithful race workload the record config does 164k ops/s against
hftbacktest-njit's 1.25M. The gap is measured proof work, not engine
defects: the per-mutation conservation scan is 83% of retired
instructions, the level walk scales ~5 ns per resting order. Owner
mandate: close the gap with internal changes only. No proofs removed,
no features killed, no behavior change. Linux and macOS; Windows stays
out of scope.

## Proposed change

Two phases, each independently shippable and revertible. Every phase
holds outputs bit-identical (golden anchors govern) and violation
detection equivalent (S14/S15 govern).

- A1: incremental conservation. Dirty-flag orders touched since the last
  check; re-verify only those instead of walking the live list per
  mutation. Target: remove most of the 83% Ir share on deep workloads.
- A2: incremental queue-ahead maintenance. Maintain `qty_ahead` on
  mutate paths instead of re-walking the level per consume, keeping
  `recompute_positions` as the debug twin run by differential tests.
  Target: remove the ~5 ns/resting-order term.

## Determinism impact

None by design: no traced value may change. Each phase lands only with
golden anchors, S14/S15, fuzz, and both profiles green. A phase that
misses its target is reverted and recorded, not tuned until it passes.

## Compatibility impact

C++23 fallback kept (new code uses no newer language); portable profile
keeps a correct baseline kernel on every ISA. Python API and CLI surfaces
unchanged.

## Performance impact

Target, not promise: record config past 1.25M on the faithful workload
(~7.6x from 164k). Measured stack: A1 ~3.8x deep, A2 ~3.7x deep (13.9x
combined, 164k to 2.28M). Shallow workloads gain less (digest/parse kernels dominate
there). The CI perf gate and the book Ir meter guard every landing;
regressions need counter-level attribution.

## Alternatives considered

- Strip proofs for speed: DITCHED by owner; it concedes the product.
- New data structures first (exotic ladders): profile says the cost is
  audit walks, not ladder ops (book kernels already do 9-27M/s);
  revisited only if A-phases stall.
- Batch sim API: falsified in step 2 (dispatch is ~15 ns); not built.

## Implementation plan

A1, then A2; one commit per phase, each with its A/B
(Ir/op, wall medians, digest equality) recorded under `results/`.
