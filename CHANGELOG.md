# Changelog

All notable changes to mog are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versioning is
semver, single-sourced from CMakeLists.txt.

## [Unreleased]

### Fixed
- STP decrement now syncs the resting tracker mirror and emits the maker
  fill report; `audit()` failed on this path before (new S15 coverage).
- Cancelled and replaced-away orders settle queue positions of mates left
  behind (new S14 coverage).
- Scripted halt/resume rows no longer submit phantom zero-qty orders that
  polluted decisions and the trace digest.
- OUCH order-table erase uses backward-shift deletion; the old
  reinsert-during-traversal could grow the table mid-erase.
- MoldUDP64 adopts a new session on heartbeat failover instead of reporting
  a false sequence gap.
- Sweeps touching more than 8 price levels now settle queue positions for
  every touched level; the overflow level settles on level exit.
- Session-tolerant listen path honors the caller's ISA kernel table, so
  differential tests pin the selected kernel.
- Online markout fills wait for their horizon mids instead of dropping to
  zero when streamed in event order.
- Price-ladder page shift capped at 12; larger shifts shifted a u64 by 64 or
  more, which is undefined behavior.

### Changed
- Hot-path cycle cuts, all A/B measured in `results/ab-hotpath-20260905/`:
  per-mutation conservation scan gated on active contracts, slot-indexed
  queue recomputation, probeless FIFO-head matching, lazy slot allocation,
  O(1) tail rest (110x rest-heavy), take-path handle threading.
- Falsified with evidence and reverted: lazy page zeroing, hierarchical
  directory bitmaps, order-carried table slots (each regressed Ir/op).

### Added
- Runnable examples: `examples/python/quickstart.py` (also executed by the
  wheel test suite), `examples/python/market_maker.py`,
  `examples/cpp/crtp_strategy.cpp`, `examples/cpp/queue_inspect.cpp`;
  built behind `-DMOG_BUILD_EXAMPLES=ON`, each registered with CTest.

## [0.1.0] - 2026-08-25

First public baseline: milestones M0 through M8 shipped, gap ledger closed.

### Added
- ITCH 5.0 parser over mmap or read() with BinaryFILE and MoldUDP64 framing;
  sequence gaps, heartbeats, and trailing bytes are loud errors carrying
  session, sequence, and byte offset.
- L3 limit-order-book engine with price/time priority, self-trade prevention
  modes, and contract-checked capacity bounds.
- Execution simulator: latency pipeline (parse/decision/wire) with jitter
  models, depletion and momentum flow, iceberg reserve slices, NASDAQ-native
  odd-lot (`nd`) and pegged order facilities with automatic repegging.
- Session model: phases, halt gating, NOII close-cross mechanics validated
  against NASDAQ's public NOII sample (5.95M snapshots).
- Trade reconstruction with canonical CSV diff against the official tape.
- Trading calendar: session bounds, early closes, timestamp visibility.
- CRTP strategy harness with columnar telemetry log; tearsheet metrics are
  recomputed from the log alone by independent arithmetic.
- `mog simrun`: scripted simulation with fees, STP, latency, and flags column
  for non-displayed and pegged orders; JSON summary plus per-fill events log.
- `tools/sweep.py`: parameter grid fan-out with p5/p50/p95 quantile bands;
  no-op sweeps must leave every digest identical.
- `tools/calibrate.py`: method-of-moments latency fits from events logs.
- Python shim (`mog` wheel): parse, book, simulator, time-travel debugger,
  cross-language digest parity pinned in CI.
- C++26 frontier profile with native contracts plus a portable C++23/26
  consumer fallback; feature probe generates `docs/COMPATIBILITY.md`.

### Fixed
- Real-capture fidelity: a $1,200 price band and an undersized ladder page
  pool silently starved high-priced names (5,180 unpriced executions, 2.3%
  of prints). Band widened to the full ladder domain, pool defaults raised,
  adaptive sizing added; a real day-prefix now reconstructs 222,968 prints
  with zero unpriced executions.
- One-sided-book mid overflow in simrun; ladder page exhaustion is a loud
  error instead of silent corruption; informative-snapshot rule for close
  crosses.

### Validated
- Ground-truth diff vs the official Nasdaq Last Sale 4.0 tape (free paired
  sample, timestamps identical to the nanosecond): 99.94% exact record
  containment for E/C prints, 94.5% per-price-point volume reconciliation;
  residuals decomposed into report-time aggregation, timestamp drift, and
  tape-only liquidity. See `results/ground-truth/`.
- Four-way throughput race vs hftbacktest and nautilus_trader under the
  documented protocol; results and harnesses archived under `results/race/`.

[Unreleased]: https://github.com/KanakBarfa/mog/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/KanakBarfa/mog/releases/tag/v0.1.0
