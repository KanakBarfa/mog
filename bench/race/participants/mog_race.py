#!/usr/bin/env python3
"""mog participant: replay the canonical stream through ExecutionSimulator.

Run from repo root:
    MOG_PYLIB=build/py-portable/pylib python -m bench.race.participants.mog_race \
        --feed bench/race/out/feed.csv --runs 5 --out bench/race/out/mog.json
"""

from __future__ import annotations

import argparse
import os
import resource
import statistics
import sys
import time
from pathlib import Path

_HERE = Path(__file__).resolve()
sys.path.insert(0, str(_HERE.parents[1]))  # bench/race for protocol
sys.path.insert(
    0, os.environ.get("MOG_PYLIB", str(_HERE.parents[2] / "build" / "py-portable" / "pylib"))
)

import mog
from protocol import (
    MOG_STRAT_REF_BASE,
    digest_fills,
    load_stream,
    write_result,
)


def run_once(stream) -> tuple[str, int]:
    sim = mog.ExecutionSimulator(lo_tick=900, hi_tick=1100, seed=1234)
    sched = list(stream.schedule)
    si = 0

    def strat(op) -> None:
        ref = MOG_STRAT_REF_BASE + op.ref
        if op.kind == "strat_cancel":
            sim.cancel_strategy(ref)
        else:
            kind = mog.SimOrderType.day_limit if op.kind == "strat_limit" else mog.SimOrderType.ioc
            sim.submit(ref, op.side, op.qty, op.price_ticks, kind, op.ts_ns)
        sim.drain()

    for mi, op in enumerate(stream.market):
        if op.kind == "ext_add":
            r = sim.seed_external(op.ref, op.side, op.qty, op.price_ticks)
            if not r.ok:
                raise RuntimeError(f"seed_external rejected ref={op.ref}: {r.error}")
        elif op.kind == "trade":
            hit = "S" if op.side == "B" else "B"
            sim.apply_external(hit, op.price_ticks, op.qty)
        else:
            raise RuntimeError(f"unsupported market op {op.kind} in round-1 workload")
        # Trigger k means "after k market rows", i.e. consumed == mi + 1 here.
        while si < len(sched) and sched[si][0] <= mi + 1:
            strat(sched[si][1])
            si += 1
    while si < len(sched):  # actions scheduled after the final market row
        strat(sched[si][1])
        si += 1
    if si != len(sched):
        raise RuntimeError(f"{len(sched) - si} scheduled actions never fired")

    fills = [f"F,{r.ref},{r.price_ticks},{r.qty},{r.side}" for r in sim.reports()]
    agg: dict[int, list] = {}
    for r in sim.reports():
        a = agg.setdefault(r.ref, [r.side, 0, 0])
        a[1] += r.price_ticks * r.qty
        a[2] += r.qty
    fills_by_order = [[str(ref), a[0], a[1] / a[2], a[2]] for ref, a in agg.items()]
    return digest_fills(fills), len(fills), fills_by_order


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--feed", required=True)
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    stream = load_stream(args.feed)
    walls, digests = [], []
    by_order = []
    for _ in range(args.runs):
        t0 = time.perf_counter()
        d, n, by_order = run_once(stream)
        walls.append(time.perf_counter() - t0)
        digests.append(d)

    payload = {
        "participant": "mog",
        "ops": len(stream.market) + len(stream.schedule),
        "market_rows": len(stream.market),
        "runs": args.runs,
        "wall_s_median": statistics.median(walls),
        "wall_s_min": min(walls),
        "wall_s_max": max(walls),
        "events_per_sec": (len(stream.market) + len(stream.schedule)) / statistics.median(walls),
        "fills": n,
        "fills_by_order": by_order,
        "fill_digest": digests[0],
        "deterministic": len(set(digests)) == 1,
        "rss_kb_peak": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss,
        "notes": "python shim over native core; per-op drain(); zero latencies/fees",
    }
    write_result(Path(args.out), payload)
    print(
        f"mog: {payload['events_per_sec']:,.0f} ops/s "
        f"({n} fills, deterministic={payload['deterministic']})"
    )


if __name__ == "__main__":
    main()
