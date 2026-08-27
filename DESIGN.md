# Design doctrine

mog is a deterministic L3 limit-order-book backtesting engine. Determinism IS
the product: identical input produces bit-identical output, asserted in CI,
hashed into every result file. This page holds the ranked principles, the
engineering budgets, and the scope boundary. Evidence lives under `results/`.

## Doctrine

Ranked principles. When they conflict, the higher number loses.

1. **Correctness before speed before features.** A fast wrong answer is worse than a slow right one.
2. **Every approximation is explicit.** Each model documents its assumptions, bias direction, and magnitude bounds. Silent shortcuts are bugs.
3. **Determinism is absolute.** Same input, bit-identical output, every time.
4. **No cleverness without proof.** Hand-written asm needs a >= 15% measured win and a pure-C++ twin kept bit-identical by tests. Exotic structures need a profile. Language-frontier features obey the same rule.
5. **Reproducibility is the product.** Every published number ships with hardware specs, dataset, command line, and script. If a claim cannot be rerun, it is not a claim.
6. **Boring interfaces, exotic internals.** Users get plain structs, callbacks, and files. The wizardry lives below a stable API.
7. **The frontier must never be a toll booth.** Anyone can consume our artifacts on old toolchains; only contributors touch the bleeding edge, behind gated build profiles (`MOG_PROFILE=portable|frontier|lab`).

## Engineering contracts

- Single-threaded deterministic core; zero heap allocation in the replay loop (debug allocator aborts on any malloc); no virtual dispatch on the hot path.
- Contracts (`pre`/`post`/`contract_assert`) encode domain invariants at their true sites: enforced in CI and fuzz builds, observed in release replays.
- Runtime ISA dispatch: baseline kernel correct everywhere, faster kernels selected via CPUID.
- Hand-written asm exists only where a benchmark proved its win; each kernel keeps a pure-C++ twin verified bit-identical by differential tests.

## Performance budgets

Budgets, not marketing claims: missed targets are documented with profiling
evidence; met targets ship with reproduction scripts (`results/perf-aws.md`,
CI perf-gate job).

| Metric | Budget |
|---|---|
| Tick-to-book update (median) | < 15 ns |
| Tick-to-book update (p99.9) | < 60 ns |
| Parser throughput | > 50M msg/s |
| Match/cancel processing | > 20M events/s |
| Replay-loop heap allocations | 0 (asserted in debug) |
| Hot-path virtual calls | 0 |
| Full NASDAQ day replay | < 60 s on the bench machine |

## Scope discipline

One venue (NASDAQ ITCH 5.0), backtest-only, single-instrument core with an
orchestration layer on top. Out of scope by design: live trading/broker
adapters, non-NASDAQ venues, dark pools, multi-venue routing, hidden size
inside displayed external orders. See `docs/site/fidelity-guide.md` for the
modeling boundary.

## Known external issues

- NautilusTrader round-trip comparison is blocked upstream: their position
  engine asserts under two-sided backtest flow
  (`model/events/position.pyx:331`, FLAT-side PositionOpened, reproducible
  under both OMS modes on 1.231.0). Our adapter and quote synthesis are
  shipped (`bench/race/`); revisit when upstream fixes it. Filing the issue
  is the last open follow-up from the race study.
