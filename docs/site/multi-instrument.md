# Multi-instrument orchestration

The core is single-instrument by design (see DESIGN.md): one
book, one simulator, one determinism contract per instrument. Portfolios
are an orchestration concern, and `Orchestrator` (C++:
`include/mog/Orchestrate.hpp`, Python: `mog.Orchestrator`) provides exactly
that - fan-out across up to 256 independent `ExecutionSimulator`s behind
one facade, without polluting per-book semantics or the hot path.

## Rules of the layer

- **Explicit routing.** Every instrument-scoped op names its target index.
  Routing is never inferred from refs, symbols, or state, so it cannot be
  ambiguous. Indices are allocation order: `add_instrument` returns them
  and they are stable for the portfolio's lifetime.
- **Ordered broadcasts.** `drain_all()` / `advance_all(dt)` apply to
  instruments in ascending index order. That ordering *is* the
  determinism contract: a portfolio is a pure function of (per-instrument
  configs, op sequence), and `global_digest()` folds per-instrument trace
  digests in the same order.
- **Independent ref spaces.** The same ref may legally exist in two
  instruments; ids are per-book, not global. Cross-instrument strategy
  logic (leg spreads routed through one decision) is out of scope for v1 -
  it needs an RFC because it touches the queue model.

## Python example

```python
import mog

cfg = dict(lo_tick=0, hi_tick=4000, arena_capacity=256,
           page_pool=8, event_capacity=1024)
o = mog.Orchestrator()
es = o.add_instrument(name="ES", **cfg)
nq = o.add_instrument(name="NQ", **cfg)

K = 1 << 62
for idx in (es, nq):
    o.seed_external(idx, K + idx * 100 + 1, "S", 50, 1000)
    o.submit(idx, K + idx * 100 + 2, "B", 30, 999,
             mog.SimOrderType.day_limit, arrival_ts=10)

o.drain_all()
o.advance_all(500)          # broadcast: ascending index order

# Per-instrument inspection through the usual simulator API:
print(o.sim(es).decisions())
print(o.sim(nq).queue_ahead_of(K + nq * 100 + 2))
print(o.global_digest())    # fold over per-instrument digests
assert o.audit()
```

## CI regression corpus

`tests/orchestrate_e2e.cpp` (ctest `orchestrate`) and
`python/tests/test_mog.py::OrchestrationParity` assert, in both languages:

- portfolio replay determinism - identical op sequences produce identical
  `global_digest()`;
- bit-exact isolation - an instrument's digest equals a solo simulator fed
  only its own script after foreign ops interleave;
- broadcast-equals-routed equivalence for time advances;
- independent ref spaces with identical refs resting on both books.

## Notes and limits

- Instruments are heap-held (`ExecutionSimulator` is non-movable); the
  fan-out array is fixed-capacity and pointer-stable.
- Adding any instrument changes `global_digest()` even if it is never
  touched - the digest covers portfolio membership.
- Time travel composes per instrument: hold a `TimeTravelSession` for a
  book you need to rewind. A portfolio-wide session type would duplicate
  that journaling and is deliberately deferred until a use case demands it.
