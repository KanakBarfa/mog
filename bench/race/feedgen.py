#!/usr/bin/env python3
"""Generate the canonical cross-engine race workload.

Emits:
    feed.csv   total-ordered op stream (schema in protocol.py)
    feed.npy   market rows only, as an hftbacktest EVENT_ARRAY (optional,
               requires hftbacktest installed): DEPTH events carry the
               absolute quantity at a level, TRADE events the aggression.

Deterministic under --seed. The generator keeps a shadow L2 book only to
choose sane prices and capped trade sizes; engines do authoritative matching.

Usage: python bench/race/feedgen.py --out bench/race/out [--ops 200000]
"""

from __future__ import annotations

import argparse
import csv
import sys
from collections import defaultdict
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from protocol import Op


def generate(n_ops: int, seed: int, want_quotes: bool = False):
    rng = np.random.default_rng(seed)
    mid, lo = 1000, 950

    book = {"B": defaultdict(int), "S": defaultdict(int)}
    ext_size: dict[int, tuple[str, int, int]] = {}  # ref -> (side, px, qty)
    strat_live: list[int] = []
    next_ext_ref = 1
    next_strat_ref = 10**9
    strat_cycle = ("post_bid", "post_ask", "ioc_buy", "ioc_sell", "cancel")
    cycle_i = 0
    ts = 0
    rows: list[Op] = []
    quotes: list[tuple[int, int, int, int, int]] = []

    def best(side: str) -> int | None:
        lv = [p for p, q in book[side].items() if q > 0]
        if not lv:
            return None
        return min(lv) if side == "B" else max(lv)

    def add_level(side: str, px: int, qty: int) -> None:
        book[side][px] += qty

    def drop_level(side: str, px: int) -> None:
        if book[side][px] <= 0:
            del book[side][px]

    for _ in range(n_ops):
        ts += int(rng.integers(300, 3000))
        r = rng.random()
        bb, ba = best("B"), best("S")

        if want_quotes and bb is not None and ba is not None:
            quotes.append((ts, bb, book["B"][bb], ba, book["S"][ba]))

        # 40% aggression, 38% new external liquidity, 22% scripted strategy
        # action. External cancels are deliberately excluded: mog's sim API
        # moves external liquidity via seeding + aggression + depletion
        # dynamics and cannot pull resting orders, so a fair common-denominator
        # workload may not contain them.
        if r < 0.40 and bb is not None and ba is not None:
            side = "B" if int(rng.integers(0, 2)) == 0 else "S"
            opp = best("S" if side == "B" else "B")
            assert opp is not None
            qty = int(min(rng.integers(50, 400), book["S" if side == "B" else "B"][opp]))
            if qty <= 0:
                continue
            rows.append(Op("trade", ts, side, opp, qty, 0))
            victim = "S" if side == "B" else "B"
            book[victim][opp] -= qty
            drop_level(victim, opp)

        elif r < 0.78:
            side = "B" if int(rng.integers(0, 2)) == 0 else "S"
            b = best(side)
            if b is None:
                px = mid - (1 if side == "B" else -1) * (2 + int(rng.integers(0, 4)))
            else:
                off = int(rng.integers(0, 4))
                px = b - off if side == "B" else b + off
            if not lo <= px <= 1050:
                continue
            qty = int(rng.integers(20, 500))
            add_level(side, px, qty)
            ext_size[next_ext_ref] = (side, px, qty)
            rows.append(Op("ext_add", ts, side, px, qty, next_ext_ref))
            next_ext_ref += 1

        else:
            action = strat_cycle[cycle_i % len(strat_cycle)]
            cycle_i += 1
            if action == "cancel":
                if strat_live:
                    rows.append(Op("strat_cancel", ts, "", 0, 0, strat_live.pop(0)))
            elif action == "post_bid" and bb is not None:
                q = int(rng.integers(80, 150))
                rows.append(Op("strat_limit", ts, "B", bb, q, next_strat_ref))
                strat_live.append(next_strat_ref)
                next_strat_ref += 1
            elif action == "post_ask" and ba is not None:
                q = int(rng.integers(80, 150))
                rows.append(Op("strat_limit", ts, "S", ba, q, next_strat_ref))
                strat_live.append(next_strat_ref)
                next_strat_ref += 1
            elif action == "ioc_buy" and ba is not None:
                rows.append(
                    Op("strat_ioc", ts, "B", ba, int(rng.integers(40, 120)), next_strat_ref)
                )
                next_strat_ref += 1
            elif action == "ioc_sell" and bb is not None:
                rows.append(
                    Op("strat_ioc", ts, "S", bb, int(rng.integers(40, 120)), next_strat_ref)
                )
                next_strat_ref += 1

    return rows, quotes


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--ops", type=int, default=200_000)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--npy", action="store_true", help="also emit hftbacktest EVENT_ARRAY feed.npy")
    ap.add_argument(
        "--quotes", action="store_true", help="also emit top-of-book quotes.csv (nautilus L1 venue)"
    )
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    rows, quotes = generate(args.ops, args.seed, args.quotes)

    with open(args.out / "feed.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["kind", "ts_ns", "side", "price_ticks", "qty", "ref"])
        for op in rows:
            w.writerow([op.kind, op.ts_ns, op.side, op.price_ticks, op.qty, op.ref])

    if args.quotes:
        with open(args.out / "quotes.csv", "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["ts_ns", "bid_px", "bid_qty", "ask_px", "ask_qty"])
            for q in quotes:
                w.writerow(q)

    if args.npy:
        # hftbacktest participant runs an L3 reconstruction: every external
        # resting order is an ADD_ORDER event (order_id = its ref), trades are
        # TRADE prints. Rows carry both EXCH and LOCAL bits - each processor
        # filters its own, and a feed lacking LOCAL rows starves the local
        # clock into reporting instant end-of-data.
        from hftbacktest import (
            ADD_ORDER_EVENT,
            BUY_EVENT,
            EXCH_EVENT,
            LOCAL_EVENT,
            SELL_EVENT,
            TRADE_EVENT,
            event_dtype,
        )

        recs = []
        for op in rows:
            side_bit = BUY_EVENT if op.side == "B" else SELL_EVENT
            base = EXCH_EVENT | LOCAL_EVENT | side_bit
            if op.kind == "ext_add":
                recs.append(
                    (
                        base | ADD_ORDER_EVENT,
                        op.ts_ns,
                        op.ts_ns,
                        float(op.price_ticks),
                        float(op.qty),
                        op.ref,
                        0,
                        0.0,
                    )
                )
            elif op.kind == "trade":
                recs.append(
                    (
                        base | TRADE_EVENT,
                        op.ts_ns,
                        op.ts_ns,
                        float(op.price_ticks),
                        float(op.qty),
                        0,
                        0,
                        0.0,
                    )
                )
        arr = np.zeros(len(recs), dtype=event_dtype)
        for i, t in enumerate(recs):
            arr[i] = t
        np.save(args.out / "feed.npy", arr)

    n_market = sum(1 for o in rows if not o.kind.startswith("strat"))
    print(f"{len(rows)} ops ({n_market} market) -> {args.out}")


if __name__ == "__main__":
    main()
