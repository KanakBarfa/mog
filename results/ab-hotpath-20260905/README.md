# Hot-path A/B, 2026-09-05 (portable profile, g++-15, 4-core 6 MiB L3 box)

Method: true baseline from a pristine `HEAD` worktree (separate build dir so
absolute include paths cannot leak across sides); current tree measured with
identical flags. Ir/op via `bench/ir_meter.sh` (callgrind, deterministic).
Latency via `mog-bench-book-percentile` (3 runs/side). Construction via
`/usr/bin/time -v` on a default full-day `OrderBook` (2 runs/side). Sim
probes are scratch microbenches (2000 tracked orders, digest-compared).

## Kept (general: fewer instructions/faults on any hardware)

| Change | Baseline | Now | Notes |
|---|---|---|---|
| Full-day book construction | 1.09 s, 1.99 GB RSS, 497k faults | 0.92 s, 1.73 GB RSS, 432k faults | lazy slot alloc (Arena/Scheduler/TimingWheel) |
| Sim consume, 2000 tracked x 200 chunks | 5.3-5.8 ms | 1.8-1.9 ms cumulative | conservation gate + slot-indexed recompute |
| Rest 5000 stacked orders, one level | 306 ms pristine (63 ms at 1a91b55) | 2.8 ms | O(1) tail-append queue position; values bit-identical |
| Take-dominated sim flow, 50k unit consumes | baseline | -8% | handle threading kills 2 probes per take |
| Isolated partial front-match, 500k takes | 6.7-7.1 ms (13.7 ns/take) | 4.8-4.9 ms (9.8 ns/take) | probeless FIFO-head handle |
| Book mutation Ir/op | 188 | 189 | neutral |
| Book p50 / p99.9 | ~81 ns / ~844 ns | ~81 ns / ~846 ns | neutral |
| Trace digests | equal | equal | bit-identical behavior on all probes |

## Reverted (falsified here, not hidden)

| Idea | Result | Verdict |
|---|---|---|
| Lazy page zeroing on claim | ctor 1.09 s to 0.08 s, but Ir/op 188 to 247 (+31%), p50 +7% | moved work from unmeasured ctor into the measured hot path; replays that fill the pool pay more overall |
| 3-level hierarchical directory bitmaps | Ir/op 188 to 202 (+7%), p50 +7%, p99.9 flat on dense book | per-transition maintenance taxes every op; savings need sparse books this workload never shows |
| Order-carried id-table slots | Ir/op 188 to 209 (+11%) on replay churn | erase-cluster maintenance costs more than the saved probes outside sim match paths |

## Not attempted (ISA/OS-specific, kept out of the portable core)

AVX2/AVX-512 parse kernels, SHA-NI digests, huge-page policy, 32-byte order
packing (needs an RFC: changes the public quantity/price width contract).
