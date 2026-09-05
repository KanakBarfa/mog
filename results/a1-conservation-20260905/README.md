# Phase A1: incremental conservation

Date: 2026-09-05. RFC-003 phase A1. Host: i5-7500T, pinned core 3.
Method: same binary recipe as the proof-overhead study; baseline is the
pre-change binary on the faithful workload (per-action drain).

## Change

Per-mutation `conservation_ok()` verified the whole live list
(O(live) `remaining_of` calls, 83% of Ir). Now it verifies only orders
dirtied since the last check (marks at the five mirror-mutation
choke points: track, victim sync, cancel, replace, STP victim), with a
full walk (which also prunes) every 512 calls. `check_conservation()`
and `audit()` are byte-for-byte the same semantics; no test calls the
fast path directly (S14/S15 assert via `audit()`).

## Results

Wall medians, 3 invocations of `--runs 5`, golden+enforce:

| round | baseline | A1 |
|---|---|---|
| 1 | 164,212 | 617,705 |
| 2 | 165,009 | 622,241 |
| 3 | 164,206 | 629,802 |
| median | 164,206 | 622,241 (+279%, 3.8x) |

Ir/op, timed region (176,096 ops): 23,704 down to 4,492 (-81%).

Behavior proof: per-order agreement baseline vs A1 is 16,123/16,123
exact qty+price; fills 28,257 both; all golden anchors green both
profiles plus sanitizers. K=512 untuned beyond order-of-magnitude:
detection delay for unmarked divergence stays under 512 mutations
while the amortized full-scan cost is ~1 entry per call.

## Reading

Remaining Ir/op (4,492) vs no-audit floor (4,099): the audit path now
costs ~10%, down from ~480%. Next largest: the level walk (phase A2)
and the digest tier (~10% in ignore mode).

## Addendum: full post-A1 matrix (modes converged)

One round of `--runs 5` per cell, same method. Pre-A1 faithful cells
for reference: A 164k, B 166k, C 627k, D 644k.

| cell | ops/s | vs A |
|---|---|---|
| A golden,enforce | 631,813 | - |
| B fast,enforce | 646,909 | +2.4% |
| C golden,ignore | 617,665 | -2.2% |
| D fast,ignore | 641,424 | +1.5% |

Spread is ±2.3%: measurement noise. The audit tax reads zero (C lands
on A), the digest tier reads ~2% on this workload. The mode matrix is
kept as a regression canary (a future divergence means a proof cost
regressed), not a product lineup: the published record is golden+enforce
only. Remaining gap to njit (2.0x) is general engine work, which is
what phases A2 and B attack.
