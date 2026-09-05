# RFC 0002: Tainted fast-digest mode

- **Status:** accepted
- **Created:** 2026-09-05
- **Requires:** none (evidence: `results/proof-overhead-20260905/`)

## Problem

Two facts collide. First, the proof-overhead study measured SHA-256 trace
streaming as the largest sim-layer cost: -26.9% Ir/op and +14.2% wall
throughput for the already-existing `DigestMode::fast` tier, clearing the
pre-registered 10% bar for a user-facing fast mode. Second, fast mode is
currently a footgun: `trace_digest()` in fast mode silently returns SHA-256
of nothing, so `SimRun` stamps every fast run with the same constant digest
and two different fast runs compare equal. A mode that lies about identity
cannot ship, however fast it is.

## Proposed change

Loud modes, tainted outputs. Wrong-mode digest access becomes a contract
violation instead of a silent constant, and every surface that emits a
digest also emits the mode that produced it:

- `ExecutionSimulator::trace_digest()` requires golden mode;
  `fast_trace_digest()` requires fast mode (both `MOG_PRE`). Portable
  builds skip contract checks by design, so the accessors document
  check-first-use; all in-repo consumers dispatch on `digest_mode()`.
- `simrun::Summary` gains `DigestMode digest_mode`; `run()` fills it from
  the sim and takes `digest_high` from the mode-appropriate digest.
- `Orchestrate::global_digest()` requires uniform mode across instruments
  (`MOG_PRE`), folds the mode-appropriate per-sim bytes, and gains a
  `global_digest_mode()` accessor. The golden fold is byte-identical to
  today; the fast fold is new surface.
- `TimeTravelSession::export_report()` prints the mode-appropriate digest;
  golden reports render byte-identical to today.
- `mog simrun` gains `--digest-mode golden|fast` (default golden) and its
  JSON line gains `"digest_mode"`. The race harness JSON records both
  modes. Python `trace_digest`/`fast_trace_digest` bindings pre-check the
  mode and raise `ValueError` on mismatch instead of reaching the aborting
  guard; `DynamicStrategyRunner` gains `fast_trace_digest` for parity.
- No contracts-ignore mode: +2.3% wall is under the bar, so audits stay
  unconditional and `live_tracked_` growth under ignore remains a
  documented non-issue.

## Determinism impact

No golden byte moves. `trace_digest()`, the golden global fold, golden
time-travel reports, and golden `digest_high` are unchanged by
construction (new branches only execute in fast mode); the S8 anchors in
`tests/sim_scenarios.cpp` and `python/tests/test_mog.py` must pass
unmodified. Fast-tier digests are new surface: determinism (same input,
same digest) and variance (different input, different digest) are pinned
by new tests, not by anchors, since the tier carries no compatibility
promise beyond self-equality.

## Compatibility impact

C++23 fallback: `MOG_PRE` and `DigestMode` exist on all profiles; no new
language use. Portable wheels: contract guards are inactive there, so
C++-level wrong-mode access is unchecked in portable (documented on the
accessors); the Python bindings enforce mode explicitly on every profile.
CLI JSON gains one field (additive); race JSON gains two (additive).

## Performance impact

Guards sit on once-per-run finalizers, never in the replay loop; the
per-event `update()` mode branch predates this RFC. Expectation for the
implementing commit: CI perf gate green, race record config (`golden`,
enforce) numerically unaffected, Ir/op on the book meter unchanged.

## Alternatives considered

- Polymorphic digest return (one accessor, mode-dependent bytes): loses
  the type-level 32-vs-16 distinction that caught this bug class; rejected.
- `std::expected`/optional from accessors: loud without abort, but churns
  every call site including `noexcept` contexts for a misuse case, not a
  runtime case; contracts already encode exactly this distinction; rejected.
- Ship fast mode without taint (bare flag): recreates the original sin, a
  fast number quotable as a proven number; rejected on doctrine grounds.

## Implementation plan

Single commit (one logical unit): RFC, guards, consumer dispatch, taint
fields, CLI flag, binding behavior, regression tests, stale recipe fix.
Each test below runs green independently of the others.
