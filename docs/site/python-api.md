# Python API

The `mog` package is a thin nanobind shim over the portable-profile C++
core. It is deliberately small: parse, book, simulate, debug - with
byte-exact determinism shared with the C++ side.

```python
import mog
mog.__version__, mog.build_info()["profile"]
```

## Deterministic data

### `mog.sample_corpus(seed, count, price_center, price_span) -> bytes`

Deterministic synthetic ITCH 5.0 capture (BinaryFILE framing). Same seed,
same bytes. Cycles the order-lifecycle message types A,F,E,C,X,D,U.

## Parsing

### `mog.parse_itch(data: bytes, sink: Callable) -> int`

Streams `data` through BinaryFILE framing; every decoded message goes to
`sink(msg)`. Returns consumed byte count (always `len(data)` on success).
Raises `mog.MogParseError` (`.offset` attribute) on malformed input.

### `mog.frame_length(type: str) -> int | None`

Wire length of a message type; `None` for unknown types.

### `mog.decode_message(buf: bytes) -> Message`

Decode one framed message without a book.

### `Message`

Attribute accessors: `type`, `locate`, `tracking`, `ts_ns`, plus per-type
fields (`side`, `shares`, `stock`, `price`, `order_ref`, `executed_shares`,
`match_number`, `execution_price`, `cancelled_shares`, ...).

### `mog.trace_hash(msg, state) -> int`

FNV-1a over a message for cheap stream fingerprinting.

## Order book

### `mog.OrderBook(arena_capacity=1<<20, lo_tick, hi_tick, page_pool=512)`

L3 price ladder with price/time priority and contract-checked bounds.

| Method | Notes |
|---|---|
| `add(ref, side, qty, px) -> BookTick` | `'B'`/`'S'` side |
| `execute(ref, qty)`, `execute_with_price(...)` | partials allowed |
| `cancel(ref, shares)` / `remove(ref)` | reduce or delete |
| `replace(orig, fresh, qty, px)` | price-time preserving |
| `apply(msg) -> BookTick` | route one decoded ITCH message |
| `best_bid()`, `best_ask()` | `int` or `None` when empty |
| `l2() -> list[BookTick]` | aggregated ladder snapshot |
| `qty_at(side, px)`, `remaining_of(ref)`, `in_band(tick)` | inspection |
| `live_orders() -> int` | resident order count |
| `audit()` | structural consistency report |

## Execution simulator

### `mog.ExecutionSimulator(...)`

Constructor knobs beyond the book's: `event_capacity`, `seed`,
`parse_latency_ns` / `decision_latency_ns` / `wire_latency_ns`,
`jitter_kind` + shape params, depletion/momentum flow models,
`external_ref_limit` (strategy refs live above it), maker/taker fees in bps.

Scripting:

- `seed_external(ref, side, qty, px)` - rest background liquidity
- `seed_iceberg(ref, side, px, display, total)`
- `submit(ref, side, qty, px, type, arrival_ts)` - strategy order;
  types are `mog.SimOrderType.day_limit`, `.ioc`, `.market`,
  `.midpoint_peg`, `.primary_peg`, `.market_peg`
- `cancel_strategy(ref)`, `replace_strategy(orig, fresh, qty, px)`
- `apply_external(side, px, qty)` - pressure against that resting side;
  consumes queue-first at and behind `px`
- `advance_time(dt_ns)`, `run_until(ts)`, `drain()`

Inspection:

- `queue_ahead_of(ref) -> int` - shares ahead of a resting order (-1 if gone)
- `reports() -> list[SimFillReport]` (maker fills), `trades()`,
  `decisions()`
- `book()` - the underlying `OrderBook`
- `trace_digest() -> str` - SHA-256 over fills+decisions; identical inputs
  give identical digests across languages and machines
- `set_stp_mode(mog.StpMode...)`, `audit()`

## Microstructure & Cross-Asset Metrics

### `mog.compute_lead_lag(series1, series2, max_lag) -> LeadLagResult`

Computes sub-window normalized Pearson cross-correlation across lag offsets
$[-K, +K]$. `optimal_lag > 0` indicates series 1 leads series 2.

### `mog.compute_hasbrouck_share(var1, var2, cov12) -> HasbrouckShare`

Calculates bivariate Hasbrouck Information Share upper, lower, and midpoint
bounds using Cholesky factor covariance matrix rotation.

## Parquet & Arrow Export

### `mog.to_parquet(obj, path, compression="zstd")`

Exports columnar telemetry (`LogFile`, `ColumnLogReader`, `TradesSummary`,
or PyArrow RecordBatches) directly to compressed Parquet files with zero
intermediate CSV conversions.

## Time-travel debugging

### `mog.TimeTravelSession(...)`

Same constructor surface as the simulator plus rewind/replay support for
post-mortem analysis; see the [time-travel debugger](time-travel-debugger.md)
page.

## Errors

`mog.MogParseError` (with `.offset`) and `mog.MogDecodeError`.

## Cross-language parity

The parser-golden hash and simulator trace digests pinned by the C++ test
suite are pinned identically in `python/tests/test_mog.py`; CI fails if the
shim ever diverges from native replay.
