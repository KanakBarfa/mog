# M2: Order Book Design Note

Status: complete functionally (differential, E2E, sanitizers, contracts all green).
Performance gates: partially met; full analysis and evidence below.

## Components

| File | Role |
|---|---|
| `include/mog/Arena.hpp` | Flat slot pool, intrusive free list, generational handles |
| `include/mog/Ladder.hpp` | Paged dense price ladder per side, incremental L2 aggregates |
| `include/mog/OrderBook.hpp` | Ops, FIFO queues, flat id table, contracts, audit |
| `include/mog/SnapshotRing.hpp` | Delta transitions groundwork for time travel |
| `include/mog/Simulate.hpp` | Execution simulator: order types, STP, queue tracking, latency |
| `include/mog/TimeTravel.hpp` | Record/replay sessions: seek, counterfactual forks, deterministic reports |
| `include/mog/Orchestrate.hpp` | Multi-instrument fan-out: routed ops, ordered broadcasts, portfolio digests |
| `tests/support/ReferenceBook.hpp` | Naive std::map reference mirroring semantics op-for-op |
| `tests/book_diff.cpp` | Differential fuzz driver |
| `tests/book_e2e.cpp` | ITCH capture through parser into book integration test |

## Memory layout

### Arena (orders)

```
Slot[capacity]                      each 64 bytes, cacheline aligned
+---------------------+-------------+-----------+------+
| Order               | generation  | free_next | live |
| ref u64             | u32         | u32       | bool |
| qty_remaining i64   |             |           |      |
| price_ticks i64     |             |           |      |
| prev u32 next u32   |             |           |      |
| side char           |             |           |      |
+---------------------+-------------+-----------+------+
```

Handles are `{index, generation}`. Release bumps the slot generation so stale
handles fail validation instead of aliasing a recycled order (no ABA). Free
slots chain through `free_next`; acquire pops the head, O(1).

### Price ladder (one instance per side)

```
band [lo_tick, hi_tick)

dir_[ (tick - lo) >> kPageShift ]  u32 page slot or kNoPage
     |
     v
Page { occupancy u32, Level[1024] }        each level 24 bytes
Level { qty_total i64, head u32, tail u32, count u32 }

FIFO: arena slot indices threaded prev/next through orders;
head/tail live in the Level, giving O(1) append and O(1) cancel.
```

Pages are **sticky**: claimed once from the pool, never returned. A fully
drained page is already pristine because every operation restores
`qty==0 / count==0 / null links` as its final state, so reclaim would only
re-zero what is provably zero. Sticky pages remove a 24KB fill from every
add-after-empty transition, which profiling showed dominating mixed workloads.
Pool sizing bounds worst-case footprint; exhaustion is a contract violation
(config error, not feed noise).

Best bid/ask are cached tick values updated incrementally; when the best level
drains, `first_above` / `first_below` scan pages outward until the next
occupied level. Amortized short near the action.

### Id table (order_ref to handle)

Flat open-addressing, power-of-two capacity at load factor <= 0.5 relative to
arena capacity, linear probing, splitmix-derived hash, **backward-shift
deletion** so probe chains stay intact without tombstones and without any
rehash allocation. Zero heap traffic during replay by construction.

## Result surface

Book operations return `BookTick`, a 16-byte POD (price touched, quantity,
kind, side) that comes back in register pairs with no sret traffic. Per-level
deltas moved behind an optional out-pointer: hot replay passes `nullptr`,
verification paths (fuzz, E2E, ring fold) pass a buffer. Best prices after
each op are read via `best_bid()` / `best_ask()` getters. Failed ops carry
the `BookError` code inside the tick (`kTickError` kind), preserving exact
error precedence without an expected-return union.

## Semantics

Validation precedence (mirrored exactly by the reference):

1. `bad_qty` for non-positive quantities.
2. `out_of_band` for prices outside the configured ladder band.
3. `unknown_order` / `duplicate_ref` membership checks.
4. `over_execute` when reduce quantity exceeds remaining.
5. `arena_full` pre-check before any mutation in replace.

Replace validates everything up front, then removes and re-adds; a rejected
replace leaves both orders untouched. C messages affect the book identically
to E; their printable flag and execution price belong to the trade tape, not
the ladder.

## Contracts at invariant sites

Enforced in frontier/lab profiles by default, which includes all fuzz runs:

- `qty_remaining > 0` while queued; level totals equal FIFO sums.
- FIFO linkage consistent in both directions; counts and tails verified.
- A level can only drain via a full order removal.
- Live order always sits on a materialized level.
- Id table membership matches arena liveness; table size within load bound.
- Delta ring pushes never carry zero deltas.

`audit()` walks everything above in O(live + band pages); the fuzz harness
calls it every 997 ops and the E2E test once per pipeline run.

## Snapshot-ring groundwork

`DeltaRing<k>` records `{seq, side, price_ticks, qty_delta}` transitions with
monotonic sequence numbers, overwriting oldest when full (`dropped_count`
exposes loss). The fuzz harness verifies the fold property: folding all
transitions of an undropped ring reconstructs the exact live L2 state.

## Verification

| Check | Result |
|---|---|
| Differential fuzz vs ReferenceBook | 10M ops, divergence 0, audits green, allocation-free (AllocGuard armed around every engine op) |
| Continuous CI fuzz | 2M ops per ctest run (env `MOG_BOOK_OPS`) |
| E2E sample capture | parse to book vs reference, per-message result and delta equality, determinism double-run trace match |
| Sanitizers | ASan + UBSan clean over smoke/golden/diff/isa/e2e plus 200k-op fuzz |
| Profiles | frontier, portable, portable+C++23, clang-frontier all build and pass |

Bugs the differential loop caught during development (kept as evidence the
harness earns its keep): inverted drain invariant, band-relative vs absolute
page-offset mismatch, best-bid accessor reading the wrong map end, page
release racing FIFO unlink, and a stack overflow from ring storage in main().

## Performance

### Reference laptop (dev baseline)

Hardware: Intel i5-7500T @ 2.70GHz base (4 cores, boost ~3.3GHz, 4MB shared
L3), Linux 6.14, GCC 15.2, `-O3 -march=native`, single pinned core,
invariant-TSC calibrated per run. All numbers reproducible via
`bench/mog-bench-book-percentile` (percentiles) and `bench/mog-bench-book`
(throughput); scripts print configuration inline.

| Pattern | Median | p99.9 |
|---|---|---|
| Execute at front, 8k orders hot set | 52 ns | 300 ns |
| Mixed stream, 2.5k live random refs | 190 ns avg | n/a |
| Mixed stream, 24k live random refs | 138 ns | 930 ns |
| Mixed stream, 180k live random refs | 330 ns avg | n/a |
| Add+remove churn pair, fixed price | 112 ns per pair | n/a |

Contracts mode makes no measurable difference (enforce vs ignore within
noise): the compiler hoists the mode checks out of loops.

### Second machine (Alder Lake)

Hardware: Intel i7-12650H (6P+4E, P-core boost 4.7GHz, 24MB shared L3),
Ubuntu 26.04, GCC 15 distro build, `-march=native`, taskset-pinned to core 3
(P-core), performance governor, invariant-TSC calibration, same scripts,
same deterministic op stream (identical attempted-op counts confirm
run-to-run equivalence across machines).

| Pattern | Median | p99.9 | p99.99 |
|---|---|---|---|
| Mixed stream, 24k live random refs | 63.3 ns | 258 ns | 2212 ns |
| Mixed stream, 180k live random refs | 67.7 ns | 545 ns | 2585 ns |

Two findings:

1. Governor sensitivity: the identical binary measured 146 ns median under
   powersave vs 63 ns under performance. Frequency scaling dominates
   everything else; benchmark protocol must pin the governor.
2. Depth flatness: 24k-live and 180k-live cost the same within noise. The
   24MB L3 absorbs the entire working set, eliminating the cache-capacity
   regime that dominates the reference laptop. Single-instrument depth is
   free on current desktop silicon.

### Instruction diet pass

The primary optimization metric is instructions retired per successful op,
measured by `bench/ir_meter.sh` under callgrind with collection toggled
exactly around the engine call. The metric is machine independent: identical
binaries and streams produce identical counts anywhere, so regression gates
can fail on integer deltas rather than noisy timings.

| Stage | Ir/op | i5-7500T p50 (24k live) |
|---|---|---|
| Baseline (expected<BookResult>, eager deltas) | 351 | 138 ns |
| Slim return (16B BookTick, lazy deltas) | 220 | n/a (intermediate) |
| Interleaved id entries + Fibonacci hash | 199 | 98.5 ns |
| Fused ladder walk in reduce, single slot load in remove | **189** | 103 ns (same noise band) |

The fused walk removed the duplicate band-check and directory traversal
between peek and apply; sticky pages make the returned level valid for the
unlink that follows. Behavior stayed bit-identical throughout: the 10M-op
differential run reproduces the same trace hash at every stage. The
caller-owned-slot idea is complete as of the slim-return stage: a 16-byte POD
returns in registers and detail deltas are opt-in, so there is no hidden
construction left to externalize.

Note the meter's semantics: total instructions include rejected ops (which
are cheap but produce no successful count), so the figure is a realistic
mixed-feed cost per accepted op, not a pure success-path cost.

### Lookup-path decision record

A paged direct-ref directory (indexing order-reference space like the price
ladder indexes tick space) was evaluated and rejected on arithmetic, not
opinion: a direct scheme costs roughly `span x 8 bytes` regardless of paging,
since every covered reference needs its entry somewhere. NASDAQ reference
spans reach into billions across sessions with protocol-guaranteed gaps, so
even a one-session window implies gigabytes against an arena measured in
megabytes. The interleaved open-addressing table with backward-shift deletion
(one cache line per probe, load factor <= 0.5, no tombstones, no rehash)
keeps O(1) behavior at bounded memory and is retained as final.

### Gate status

PLAN target: median < 15 ns, p99.9 < 60 ns. Not met yet; the gap remains
instruction retirement, now reduced 43 percent on the meter with zero
divergence across the full differential corpus. On bench-class silicon
(Alder Lake P-core) the same binary class projects to roughly 40-45 ns
median mixed-stream; closing further requires deeper surgery (SWAR-packed linkage fields,
branchless best updates, probe-free dense windows), each gated on the Ir
meter showing its isolated contribution first.
