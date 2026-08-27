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

- ITCH 5.0 ingestion over mmap or read(); BinaryFILE and MoldUDP64 framing
  where sequence gaps are loud errors, never silent corruption
- L3 matching core: price/time priority, STP modes, contract-checked bounds
- Execution simulator: parse/decision/wire latency pipeline with jitter,
  flow depletion and momentum, icebergs, NASDAQ-native odd-lot (`nd`) and
  pegged facilities with automatic repegging
- Session model: phases, halt gating, NOII close crosses (validated against
  NASDAQ's public NOII sample, 5.95M snapshots)
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
([protocol and raw data](results/race/), AWS c7g.x86 class host):

| Participant | Events/s | Deterministic |
|---|---|---|
| mog (native C++) | 720,938 | yes |
| hftbacktest (njit) | 1,258,662 | yes |
| hftbacktest (python callbacks) | 387,433 | yes |
| mog (python shim) | 11,742 | yes |
| nautilus_trader | n/a | upstream assert (their bug, documented) |

Honest reading: on this workload njit-compiled hftbacktest posts the
highest number; mog's native arm leads its own python shim by ~60x and
beats callback-driven hftbacktest without any JIT warmup. Every arm was
required to prove determinism before its number counted.

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
