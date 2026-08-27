# Getting started

MOG is a deterministic L3 limit-order-book backtesting engine. This page
takes you from a clean machine to a full experiment: build, verify, and
replay - either in C++ or through the Python shim.

## Prerequisites

- CMake ≥ 3.28 and Ninja
- A C++ compiler:
  - **C++26 path** (frontier/lab profiles): GCC ≥ 14 or Clang ≥ 19
  - **C++23 path** (portable profile): any stock distro g++/clang - this is
    the consumer-fallback dialect and what wheels are built from
- Python ≥ 3.10 with development headers (only for the shim/wheel)
- Optional: libbenchmark-dev (benches), perf (counter audits)

## Build and test (C++)

```sh
cmake -B build/frontier -G Ninja -DMOG_PROFILE=frontier   # native tuning
cmake --build build/frontier -j
ctest --test-dir build/frontier                           # 15 tests
```

Profiles:

| Profile   | Dialect | Codegen                  | Contracts        |
|-----------|---------|--------------------------|------------------|
| `portable`| 26 or 23| baseline ISA, no -march  | compiled out     |
| `frontier`| 26      | `-march=native`/`MOG_ARCH`, `-Werror` | enforced |
| `lab`     | 26      | like frontier            | experimental     |

The deterministic corpus generator and replay CLI:

```sh
./build/frontier/tools/gen-sample/mog-gen-sample --seed 0xBEEF \
    --count 10000 --price-center 4000000 --price-span 400000 capture.itch
./build/frontier/mog replay capture.itch
```

```text
file            capture.itch (314303 bytes)
messages        10000 decoded, 0 skipped
book events     7142 failed
final book      live=2858 best_bid=4399727 best_ask=4000017
digest          0x718f399569707a73
elapsed         19.9 ms (0.5 Mmsg/s)
```

Replay streams the file through `mmap(MAP_POPULATE)` (a plain `read()`
with `--no-mmap`), decodes the order-lifecycle subset into an L3 book,
and skips other standard ITCH 5.0 types (System Event, Trading Action,
NOII, Trade prints, ...) with per-type counts reported - skipping is
explicit and auditable, never silent. The digest is identical whether
bytes arrive via mmap, read(), or MoldUDP64 framing.

```text
flags:   --mold      input is MoldUDP64-framed; sequence gaps are fatal
         --lenprefix NASDAQ BinaryFILE framing (their public samples)
         --no-mmap   read() instead of mmap
         --json      one machine-readable summary line
         --arena N --pool N   size orders/ladder pages for full days
exit:    0 ok · 4 io error · 5 parse error (byte offset printed)
         6 moldudp64 sequence gap (session, expected/got seq printed)
         7 empty stream
```

`book events failed` counts messages the strict book rejected (e.g.
executes against unknown refs). Synthetic corpora contain such noise by
design; real captures should show ~0 - anything else means your framing
or day boundary is wrong, and the count makes that visible instead of
hiding it.

## Scripted simulation, sweeps, tearsheets

`mog simrun` drives the execution simulator over a script CSV (the same
format the cross-engine race uses: `kind,ts_ns,side,price_ticks,qty,ref`
with `ext_add`/`trade`/`strat_limit`/`strat_ioc`/`strat_cancel` rows):

```sh
./build/frontier/mog simrun script.csv --seed 7 --depletion 0.5,0.3 \
    --latency-ns 200 --jitter uniform:100 --maker-fee-bps -2 \
    --taker-fee-bps 5 --events-csv events.csv
```

An optional seventh `flags` column selects NASDAQ-native order behavior:
`nd` rests a non-displayed odd-lot order; `mid`, `bid:N`, `ask:N` rest
pegged orders that automatically reprice with their reference (pegged
facilities are non-displayable by rule) and execute only when flow
crosses them - time priority is preserved across repegs.

The JSON line carries fills, volume, fees and the trace digest; the events
log (one row per fill/print with mid marks and maker/taker ledger tags) is
what `mog tearsheet` recomputes - equity/drawdown, cash vs marked PnL,
hit rate, fee attribution, markouts at +1ms/+10ms/+100ms. If a number
cannot be derived from that log, the log is wrong.

```sh
tools/sweep.py --cmd "./build/frontier/mog simrun script.csv --seed {seed} \
    --depletion {dep}" --param seed=1..16 --param dep=0.0,2.0 \
    --bands fills,volume_ticks
```

Sweep fans one process per grid point across cores and aggregates:
min/mean/max per key plus p5/p50/p95 quantile bands for `--bands` keys -
N-seed sensitivity distributions per G10. A no-op parameter sweep must
leave every digest identical; the tool marks that invariant when it holds.

Latency calibration fits jitter parameters from an events log:

```sh
tools/calibrate.py events.csv            # method-of-moments fit
```

## MoldUDP64 captures

Real NASDAQ feeds arrive sequence-numbered. A silently dropped packet
corrupts everything after it, so `--mold` treats continuity as law:

```text
mog: moldudp64 sequence_gap at byte 46940: session 'NASDAQTEST '
     expected seq 1401 got 1601
```

Every packet's start sequence must equal the running expectation;
heartbeats and end-of-session markers are validated too, and trailing
bytes after an end-of-session marker are an error rather than ignored
garbage (`include/mog/MoldUdp.hpp`).

## Python shim

Installable wheels are produced by CI (`wheels-dist` artifacts): manylinux
x86_64 for Linux and dual-arch (arm64 + Intel) for macOS, CPython 3.13/3.14:

```sh
pip install mog-<version>-<tags>.whl
```

Or from source - builds the portable profile with your stock compiler:

```sh
python -m build --wheel          # requires: pip install build
pip install dist/*.whl
```

A complete experiment in twenty lines:

```python
import mog

# 1. Deterministic synthetic capture (or bring real ITCH bytes).
data = mog.sample_corpus(seed=0xBEEF, count=100_000,
                         price_center=4_000_000, price_span=400_000)

# 2. Stream-parse; every record lands in your callback.
book = mog.OrderBook(arena_capacity=1 << 20, lo_tick=0,
                     hi_tick=12_000_000, page_pool=512)

def sink(msg):
    tick = book.apply(msg)          # A/F/E/C/X/D/U routing
    assert tick.ok

consumed = mog.parse_itch(data, sink)
assert consumed == len(data)
print(book.best_bid(), book.best_ask(), len(book.l2()))

# 3. Execution simulation with byte-exact determinism.
sim = mog.ExecutionSimulator(arena_capacity=1 << 20, lo_tick=0,
                             hi_tick=12_000_000, page_pool=512)
sim.seed_external(9, "S", 70, 4_010_000)
sim.submit((1 << 62) + 50, "B", 25, 4_010_001, mog.SimOrderType.day_limit, 5)
sim.drain()
digest = sim.trace_digest()         # SHA-256 over fills+decisions
```

The same script run twice - same inputs, same machine, same anything -
produces the identical `digest`. The Python shim produces digests identical
to the C++ test-suite anchors by construction; that cross-language parity is
pinned in CI (`python/tests/test_mog.py`).

## Where numbers come from

Benchmarks and regression gates follow a written protocol (pinning,
interleaving, medians): see [Benchmark methodology](benchmark-methodology.md).
Counter-audit results ship with hardware context in `results/`.

## Going deeper

- [Fidelity guide](fidelity-guide.md) - what is modeled, and what is not
- [Model bias reference](model-bias-reference.md) - known wrongness, honestly
- [C++26 ledger explainer](cpp26-ledger-explainer.md) - features and fallbacks
- Design notes: `docs/PARSER.md`, `docs/BOOK.md`, `docs/SCHEDULER.md`,
  `docs/API.md`, `docs/FILLMODEL.md`
