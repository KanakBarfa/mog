# Chapter 10: Codebase Map

**What you will learn:** what every file in the repository does, grouped
by subsystem so you can navigate by interest. Paths link to the real
source.

---

## 1. The big picture

mog is header-centric: most of the engine lives in `include/mog/` as
headers, keeping the compiled library thin. The layers run in reading
order below - types first, then market data, then the book, then
simulation, then analytics and tooling.

## 2. Foundation: numbers, memory, checking

| File | Role |
|------|------|
| [Types.hpp](../include/mog/Types.hpp) | Fixed-point `Price`/`Qty` integers (ch 01), strong ID types, packed wire structs |
| [Arena.hpp](../include/mog/Arena.hpp) | Cache-line-aligned order arena; generational handles; free list (ch 04) |
| [AllocGuard.hpp](../include/mog/AllocGuard.hpp) | Debug tripwire that aborts if replay allocates (ch 04) |
| [Contracts.hpp](../include/mog/Contracts.hpp) | `MOG_PRE`/`MOG_POST`/`MOG_CONTRACT_ASSERT` guardrails; enforced in CI/fuzz builds, observed in release |
| [Sha256.hpp](../include/mog/Sha256.hpp) | Cryptographic state hashing behind determinism digests (ch 00) |
| [FastDigest.hpp](../include/mog/FastDigest.hpp) | 128-bit vectorized streaming hash behind fast-digest mode (ch 00) |
| [Reflect.hpp](../include/mog/Reflect.hpp) | Compile-time field reflection for structs |
| [Calendar.hpp](../include/mog/Calendar.hpp) | Trading sessions, holidays, epoch math |

## 3. Market data: decoding the exchange feed

| File | Role |
|------|------|
| [Wire.hpp](../include/mog/Wire.hpp) | Safe unaligned big-endian loads (ch 03) |
| [ITCHParser.hpp](../include/mog/ITCHParser.hpp) | Zero-copy ITCH 5.0 decoder (ch 03) |
| [simd/SWAR.hpp](../include/mog/simd/SWAR.hpp) | 8-bytes-at-a-time SWAR decode kernels (ch 06) |
| [IsaDispatch.hpp](../include/mog/IsaDispatch.hpp) | CPUID-based kernel selection at startup (ch 06) |
| [MoldUdp.hpp](../include/mog/MoldUdp.hpp) | MoldUDP64 framing with sequence-gap detection (ch 03) |
| [SessionFeed.hpp](../include/mog/SessionFeed.hpp) | Multi-stream feed aggregation |

## 4. The book: state of the market

| File | Role |
|------|------|
| [Ladder.hpp](../include/mog/Ladder.hpp) | Paged dense price ladder: price to level in O(1) (ch 05) |
| [OrderBook.hpp](../include/mog/OrderBook.hpp) | L3 book: FIFO queues + Fibonacci-hash order lookup (ch 01, 05) |
| [SnapshotRing.hpp](../include/mog/SnapshotRing.hpp) | Ring buffer of book snapshots for rewind |
| [Orchestrate.hpp](../include/mog/Orchestrate.hpp) | Multi-instrument session orchestration on one venue |
| [OUCH.hpp](../include/mog/OUCH.hpp) | OUCH 4.2 binary order entry protocol encoding and decoding |

## 5. Simulation: fills under realistic physics

| File | Role |
|------|------|
| [TimingWheel.hpp](../include/mog/TimingWheel.hpp) | 11-level base-64 hierarchical timing wheel, O(1) scheduling (ch 09) |
| [Scheduler.hpp](../include/mog/Scheduler.hpp) | Binary-heap reference scheduler kept for differential comparison |
| [Simulate.hpp](../include/mog/Simulate.hpp) | Execution simulator: queue position, depletion, latency/jitter, Hawkes bursts, icebergs, pegs, STP (ch 02, 07) |
| [SpscRing.hpp](../include/mog/SpscRing.hpp) | Lock-free single-producer/single-consumer telemetry queue (ch 09) |
| [TimeTravel.hpp](../include/mog/TimeTravel.hpp) | Journal, rewind, and counterfactual forking (ch 09) |

## 6. Strategy & analytics: plugging in and scoring

| File | Role |
|------|------|
| [Strategy.hpp](../include/mog/Strategy.hpp) | CRTP strategy harness (ch 08) |
| [Trades.hpp](../include/mog/Trades.hpp) | Trade-print reconstruction from book events |
| [TradeDiff.hpp](../include/mog/TradeDiff.hpp) | Multiset ground-truth matching vs official tapes (ch 07) |
| [Tearsheet.hpp](../include/mog/Tearsheet.hpp) | PnL layers, maker/taker fees, markout analytics (ch 08) |
| [ColumnLog.hpp](../include/mog/ColumnLog.hpp) | Self-describing zero-allocation binary event log (readable via `tools/moglog.py`) |
| [Metrics.hpp](../include/mog/Metrics.hpp) | Latency histograms and percentile computation |
| [Arrow.hpp](../include/mog/Arrow.hpp) | Zero-copy Apache Arrow and Parquet telemetry export |
| [Replay.hpp](../include/mog/Replay.hpp) | mmap'd full-file replay driver (ch 09); default capacities documented here |
| [SimRun.hpp](../include/mog/SimRun.hpp) | Scenario-run driver wrapping the simulator |

## 7. Entry points

| File | Role |
|------|------|
| [src/main.cpp](../src/main.cpp) | CLI subcommands: `replay`, `trades`, `simrun`, `tearsheet` |
| [python/mog/_core.cpp](../python/mog/_core.cpp) | nanobind shim exposing the engine to Python, including fused `parse_apply` (ch 09) |
| [tools/nls_diff.py](../tools/nls_diff.py), [tools/sweep.py](../tools/sweep.py), [tools/calibrate.py](../tools/calibrate.py) | Ground-truth diffing, parameter sweeps, latency calibration |

## 8. How it was built, in four layers

For contributors wondering how to approach an engine like this, the
dependency order is instructive:

1. **Primitives**: byte loading ([Wire.hpp](../include/mog/Wire.hpp)),
   integer prices ([Types.hpp](../include/mog/Types.hpp)), the arena
   ([Arena.hpp](../include/mog/Arena.hpp)) - nothing else works without
   these being exact.
2. **The book**: ladder, hash table, FIFO queues - pure data structures,
   exhaustively fuzz-tested against a naive reference implementation.
3. **Simulation**: scheduler plus fill modeling on top of the book.
4. **Analytics & bindings**: strategies, PnL, logging, Python - all
   consumers of the layers below.

Each layer was differential-tested against a deliberately naive twin
before anything clever was allowed in (doctrine #4). For the complete
mathematical and Big-O complexity breakdown of each data structure and algorithm,
see [Chapter 11: Core Algorithms & Complexity Reference](11_algorithms_complexity_and_reference.md).
