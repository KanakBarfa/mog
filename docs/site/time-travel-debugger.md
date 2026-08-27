# Time-travel debugger

Every backtest is a deterministic function of its inputs, so MOG never
snapshots state to travel in time - it replays. `TimeTravelSession` (C++:
`include/mog/TimeTravel.hpp`, Python: `mog.TimeTravelSession`) journals each
causal call into an operation log and rebuilds the past by re-running it.

## Core ideas

- **Record**: seed/submit/stp/drain/apply_external/advance_time/cancel/
  replace are journaled as ops and applied to a live simulator.
- **Seek**: `seek(k)` moves the cursor anywhere in history. Rewinds rebuild
  from scratch; fast-forwards apply incrementally. Postcondition either way:
  state equals sequential application of `ops[0..k)`.
- **Fork**: `fork_at(k)` hands the prefix `[0..k)` to an independent branch.
  Different suffix, different world: that is counterfactual replay v1.
- **Compare**: `first_decision_divergence(a, b)` returns the first index
  where two branches' decision streams disagree (`-1` in Python when equal).
- **Report**: `export_report()` renders a deterministic markdown timeline -
  ops, decisions, fills, trades, book L2, trace digest. Identical logs
  produce byte-identical text, so reports are golden-testable.

Determinism carries over untouched because the simulator's seeded RNG is
consumed strictly in op order: replaying identical prefixes reproduces
identical traces bit-for-bit.

## Python example

```python
import mog

s = mog.TimeTravelSession(lo_tick=0, hi_tick=4000)
s.seed_iceberg(1, "S", 1002, display=30, total=90)
s.set_stp_mode(mog.StpMode.cancel_newest)

rest = (1 << 62) + 10
s.submit(rest, "B", qty=900, price=998,
         type=mog.SimOrderType.day_limit, arrival_ts=100)
s.drain()
s.apply_external("B", price=998, qty=200)
s.submit((1 << 62) + 11, "S", qty=150, price=998,
         type=mog.SimOrderType.ioc, arrival_ts=500)
s.drain()

# Why did the attack die? Rewind before its drain and inspect.
s.seek(s.op_count() - 1)
print(s.queue_ahead_of(rest))                  # exact units queued ahead
print(s.l2_rows())                             # book at that instant

# Counterfactual: without the resting victim the attack survives.
cf = s.fork_at(s.op_count() - 1)
cf.cancel_strategy(rest)
cf.advance_time(1000)
cf.submit((1 << 62) + 11, "S", qty=150, price=998,
          type=mog.SimOrderType.ioc, arrival_ts=500)
cf.drain()
print(mog.first_decision_divergence(cf, s))    # first visible effect
print(s.sim().trace_digest() != cf.sim().trace_digest())
```

## CI regression corpus

The adversarial scenario above is versioned twice - C++
(`tests/timetravel_e2e.cpp`, ctest `timetravel`) and Python
(`python/tests/test_mog.py::TimeTravelParity`) - and asserts replay
determinism, seek equivalence at every checkpoint, fork controls
(identical suffix must reproduce the base trace exactly), deterministic
export, and the milestone exit criterion: an adversarial STP fill debugged
end-to-end using only shipped tooling.

## Notes and limits

- The session is tooling, not hot path: it allocates freely; the wrapped
  simulator's zero-allocation discipline is unchanged.
- Telemetry hygiene calls (`clear_reports`, `clear_events`) are deliberately
  not journaled - they are not part of causal history.
- Rewind cost is O(history). Scenario-scale histories are tiny; if you need
  checkpointing for million-op sessions, file an RFC.
