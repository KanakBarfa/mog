# Performance Benchmark Summary (Frontier Profile)

Measured 2026-09-05 on i5-7500T (4C, 6 MiB L3), g++-15, frontier profile
(`-march=native`, `-O3`, `-DNDEBUG`, contract assertions enabled). Prior record
archived at `results/perf-20260825.md`.
Competitor rows below are their Round-1 runs, unchanged and not re-run.

## 1. Google Benchmark Results

| Benchmark | CPU ns/iter | Throughput (items/s) | Notes |
|---|---|---|---|
| `BM_BookBestQuery` | 0.317 ns | > 3,150,000,000 / s | cached register read |
| `BM_DecodeAddOrderCpp` | 1.75 ns | 572,984,000 msg/s | fused 32-bit load + branchless dispatch |
| `BM_DecodeAddOrderAsm` | 1.92 ns | 520,267,000 msg/s | hand-tuned assembly twin (loses, kept as harness) |
| `BM_ParseStream` | 20.41 ms / 1M | 51,369,800 msg/s | full stream decode and dispatch |
| `BM_ParseStreamBaselineKernel` | 22.10 ms / 1M | 47,451,200 msg/s | SSE4 baseline parsing kernel |
| `BM_BookAddRemoveCycle` | 74.6 ns | 26,822,400 ops/s | full add + remove lifecycle |
| `BM_BookMixedFeed` | 234.10 ms / 2M | 9,166,810 ops/s | mixed feed of adds, execs, cancels |
| `BM_NearSorted_Heap/262144` | 29.17 ms | 8,995,650 items/s | priority queue heap insertion |
| `BM_NearSorted_Wheel/262144` | 19.04 ms | 13,777,400 items/s | timing wheel scheduled queue |
| `BM_PushPopSteady/65536` | 20.86 ms | 3,142,070 items/s | steady-state scheduler push/pop |
| `BM_DrainSorted/262144` | 82.62 ms | 3,173,430 items/s | full priority queue drain |
| `BM_ShuffledDrain_Heap/131072` | 32.42 ms | 4,046,890 items/s | shuffled event heap drain |
| `BM_ShuffledDrain_Wheel/131072` | 24.54 ms | 5,340,780 items/s | shuffled event wheel drain |
| `BM_WorstCaseSift/32768` | 5.04 ms | 6,506,600 items/s | adverse scheduler sift operations |
| `BM_CancelHeavy/32768` | 1.97 ms | 12,462,000 items/s | heavy order cancellation queue |

## 2. Callgrind Instruction Meter

| Target | Instructions Retired | Operations / Messages | Instructions / Unit |
|---|---|---|---|
| ITCH 5.0 Decode + Trace Hash | 644,729,392 Ir | 8,388,608 messages | **76.9 Ir / msg** |
| PriceLadder & OrderBook Mutation | 33,158,557 Ir | 171,401 successful ops | **193 Ir / op** |

## 3. Four-Way Throughput Race

Same 176,096-op canonical feed, per-action drain in both mog harnesses,
medians over repeated runs with cooldown (Round 5; the Round-4 end-drain
numbers are retracted in `results/proof-overhead-20260905/CORRECTION.md`):

| Participant | Throughput (ops/s) | Self-Deterministic |
|---|---|---|
| hftbacktest (njit baseline, Round 1) | 1,246,000 | yes |
| hftbacktest (python-driven, Round 1) | 391,000 | yes |
| **mog (native C++, golden digest, audits on)** | **2,281,000** | yes (28,257 fills) |
| **mog (python shim)** | **590,000** | yes (28,257 fills) |
| nautilus_trader (tick ingest, Round 1) | 133,000 | yes |

Both mog arms agree per-order exactly (16,123/16,123, exact qty+price).
Competitors were not re-run; nothing in their code changed on our side
to move them.

## 4. Book Latency Percentiles

* 100,000 successful operations under strict contract enforcement:
  * `p50`: 84.1 ns
  * `p90`: 228.6 ns
  * `p99`: 552.0 ns
  * `p99.9`: 855.9 ns
