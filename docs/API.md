# Strategy API & Telemetry (M5)

How to plug a strategy into the simulator, stream its telemetry to disk, and
recompute every reported number independently. Everything here is exercised
end-to-end by `tests/strategy_e2e.cpp`; the walkthrough below uses that
scenario's exact numbers.

## The four hooks

Derive from `mog::Strategy<Derived>` and override what you need. Dispatch is
CRTP - there is no virtual call anywhere on the hot path, and unoverridden
hooks fall back to no-ops.

```cpp
class MyStrategy final : public mog::Strategy<MyStrategy> {
public:
    void on_order_book_update(const mog::BookUpdate& u);  // top-of-book changed
    void on_trade(const mog::SimTrade& t);                // external execution printed
    void on_order_ack(const mog::OrderAck& a);            // a submission reached outcome
    void on_order_fill(const mog::OrderFill& f);          // a resting order was filled
};
```

Payload semantics worth internalizing:

- **BookUpdate** fires when the best bid or ask changes, not on every
  internal mutation. Quantities are the displayed size at the touch.
- **SimTrade** prints external executions against resting depth, with the
  aggressor side inferred from which side of the book lost quantity.
  Strategy sweeps do not print trades - they are not external flow.
- **OrderAck** carries the decision outcome (`kind`, mirroring
  `SimDecision::Kind`) plus, for aggressive legs, an exact integer
  `notional` = sum of price*qty over the sweep. No averaging anywhere:
  accounting from acks alone is lossless.
- **OrderFill** reports passive fills of resting strategy orders with their
  exact price. Aggressive and passive executions are disjoint events - an
  aggressive fill never appears as a fill report - so accounting from both
  streams never double counts.

## Driving: StrategyRunner

`mog::StrategyRunner<S>` owns an `ExecutionSimulator` and your strategy,
advances time, and pumps hooks in exact causal order:

```cpp
mog::SimConfig cfg{};
cfg.parse_latency_ns = 200;
cfg.decision_latency_ns = 300;
cfg.wire_latency_ns = 150;

mog::StrategyRunner<MyStrategy> runner(cfg, MyStrategy{});
runner.strategy().bind(runner);              // give the strategy its handle
runner.submit(inbound_order);                // stamped at runner.now()
runner.advance_time(500);                    // external flow + due decisions
```

After each step the runner merges the simulator's queued fills, decisions,
and trade prints by their global sequence numbers and dispatches hooks in
the order the events actually occurred, regardless of batching granularity.
Delivery is a deterministic function of the simulation.

The book update hook fires after decisions drain but before the other
events of the same step are delivered, reflecting one rule: the strategy
reacts to the world as it is now, and learns afterward what its earlier
orders did.

## Telemetry: ring, logger, columnar file

Events leave the core through a bounded SPSC ring
(`mog::SpscRing<T, N>`, trivially copyable slots) and land in a
`mog::ColumnLogWriter`. One envelope struct per pipeline keeps the ring
single-type:

```cpp
struct TelemetryRec {
    std::uint8_t kind;   // book / trade / ack / fill
    BookUpdate b{};
    SimTrade t{};
    OrderAck a{};
    OrderFill f{};
};
mog::SpscRing<TelemetryRec, 4096> ring;      // ~700 KB; size for burst depth
```

A background thread pops and appends; a synchronous mode pops inline after
each step. Both must produce logically identical logs - the E2E test runs
the identical scenario through both paths and compares per-stream content.

### Schemas cannot drift

Every record type declares its fields once:

```cpp
struct MidSample {
    std::uint64_t ts = 0;
    std::int64_t mid2 = 0;
    static constexpr auto mog_fields =
        std::tuple{mog::field("ts", &MidSample::ts),
                   mog::field("mid2", &MidSample::mid2)};
};
```

Writers, readers, schema tables, and the Python tool all derive from that
one declaration via `mog::for_each_field`. A reader refuses streams whose
stored fingerprint differs from its own reflected schema
(`schema_matches<S>()`), so layout changes are caught at open, not as
silent garbage.

### File format

Little-endian throughout. Header magic `MOGLOG\x01\x00`, stream count, then
per-stream schema blocks (names + column type codes). Data follows as row
groups: `u32 stream index, u32 row count`, then each column's bytes
contiguously. Row groups are batching units only; readers concatenate them,
so grouping may vary with producer scheduling without changing content.

Inspect any file without the C++ tree:

```
python3 tools/moglog.py info  run.bin
python3 tools/moglog.py dump run.bin fills --format csv
python3 tools/moglog.py dump run.bin acks  --format json
```

Parquet: the tool has no numpy/pyarrow dependency by design; converting a
dump is two lines under any environment that has pyarrow
(`pd.read_csv(...).to_parquet(...)` or the arrow IPC writer). The native
columnar format is the source of truth; Arrow export is a consumer concern,
gated on the consumer's dependencies.

## Metrics

`mog::Metrics.hpp` provides three instruments, all deliberately dumb
arithmetic over explicit event streams so third parties can recompute them.

- **Account** - cash/inventory from fills (`on_fill` at a price, or
  `on_aggressive_fill` with a raw notional), equity against a mark, peak
  drawdown via `observe_equity`.
- **Markouts** - feed `(ts, doubled_mid)` samples and execution records;
  mean signed distance from fill price to the first mid at or after
  T+1us/T+10us/T+100us. Executions beyond the last sample simply do not
  count, rather than extrapolating.
- **TickHistogram** - rdtsc latency buckets (powers of two from 16 ticks).
  Bucket maxima go through an atomic CAS max ("fetch_max watermarks"), so a
  monitor thread can read worst cases without locks while the measured core
  never leaves its fast path.

## Walkthrough: the scripted MM scenario

Seeded book: bids 990x50 and 985x50, asks 1010x50 and 1016x50. Zero jitter,
latencies parse+decision+wire = 650ns total stamp. The strategy quotes
mid +/- 2 for size 10 on every top-of-book change.

1. **t=0**: submit market buy 30. Decision lands at t=500, sweeps 30 of the
   1010 offer. Ack: kind=filled, filled=30, notional=30300, visible stamp
   650. Account: cash -30300, inventory +30. The strategy requotes around
   mid 1000 -> bids 998, asks 1002.
2. **t=1000**: the requote pair rested at t=950, so the touch is now our own
   998/1002. External sellers exhaust the old 1010 level (trade print, 20
   lots, aggressor buy). The hook rebids around the unchanged mid; between
   cancel and re-rest the touch transiently shows 990/1016 - this gap is
   real cancel/replace behavior, not noise.
3. **t=1500**: replacement quotes rested. External selling lifts our 998
   bid: passive fill report, 10 @ 998, stamp 1650. Account: cash -9980,
   inventory +40.
4. **t=2000..2500**: external buying erodes 990x50 away; the mark moves
   against our inventory and drawdown goes positive.
5. Settle. Final: inventory 40, cash -40280, one fill report, three trade
   prints, four book updates.

Verification (all inside `strategy_e2e.cpp`):

- Hand-computed values above asserted directly.
- An independent recomputation replays the LOG FILE - ack notionals plus
  fill reports through separate arithmetic - and must reproduce the live
  account exactly, including drawdown over the logged equity curve.
- Markouts recomputed from logged mids: exec A sees a mid-requote 1003 at
  T+1us (-7), the settled 996 at longer horizons (-14); exec B sees 996
  (-2); means -4.5/-8/-8, all exact integer-halves.
- Threaded-logger run must match the synchronous run stream-for-stream.

## Scope notes

- Latency stamps are simulated nanoseconds, deterministic by construction;
  rdtsc histograms measure host-side costs separately and never mix into
  simulated timestamps.
- The debug allocator guard (`mog_alloc`) applies to replay-loop targets;
  the telemetry path allocates only outside the deterministic core (ring
  slots are preallocated, log buffers grow on the consumer side).
