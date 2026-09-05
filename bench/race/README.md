# The Race

Cross-engine benchmark harness pitting mog against other microstructure
simulators on an identical, total-ordered op stream ("the canonical feed").

## Participants

| participant | driver | notes |
|---|---|---|
| mog-native | C++ (`participants/mog_native.cpp`) | the engine itself; frontier profile |
| mog (python shim) | `participants/mog_race.py` | binding-overhead reference |
| hftbacktest 2.4.x | `participants/hftbacktest_race.py` | L3 reconstruction (`ADD_ORDER` + `TRADE`), `l3_fifo_queue_model` |
| nautilus_trader | `participants/nautilus_race.py` | L1 venue over TradeTicks; processing baseline (fills require quote-driven venue - round 2) |

## Canonical feed

`feedgen.py` emits a deterministic stream (seeded): external resting adds,
external aggression prints, and a scripted strategy cycle (post bid/ask,
IOC buy/sell, cancel) interleaved as rows of one CSV. Every participant
replays the same rows in the same order; engines whose strategies are code
rather than data fire the same actions at the same stream positions via a
schedule keyed on market rows consumed.

External cancellations are deliberately excluded: mog's sim API moves
external liquidity through seeding, aggression and depletion dynamics and
cannot pull resting orders, so a common-denominator workload may not
contain them.

## Protocol

- Same feed bytes, same action interleaving, zero fees/latencies everywhere.
- Each participant replays in its own process, multiple runs.
- Metrics: events/sec over the replay loop (load excluded), peak RSS,
  self-determinism (run twice, compare content digests).
- The orchestrator idles between participants (`--cooldown`, default 20 s):
  desktop turbo/thermal ordering bias is real and was measured at ~4.6x on
  this machine before the fix. Warm-up runs are absorbed by taking medians
  over odd-sized run counts (hftbacktest JIT-compiles on first callback).

## Round-1 results (200k-op feed, i5-7500T desktop)

See `docs/site/benchmarks.md` for the table and methodology caveats.

## Running

```bash
python bench/race/feedgen.py --ops 200000 --out bench/race/out --npy
# build the native runner once:
g++-15 -std=c++26 -O2 -march=native -DMOG_PROFILE_FRONTIER=1 \
    -I include -I build/frontier/generated \
    bench/race/participants/mog_native.cpp -o bench/race/out/mog_native
/path/to/race-venv/bin/python -m bench.race.run_race --runs 7 \
    # venv needs: hftbacktest nautilus_trader; MOG_PYLIB env or default path
```
