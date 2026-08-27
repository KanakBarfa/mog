# Benchmark methodology

How MOG's performance numbers are produced, gated, and reproduced. The full
counter-level writeup lives in `docs/EVALUATION.md`; this page is the
operating protocol.

## Protocol

1. **Pin a core.** Bench runs use `taskset -c N` (`MOG_GATE_CORE`, default
   2). If pinning is unavailable the tool says so and continues unpinned.
2. **Interleave, never compare across time.** Regression gates build BASE
   and HEAD into separate worktrees and run benches in alternating rounds.
   Frequency drift and neighbor noise hit both sides equally.
3. **Median of round medians.** Each round captures per-benchmark medians;
   the verdict compares per-side medians *across* rounds. Shared-runner noise
   moves single rounds by >10% on identical code; real regressions move every
   round. Any-round-fails logic was tried and rejected as flaky by
   construction.
4. **Tracked kernel subset.** Gates run representative shapes only - parser
   stream + decode pair; book mixed feed + add/remove cycle; scheduler
   cancel-heavy + sorted drain + near-sorted wheel - with short sample times.
   Full suites remain available locally.
5. **Threshold: 10%.** A crashing or data-less side fails the gate; one
   automatic retry absorbs transient flakes before that is declared.

## Reproduce

```sh
# Gate HEAD against merge-base (what CI runs):
tools/perfgate.sh "$(git merge-base HEAD origin/main)" --rounds 3

# Full evaluation suite: dual-profile tests, 1M-msg corpus through golden +
# book e2e, counter medians with machine context:
suite/run_suite.sh results/
```

`run_suite.sh` writes `env-<host>.txt` next to every result file:
throughput numbers are machine-local and meaningless without it.

## Reading numbers honestly

- Absolute throughput claims are tied to the committed baseline host
  (4-core desktop CPU, see `results/env-aws.txt`) and to the banded corpus;
  adversarial price distributions change working-set size and therefore the
  memory-bound numbers.
- Counter audits (IPC, branch misses, LLC miss attribution via `perf
  record`) accompany every gate verdict in evaluation reports, so a
  regression comes with its likely cause, not just its ratio.
- The perf gate can false-positive under heavy neighbor load by design of
  shared runners: a *consistent* red across reruns is signal; a single red
  on an unpinned runner warrants a rerun before diagnosis.

## Current targets (PLAN §perf table)

Parser >50M msg/s and book replay >20M events/s are MET on the baseline
host; tick-to-book latency targets are tracked but not yet CI-gated
(latency measurement needs isolation this repo does not assume).
