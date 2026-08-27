# Examples

Runnable walkthroughs under [`examples/`](https://github.com/KanakBarfa/mog/tree/main/examples).
Every example is deterministic: same inputs, same digest, every run.

## Python

Both scripts run against an installed wheel (`pip install mog`) or the
in-tree module (`PYTHONPATH=build/pylib` after building the `_core` target).

### quickstart.py

Corpus to book to mini execution sim in ~40 lines. This exact script is part
of the wheel test suite, so every release build is proven to run it.

```sh
python examples/python/quickstart.py
```

### market_maker.py

Quotes straddle the mid, selling pressure works the bid side, and the fill
reports show maker-rebate accounting (negative fees). Prints the queue
position before the sweep and the trace digest at the end.

```text
queue ahead of our bid (ref 1000): 0
fill: ts=1000 side=B qty=30 px=3999995 fee=-23999
fills=1 inventory=+30 cash=-119999850 fees=-23999
trace digest: f19636f8...
```

## C++

Built behind `-DMOG_BUILD_EXAMPLES=ON`; each binary is registered with CTest,
so a broken example fails the build gate like any other test.

```sh
cmake -B build/frontier -G Ninja -DMOG_PROFILE=frontier -DMOG_BUILD_EXAMPLES=ON
cmake --build build/frontier -j
ctest --test-dir build/frontier -R example-
```

### crtp_strategy.cpp

A minimal market maker through the CRTP `Strategy`/`StrategyRunner` hooks:
requote on every book update with decision and wire latencies configured,
survive a scripted flow sequence. The final ledger matches the numbers the
e2e suite hand-computed for the same scenario (inventory 40, cash -40280).

### queue_inspect.cpp

Joins the back of a level, then watches `queue_ahead_of()` fall as external
pressure consumes the queue: 100 ahead -> 40 -> filled passively. The L2
ladder is dumped at each step so priority semantics are visible.
