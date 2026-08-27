#!/usr/bin/env python3
"""hftbacktest participant, numba-njit edition: the entire replay loop -
feed pumping AND strategy firing - compiles to native code calling their
Rust core directly, eliminating the per-event Python boundary tax that the
plain adapter pays. This is their documented fast path.

Schedule arrays are small-int encoded because numba cannot specialize on
our string ops. Fills are snapshotted from Python after the jitted replay
returns (outside the timed region, same as every other participant).

Run from repo root:
    /path/to/venv/bin/python -m bench.race.participants.hftbacktest_njit_race \
        --feed bench/race/out/feed.csv --npy bench/race/out/feed.npy \
        --runs 5 --out bench/race/out/hft_njit.json
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


def encode_schedule(stream):
    """kind: 0=limit GTC, 1=ioc(MARKET), 2=cancel; side: 0=buy 1=sell."""
    import numpy as np

    sched = list(stream.schedule)
    n = len(sched)
    triggers = np.empty(n, dtype=np.int64)
    kinds = np.empty(n, dtype=np.int8)
    sides = np.empty(n, dtype=np.int8)
    prices = np.empty(n, dtype=np.float64)
    qtys = np.empty(n, dtype=np.float64)
    refs = np.empty(n, dtype=np.uint64)
    for i, (trig, op) in enumerate(sched):
        triggers[i] = trig
        sides[i] = 0 if op.side == "B" else 1
        prices[i] = float(op.price_ticks)
        qtys[i] = float(op.qty)
        refs[i] = op.ref
        kinds[i] = {"strat_limit": 0, "strat_ioc": 1, "strat_cancel": 2}[op.kind]
    return triggers, kinds, sides, prices, qtys, refs


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--feed", required=True)
    ap.add_argument("--npy", required=True)
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    from hftbacktest import (
        GTC,
        LIMIT,
        MARKET,
        BacktestAsset,
        HashMapMarketDepthBacktest,
    )
    from numba import njit

    stream = load_stream(args.feed)
    triggers, kinds, sides, prices, qtys, refs = encode_schedule(stream)

    @njit(cache=False)
    def replay(bt, trg, knd, sd, px, qy, rf):
        si = 0
        mi = 0
        n = len(trg)
        while True:
            rc = bt.wait_next_feed(True, 60000000000)
            if rc == 1:
                break
            if rc == 2:
                mi += 1
            while si < n and trg[si] <= mi:
                if knd[si] == 2:
                    _ = bt.cancel(0, rf[si], True)
                elif sd[si] == 0:
                    ot = LIMIT if knd[si] == 0 else MARKET
                    _ = bt.submit_buy_order(0, rf[si], px[si], qy[si], GTC, ot, True)
                else:
                    ot = LIMIT if knd[si] == 0 else MARKET
                    _ = bt.submit_sell_order(0, rf[si], px[si], qy[si], GTC, ot, True)
                si += 1
        # trailing actions after end of feed
        while si < n:
            if knd[si] != 2:
                if sd[si] == 0:
                    ot = LIMIT if knd[si] == 0 else MARKET
                    _ = bt.submit_buy_order(0, rf[si], px[si], qy[si], GTC, ot, False)
                else:
                    ot = LIMIT if knd[si] == 0 else MARKET
                    _ = bt.submit_sell_order(0, rf[si], px[si], qy[si], GTC, ot, False)
            si += 1
        return 0

    walls, digests = [], []
    orders_count = 0
    last_by_order = []
    for _run in range(args.runs):
        asset = (
            BacktestAsset()
            .data([args.npy])
            .linear_asset(1.0)
            .l3_fifo_queue_model()
            .constant_order_latency(0, 0)
            .tick_size(1.0)
            .lot_size(1.0)
            .last_trades_capacity(4_000_000)
        )
        bt = HashMapMarketDepthBacktest([asset])
        try:
            t0 = time.perf_counter()
            replay(bt, triggers, kinds, sides, prices, qtys, refs)
            walls.append(time.perf_counter() - t0)

            rows = []
            by_order = []
            vals = bt.orders(0).values()
            while True:
                o = vals.next()
                if o is None:
                    break
                q = int(o.exec_qty)
                if q == 0:
                    continue
                rows.append(f"O,{o.order_id},{q}")
                by_order.append([str(o.order_id), "", float(o.exec_price_tick) * 1.0, q])
            orders_count = len(rows)
            digests.append(digest_fills(rows))
            last_by_order = by_order
        finally:
            bt.close()

    payload = {
        "fills_by_order": last_by_order,
        "participant": "hftbacktest-njit",
        "ops": len(stream.market) + len(stream.schedule),
        "market_rows": len(stream.market),
        "runs": args.runs,
        "wall_s_median": statistics.median(walls),
        "wall_s_min": min(walls),
        "events_per_sec": (len(stream.market) + len(stream.schedule)) / statistics.median(walls),
        "orders": orders_count,
        "fill_digest": digests[0],
        "deterministic": len(set(digests)) == 1,
        "rss_kb_peak": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss,
        "notes": "L3 reconstruction, l3_fifo_queue_model, zero latency; "
        "replay loop + strategy firing fully njit-compiled; "
        "digest = final per-order fills",
    }
    write_result(Path(args.out), payload)
    print(
        f"hftbacktest-njit: {payload['events_per_sec']:,.0f} ops/s "
        f"({orders_count} orders, deterministic={payload['deterministic']})"
    )


if __name__ == "__main__":
    main()
