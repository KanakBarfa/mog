#!/usr/bin/env python3
"""hftbacktest participant: L3 reconstruction replay through
HashMapMarketDepthBacktest with the L3-FIFO queue model.

The market feed (feed.npy) carries ADD_ORDER events for every external
resting order plus TRADE prints, mirroring mog's exact-FIFO world rather
than an approximation of it. Strategy rows fire from a schedule keyed on
market rows consumed. strat_ioc maps to their MARKET order type (no IOC
time-in-force exists); strat_limit to LIMIT+GTC.

Run from repo root:
    /path/to/venv/bin/python -m bench.race.participants.hftbacktest_race \
        --feed bench/race/out/feed.csv --npy bench/race/out/feed.npy \
        --runs 3 --out bench/race/out/hft.json
"""

from __future__ import annotations

import argparse
import resource
import statistics
import sys
import time
from pathlib import Path

_HERE = Path(__file__).resolve()
sys.path.insert(0, str(_HERE.parents[1]))

from protocol import digest_fills, load_stream, write_result


def run_once(stream, npy_path: str) -> tuple[str, int]:
    from hftbacktest import (
        GTC,
        LIMIT,
        MARKET,
        BacktestAsset,
        HashMapMarketDepthBacktest,
    )

    asset = (
        BacktestAsset()
        .data([npy_path])
        .linear_asset(1.0)
        .l3_fifo_queue_model()
        .constant_order_latency(0, 0)
        .tick_size(1.0)
        .lot_size(1.0)
        .last_trades_capacity(4_000_000)
    )
    bt = HashMapMarketDepthBacktest([asset])

    def strat(op) -> None:
        if op.kind == "strat_cancel":
            rc = bt.cancel(0, op.ref, True)
            if rc not in (0, 1):
                raise RuntimeError(f"cancel ref={op.ref} rc={rc}")
            return
        buy = op.side == "B"
        fn = bt.submit_buy_order if buy else bt.submit_sell_order
        kind = LIMIT if op.kind == "strat_limit" else MARKET
        rc = fn(0, op.ref, float(op.price_ticks), float(op.qty), GTC, kind, True)
        if rc not in (0, 1):
            raise RuntimeError(f"submit ref={op.ref} rc={rc}")

    try:
        sched = list(stream.schedule)
        si = 0
        mi = 0

        def due() -> bool:
            return si < len(sched) and sched[si][0] <= mi

        while due():
            strat(sched[si][1])
            si += 1
        while True:
            rc = bt.wait_next_feed(True, 60_000_000_000)
            if rc == 1:
                break
            if rc not in (0, 2, 3):
                raise RuntimeError(f"wait_next_feed rc={rc}")
            if rc == 2:
                mi += 1
            while due():
                strat(sched[si][1])
                si += 1
        while si < len(sched):
            strat(sched[si][1])
            si += 1

        rows = []
        vals = bt.orders(0).values()
        while True:
            o = vals.next()
            if o is None:
                break
            rows.append(f"O,{o.order_id},{int(o.exec_qty)}")
        return digest_fills(rows), len(rows)
    finally:
        bt.close()


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--feed", required=True)
    ap.add_argument("--npy", required=True)
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    stream = load_stream(args.feed)
    walls, digests = [], []
    for _ in range(args.runs):
        t0 = time.perf_counter()
        d, n = run_once(stream, args.npy)
        walls.append(time.perf_counter() - t0)
        digests.append(d)

    payload = {
        "participant": "hftbacktest",
        "ops": len(stream.market) + len(stream.schedule),
        "market_rows": len(stream.market),
        "runs": args.runs,
        "wall_s_median": statistics.median(walls),
        "wall_s_min": min(walls),
        "wall_s_max": max(walls),
        "events_per_sec": (len(stream.market) + len(stream.schedule)) / statistics.median(walls),
        "orders": n,
        "fill_digest": digests[0],
        "deterministic": len(set(digests)) == 1,
        "rss_kb_peak": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss,
        "notes": "L3 ADD_ORDER+TRADE reconstruction, l3_fifo_queue_model, "
        "zero latency; python callback per strategy action; "
        "strat_ioc as MARKET; digest = final per-order fills",
    }
    write_result(Path(args.out), payload)
    print(
        f"hftbacktest: {payload['events_per_sec']:,.0f} ops/s "
        f"({n} orders, deterministic={payload['deterministic']})"
    )


if __name__ == "__main__":
    main()
