# Correction: faithful drain cadence inverts the cost ranking

Date: 2026-09-05. The original study above is preserved as-run; what
follows corrects its workload, not its arithmetic. Do not quote the
original matrix without this note.

## The bug

The native harness drained once at end of run while the Python harness
drained per strategy action (and fired each action one market row late;
both fixed). Same engine, same feed: 25,077 vs 28,268 fills. Per-action
drain is the faithful market-making semantics (a quote posted at row k
is hittable at row k+1); it also matches hftbacktest's per-event loop
and `simrun::run`, which drains per row. The original numbers measured
an end-drain workload where strategy depth never accumulated intraday.

After the fix both mog harnesses agree exactly: 28,257 fills,
16,123/16,123 per-order qty+price exact via `bench/race/agree.py`.

## Fresh matrix (per-action drain, else same method)

Wall medians, i5-7500T, pinned, 3 invocations of `--runs 5`:

| cell | ops/s | vs A |
|---|---|---|
| A golden,enforce | 164,206 | - |
| B fast,enforce | 165,524 | +0.8% |
| C golden,ignore | 627,485 | +282% (3.8x) |
| D fast,ignore | 644,476 | +293% (3.9x) |

Ir/op, timed region only (176,096 ops):

| cell | Ir/op | vs A |
|---|---|---|
| A golden,enforce | 23,704 | - |
| B fast,enforce | 23,294 | -1.7% |
| C golden,ignore | 4,099 | -82.7% |
| D fast,ignore | 3,690 | -84.4% |

## Reading

The ranking inverts. With strategy depth live intraday, the
per-mutation conservation scan is 82.7% of retired instructions; the
digest tier is 1.7% (enforce) to 10% (ignore) - under the pre-registered
10% bar on this workload, over it only on shallow workloads where the
original 14% wall / 27% Ir stands. The fast tier stays shipped (guards
and taint are correct regardless), but its headline justification is
workload-specific, recorded here as such.

The audit cost is now the top engine opportunity (see step-2 notes):
same violations, fewer instructions. No engine change in this unit.
