# mog

[![ci](https://github.com/KanakBarfa/mog/actions/workflows/ci.yml/badge.svg)](https://github.com/KanakBarfa/mog/actions/workflows/ci.yml)
[![lint](https://github.com/KanakBarfa/mog/actions/workflows/lint.yml/badge.svg)](https://github.com/KanakBarfa/mog/actions/workflows/lint.yml)
[![PyPI](https://img.shields.io/pypi/v/mog)](https://pypi.org/project/mog/)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

A deterministic, nanosecond-scale L3 limit-order-book backtesting engine in
C++26, with a thin Python shim over the same core.

**Determinism is the product.** Identical inputs produce identical fills,
logs, and digests across runs, languages, and machines. Golden-digest
anchors pin this in CI; the Python shim and C++ tests share the same trace
digests by construction.

## Quickstart

```sh
pip install mog
```

```python
import mog

data = mog.sample_corpus(seed=0xBEEF, count=100_000,
                         price_center=4_000_000, price_span=400_000)

book = mog.OrderBook(arena_capacity=1 << 20, lo_tick=0,
                     hi_tick=12_000_000, page_pool=512)
mog.parse_itch(data, lambda msg: book.apply(msg))
print(book.best_bid(), book.best_ask(), len(book.l2()))
```

Runnable walkthroughs live in [examples/](examples/): a five-minute
quickstart, a quote-straddling market maker with maker-rebate accounting,
a CRTP strategy in native C++, and queue-position inspection. The wheel
test suite runs `examples/python/quickstart.py` on every release build.

## From source

```sh
cmake -B build/frontier -G Ninja -DMOG_PROFILE=frontier -DMOG_BUILD_EXAMPLES=ON
cmake --build build/frontier -j
ctest --test-dir build/frontier
```

| Profile | Dialect | Codegen | Contracts |
|---------|---------|---------|-----------|
| `portable` | C++23 or 26 | baseline ISA, no `-march`, stock toolchain | compiled out |
| `frontier` | C++26 | `-march=native`, `-Werror` | enforced (`MOG_PRE`) |
| `lab` | C++26 | like frontier | experimental reflection probes |

The wheels ship the portable profile; the frontier profile is how the
authors develop and gate changes (its stricter warnings and active
contracts have caught bugs the portable profile passed).

A container image (`ghcr.io/kanakbarfa/mog`, portable profile, stock
toolchain build) is published per release tag. Windows is out of scope:
the engine relies on mmap semantics and C++26 library coverage that
Windows toolchains do not provide; bring an RFC if you need it.

## What is inside

- ITCH 5.0 ingestion over mmap or read(); BinaryFILE, MoldUDP64, and zero-copy
  PCAP/Ethernet-IPv4-UDP framing where sequence gaps are loud errors
- SIMD AVX2/AVX-512 vectorized wire scanning and branchless symbol dispatch
- L3 matching core: price/time priority, STP modes, contract-checked bounds,
  and hierarchical 2-level bitmasks for O(1) price ladder traversals
- Execution simulator: parse/decision/wire latency pipeline with jitter,
  depth-decay power-law queue depletion, icebergs, NASDAQ-native odd-lot (`nd`),
  and pegged facilities (midpoint, primary, market) with automatic repegging
- Session model: phases, halt gating, NOII Opening, Closing, and Halt cross auctions
- Multi-threaded lock-free parallel grid runner for high-throughput parameter sweeps
- Hardware PMU / Linux perf event counters (IPC, branch misses, L1D cache misses)
- Columnar telemetry with native Zstd/Snappy Parquet sinks and Apache Arrow export
- Quantitative metrics: Hasbrouck Information Share and cross-asset lead-lag analysis
- Trade reconstruction plus canonical CSV diff against the official tape
- CRTP strategy harness, columnar telemetry, tearsheet recomputed from the
  log alone by independent arithmetic
- CLI: `mog replay / trades / simrun / tearsheet`; tools for sweeps with
  quantile bands and latency calibration
- Trading calendar with session bounds and early closes

## Ground truth

Claims are checked against official files, not vibes:

- **Official-tape diff**: paired ITCH 5.0 + Nasdaq Last Sale 4.0 captures
  opening at the identical nanosecond give **99.94% exact record
  containment** for execution prints and **94.5% per-price-point volume
  reconciliation**; residuals decompose into report-time aggregation
  (verified), timestamp drift, and tape-only liquidity.
  See [results/ground-truth/](results/ground-truth/).
- **Real-capture reconstruction**: a full morning prefix replays 222,968
  prints with zero unpriced executions after two silent capacity caps were
  found and fixed (regression-pinned).
- Method notes and falsified hypotheses stay archived next to the numbers;
  negative results are recorded, not hidden.

## Throughput

Four-way race on the scripted market-maker workload
([protocol and raw data](results/race/), i5-7500T class host; mog rows
re-measured 2026-09-05, competitors are their Round-1 runs):

| Participant | Events/s | Deterministic |
|---|---|---|
| mog (native C++, golden digest, audits on) | 2,281,000 | yes |
| hftbacktest (njit) | 1,246,000 | yes |
| hftbacktest (python callbacks) | 391,000 | yes |
| mog (python shim) | 590,000 | yes |
| nautilus_trader | n/a | upstream assert (their bug, documented) |

Honest reading: mog's proofs-on arm leads njit-compiled hftbacktest by
~1.8x on this workload with zero JIT warmup, at 2.28M events/s carrying
exact per-op queue recomputation, conservation audits, decision
pipelines, STP checks and SHA-256 trace hashing inline. The climb from
164k took two internal passes (incremental conservation, incremental
queue-ahead: RFC-003 A1/A2), each behavior-proven by per-order exact
agreement and green anchors. Every arm proved determinism before its
number counted, and both mog arms agree per-order exactly (16,123
orders, exact qty+price). An earlier 3.7M mog number measured an
end-of-run drain cadence that never let quotes rest intraday; it is
retracted in the correction note, not hidden.

## Documentation

- [Fidelity guide](docs/site/fidelity-guide.md): what is modeled, what is not
- [Model bias reference](docs/site/model-bias-reference.md): known wrongness, honestly
- [Benchmark methodology](docs/site/benchmark-methodology.md)
- Design notes under [docs/](docs/); ranked doctrine and budgets in [DESIGN.md](DESIGN.md)

## Contributing

Read [CONTRIBUTING.md](CONTRIBUTING.md). Engine-behavior changes require an
[RFC](docs/rfcs/000-template.md); determinism anchors are part of review.
Security issues go through [SECURITY.md](SECURITY.md).

## Citation

If mog contributes to your research, please cite it
([CITATION.cff](CITATION.cff)):

```bibtex
@software{mog,
  author = {Barfa, Kanak},
  title = {mog: deterministic L3 limit-order-book backtesting engine},
  version = {0.1.0},
  url = {https://github.com/KanakBarfa/mog}
}
```

## License

MIT.
