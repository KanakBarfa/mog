# Evaluation & Hardening Methodology (M6)

How performance numbers in this repository are produced, gated, and
challenged. Every claim here is reproducible from a clean checkout with the
committed scripts; where a number cannot be reproduced on your machine, that
is expected and the protocol explains why.

## 1. Protocol

1. **Pin a core.** All benchmark runs use `taskset -c N` (N=2 by default,
   overridable via `MOG_GATE_CORE` / `MOG_SUITE_CORE`). If pinning is
   unavailable the gate proceeds unpinned and says so.
2. **Interleave, never compare across time.** Regression gates build BASE and
   HEAD into separate worktrees and run their benches in alternating rounds
   (base, head, base, head, ...). Frequency drift, thermal throttling, and
   neighbor noise then hit both sides equally.
3. **Median of round medians.** Each round captures per-benchmark medians;
   the verdict compares the per-side medians ACROSS rounds. Runner noise
   moves single rounds by >10% (observed on identical code); a real
   regression shifts every round and survives the aggregate. Any-round-fails
   logic was tried and rejected as flaky by construction.
4. **Tracked kernel subset.** The gate runs representative shapes only -
   parser stream + decode pair, book mixed feed + add/remove cycle,
   scheduler cancel-heavy + sorted drain + near-sorted wheel - with
   `--benchmark_min_time=0.25s`. Full suites stay available locally.
5. **Threshold: 10%.** A crashing or data-less side is a failure, not a
   skip; one automatic retry absorbs transient runner flakes before that is
   declared.
6. **Numbers are machine-local.** Absolute throughput is only meaningful
   together with `results/env-<host>.txt`, which the suite emits every run.

Gate wall-time on a 4-core runner: roughly one minute for two rounds plus
two worktree builds.

## 2. Hardware

| Machine | CPU | Cores | L3 | Kernel | Role |
|---|---|---|---|---|---|
| audit host (`aws`) | Intel i5-7500T @ 2.70 GHz (Kaby Lake) | 4 | 6 MiB | 7.0.0-30 | baselines, counter audit |
| CI runners | unspecified shared x86_64 | 4 | - | - | gates only (relative) |

Budget targets from DESIGN.md bind on hardware comparable to the reference
machine; on slower/loaded hosts the suite reports values with a MISSED/MET
flag for context, not as release blockers.

## 3. Counter audit (2026-08, i5-7500T, frontier profile, `-march=native`)

`perf stat` medians over ≥2 s steady-state runs, one pinned core:

| Loop | IPC | branch-miss | LLC-miss | rate | budget |
|---|---|---|---|---|---|
| Parser stream (1M msgs/iter) | 2.21 | 0.94% | 1.47/msg (streaming) | 59.1M msg/s | >50M msg/s MET |
| Book mixed feed (2.8M ops/iter) | 0.66 | 5.2% | ~14/op | 21.9M ops/s | >20M ops/s MET |
| Scheduler cancel-heavy | 0.85 | 0.90% | high (erase-random) | 893k ops/s | (no absolute target) |

Findings:

- **Parser**: IPC 2.21 with sub-1% branch misses. The corpus uses uniform
  random message types, so type-dispatch mispredicts sit near the entropy
  floor; remaining cost is cold-stream DRAM traffic (32 MB working set vs
  6 MiB L3). No action.
- **Book**: IPC 0.66 is memory-stall dominated, not branch-dominated.
  `perf record -e cache-misses` attributes misses across the id-table probe,
  arena slots, and ladder pages - classic random access over a ~10 MB+ live
  set (160k concurrent orders is deliberately adversarial; real single-
  instrument depth is orders of magnitude smaller). The 20M events/s budget
  is met even so. Restructuring residency (pool sizing, prefetch of probe
  chains) is flagged as future research, not hardening.
- **Scheduler**: 0.9% branch misses confirm the wheel/heap decision logic;
  erase-heavy patterns thrash by nature of priority structures under random
  deletion. Documented, no action.

## 4. Hardening outcome: ladder page-pool initialization bug

Setting up the bench harness surfaced a real correctness bug that all
functional tests had missed: the paged ladder's page pool is allocated with
raw `::operator new`, so `Page`'s default member initializers never run, and
the construction sweep initialized `levels` but not `occupancy`. Fresh
mmap-backed pages are zero-filled, masking the bug; any process that
constructs a book on recycled heap memory (Google Benchmark constructs one
per calibration call) started with garbage occupancy counters and tripped the
drain precondition.

- Fixed in `include/mog/Ladder.hpp` (sweep covers occupancy).
- Regression test added to `tests/book_e2e.cpp`: construct/destroy cycles
  with allocator churn between epochs.
- Structural prevention: benches now run in CI (perf gate), so multi-epoch
  harness patterns execute on every push instead of only on developer
  machines.
- Diagnosis chain preserved for posterity: sanitizer pass + native abort ->
  standalone replay clean for 7000+ iterations -> two-world reproduction ->
  valgrind origin trace to the aligned operator-new allocation.

## 5. Asm parity

`tools/asm_parity_ci.sh` builds the parser bench at merge-base and at HEAD
with a fixed ISA target (`-DMOG_ARCH=x86-64-v3`, never `-march=native`),
snapshots the tracked kernels' disassembly from both, and requires equality.
Same job, same compiler, same flags: every diff is attributable to the change
under review. Intentional codegen changes are acknowledged with
`ASM_PARITY_ALLOW=1` and documented in the PR.

Note: the tracked symbol set includes the bench's parse entry point, which
inlines corpus-generation helpers; touching test-support code can move it.
That is by design - if the hot entry's body changed at all, a human should
look once.

## 6. Compatibility drill

The compatibility doctrine (DESIGN.md) requires re-running the compatibility matrix against a newer
compiler than CI uses. Drill (2026-08): Clang 21.1.8 vs GNU 15.2.0,
frontier profile:

- Feature ledgers identical across all rows (only the compiler line differs).
- Floor features present: `EXPECTED`, `SATURATE_ARITHMETIC`, `EMBED`,
  `PACK_INDEXING`.
- Known-absent upstream features (`STD_SIMD`, `INPLACE_VECTOR`, `HIVE`,
  `FUNCTION_REF`, `ATOMIC_MIN_MAX`) remain absent under both vendors; the
  vendored fallbacks carry the load. Notably `std::atomic::fetch_max`
  still does not exist, validating M5's CAS-max implementation.

Verdict: matrix holds one full compiler release ahead of CI's newest
(llvm.sh 20). No action required.

## 7. Evaluation suite v0

`suite/run_suite.sh [OUT_DIR]` produces, per run:

1. Environment header (`env-<host>.txt`) - commit alongside results.
2. Full ctest on frontier and portable profiles (logs kept).
3. A deterministic 1M-message ITCH dataset (seeded, price-banded to fit
   realistic ladder residency); parser golden and OrderBook-vs-reference
   differential must both accept it end-to-end.
4. Perf medians (pinned core, repetitions=3) for all three bench binaries as
   JSON plus a rendered markdown table with budget verdicts.

Committed baseline: `results/` (audit host, g++ 15.2.0, commit recorded in
the env file). Reproduce anywhere with the script; expect different absolute
numbers on different hardware - that is what the env file is for.

## 8. Formal bounded model (RFC-0001)

Differential testing samples op streams; it never enumerates orderings.
`specs/matching/BookMatching.tla` states matching semantics as a bounded
state machine - FIFO queues per level plus an independently-maintained
aggregate mirror - with actions mirroring engine operations (`Rest`,
`Cancel`, `Deplete`, `MarketTake`) and five invariants: `TypeOK`,
`Conservation`, `NoCrossedBook`, `RefUnique`, `QtyPositive`. TLC explores
the full reachability graph in CI (`tla` job, hash-pinned tla2tools v1.8.0).

The JVM-free half lives in ctest: `tests/matching_modelcheck.cpp` drives
`OrderBook<>` through the same action set and asserts the same invariants
over the full reachable state set (~1.3k states at depth 12, sub-second).
Spec and twin are reviewed together; either drifting from the other is a
review-visible failure by construction.

Scope note: price-time priority is enforced structurally in both artifacts
(only head entries mutate), not checked as a separate property; STP and
sweep-then-rest adds are deferred follow-ups under the same RFC.

## 9. Honest limitations

- GitHub shared runners add scheduling noise the interleaving mitigates but
  cannot eliminate; a red perf gate warrants one rerun before investigation.
- The book working set at adversarial scale exceeds desktop L3; conclusions
  about "DRAM-bound" here do not transfer to server-class caches without
  re-measurement.
- Counter-based audits reflect this microarchitecture; ISA-specific effects
  (e.g., AVX-512 downclocking) are untested on this machine.
- The asm-parity tripwire covers the parser kernel set; book/scheduler
  kernels are candidates once symbol stability across inliner versions is
  validated for them.
