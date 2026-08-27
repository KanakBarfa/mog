# RFC 0001: Bounded formal model of matching semantics (TLA+ + exhaustive C++ twin)

- **Status:** accepted
- **Created:** 2026-08-24
- **Requires:** none

## Problem

M9 gates the research track on demonstrated need, one item at a time, each
behind an RFC. The four candidates are JIT bridge, TLA+ spec, GPU sweeps,
and additional venues.

The differential suite (book/sim fuzz vs naive references) and the
contract system verify that *implementations agree with each other*. Nothing
in the repo verifies that the *specification-level* properties we claim in
docs - never-crossed book, fill conservation between queues and L2
aggregates, ref uniqueness, positive resting quantity - hold for **all**
reachable states, not just the states random streams happen to visit.
Randomized testing samples; it does not exhaust. The scenario that hurts
today: a subtle matching-semantics change can pass every current gate while
violating an invariant only reachable in a rare op ordering, because no gate
enumerates orderings exhaustively even at toy scale.

GPU sweeps need hardware this project does not have; additional venues need
captures this repo does not own; the JIT bridge has no consumer yet. None
have demonstrated need today. The TLA+ slice does: it is the cheapest item
that strengthens every existing gate's *meaning*, it is CI-verifiable, and
it produces a durable artifact future venues/bridges must update when they
change matching semantics.

## Proposed change

Three artifacts under `specs/matching/` plus one executable twin:

1. `BookMatching.tla` - bounded state machine over FIFO queues per
   (side, price) with an independently-maintained aggregate mirror `agg`.
   Three actions mirroring engine operations:
   - `Rest(ref, s, p, q)` - rest a limit order; precondition refuses to
     cross the touch (the post-only/decide() gate), appends at level tail.
   - `Cancel(ref)` - remove any existing order by ref.
   - `Deplete(s, p, q)` - consume up to q units from the front of one
     level (`apply_external` / `consume_level` analog); partial head fills
     allowed, so price-time priority is exercised structurally: only head
     entries ever mutate.
   - `MarketTake(s, p)` - remove **entire** levels priced through the
     bound (`cross()` analog); remainder is discarded, so no crossed state
     is reachable by construction rather than by invariant luck.

   Checked invariants: `TypeOK`, `Conservation` (per-level queue sums equal
   the independently-updated `agg` mirror), `NoCrossedBook`, `RefUnique`,
   `QtyPositive`. TLC explores the full reachability graph for small
   constants (3 refs, 3 prices/side, qty ≤ 2).

2. `MC.cfg` - constants, invariants, deadlock checking off (terminal
   states are legal: all refs cancelled/consumed).

3. `tests/matching_modelcheck.cpp` - the same action set driven against
   `OrderBook<>` directly, BFS over the full reachable state set with
   canonical-state deduplication, asserting the same five invariants via
   `audit()` plus explicit crossing/ref checks. This is the local,
   JVM-free half: ctest proves the *semantics* on every dev machine and
   profile; CI's TLC run proves the *spec* where Java exists. Spec drift
   from code shows up as either twin failing its counterpart's review.

4. CI job `tla`: Temurin 21 + pinned `tla2tools.jar`
   (v1.8.0, sha256 `eabd140a70f49eb9305a3bd3f3df944eddf87e5a90d329789085f8953a80533a`,
   verified against GitHub's asset digest and re-verified locally at pin
   time) running TLC with `-workers 2`; non-zero exit fails the job.

Scope deliberately excluded (follow-ups, each cheap to add): atomic
sweep-then-rest `Add` (needs recursive sweep machinery; `MarketTake` +
separate `Rest` cover the invariant surface v1 claims), STP/queue-position
modeling, liveness properties.

## Determinism impact

None. No traced value changes: parser, book, simulator, and Python shim
are untouched. All anchors stand (fuzz trace `5a69a38c…`, S8 digest
`498cda3c…`, parser golden `0x308e35B105CFE9D0`). The twin reads book
state through existing public const APIs only.

## Compatibility impact

C++23 fallback: twin uses nothing beyond the existing portable surface;
it builds and runs in every profile including contract-enforce frontier
(`audit()` calls are const). Wheels unaffected. New "dependency" is a
CI-downloaded, hash-pinned jar - not vendored, not a build dependency;
local development requires nothing new (this machine has no JVM, which is
precisely why the twin exists).

## Performance impact

Zero on tracked kernels: no hot-path file changes. The twin is a new
ctest with a bounded state space (target < 60 s wall on the bench
machine, enforced by ctest TIMEOUT like the other diff tests). The tla
job runs parallel to build jobs on its own runner.

## Alternatives considered

- **GPU sweeps / additional venues / JIT bridge first**: rejected for
  M9 sequencing - no hardware, no owned fixtures, no consumer
  respectively; each remains available behind future RFCs.
- **Alloy instead of TLA+**: nicer relational syntax, but TLC's
  command-line story fits CI better and the TLA+ ecosystem (VS Code,
  TLC) is actively maintained while Alloy tooling rotates more often.
- **Property-test-only approach (no .tla artifact)**: the exhaustive twin
  alone would still be valuable, but without the spec there is no
  language-agnostic artifact stating WHAT is guaranteed, only code
  asserting it. The pair is the point; either alone decays.
- **Vendoring the jar**: hermetic but 4.5 MB binary in git; hash-pinned
  download achieves the same reproducibility without the bloat.

## Implementation plan

1. Commit A: this RFC.
2. Commit B: `specs/matching/{BookMatching.tla,MC.cfg,README.md}`,
   twin test + ctest registration, `tla` CI job, `.gitignore` for TLC
   output, EVALUATION.md testing-strategy entry. Twin verified locally in
   portable/frontier/clang-frontier; TLC verified by first CI run (no JVM
   locally - stated plainly rather than hidden).
