# Performance Benchmark Summary (Frontier Profile)

Measured on x86_64 host with g++-15, frontier profile (`-march=native`, `-O3`, `-DNDEBUG`, contract assertions enabled).

## 1. Google Benchmark Results

| Benchmark | CPU ns/iter | Throughput (items/s) | Notes |
|---|---|---|---|
| `BM_BookBestQuery` | 0.309 ns | > 3,230,000,000 / s | Unconditional cached register read |
| `BM_DecodeAddOrderCpp` | 1.71 ns | 583,106,000 msg/s | Fused 32-bit load + branchless dispatch |
| `BM_DecodeAddOrderAsm` | 1.89 ns | 529,598,000 msg/s | Hand-tuned assembly twin |
| `BM_ParseStream` | 20.06 ms / 1M | 52,263,400 msg/s | Full stream decode & dispatch |
| `BM_ParseStreamBaselineKernel` | 21.71 ms / 1M | 48,309,900 msg/s | SSE4 baseline parsing kernel |
| `BM_BookAddRemoveCycle` | 80.4 ns | 24,890,500 ops/s | Full add + remove lifecycle |
| `BM_BookMixedFeed` | 217.16 ms / 2M | 9,767,570 ops/s | Mixed feed of adds, execs, cancels |
| `BM_NearSorted_Heap/262144` | 53.03 ms | 4,943,050 items/s | Priority queue heap insertion |
| `BM_NearSorted_Wheel/262144` | 53.64 ms | 4,887,490 items/s | Timing wheel scheduled queue |
| `BM_PushPopSteady/65536` | 20.41 ms | 3,210,970 items/s | Steady-state scheduler push/pop |
| `BM_DrainSorted/262144` | 97.32 ms | 2,693,520 items/s | Full priority queue drain |
| `BM_ShuffledDrain_Heap/131072` | 54.54 ms | 2,403,380 items/s | Shuffled event heap drain |
| `BM_ShuffledDrain_Wheel/131072` | 54.57 ms | 2,401,910 items/s | Shuffled event wheel drain |
| `BM_WorstCaseSift/32768` | 29.80 ms | 1,099,510 items/s | Adverse scheduler sift operations |
| `BM_CancelHeavy/32768` | 26.92 ms | 912,777 items/s | Heavy order cancellation queue |

## 2. Callgrind Instruction Meter

| Target | Instructions Retired | Operations / Messages | Instructions / Unit |
|---|---|---|---|
| ITCH 5.0 Decode + Trace Hash | 80,591,174 Ir | 1,048,576 messages | **76.8 Ir / msg** |
| PriceLadder & OrderBook Mutation | 32,487,224 Ir | 171,401 successful ops | **189 Ir / op** |

## 3. Four-Way Throughput Race

| Participant | Throughput (ops/s) | Self-Deterministic |
|---|---|---|
| **mog (native C++ frontier)** | **715,920 ops/s** | yes |
| **mog (python shim)** | **61,122 ops/s** | yes |
| hftbacktest (njit baseline) | 1,246,000 ops/s | yes |
| hftbacktest (python-driven) | 391,000 ops/s | yes |
| nautilus_trader (tick ingest) | 133,000 ops/s | yes |

## 4. Book Latency Percentiles

* 100,000 successful operations under strict contract enforcement:
  * `p50`: 51.3 ns
  * `p90`: 195.8 ns
  * `p99`: 456.5 ns
  * `p99.9`: 852.5 ns
