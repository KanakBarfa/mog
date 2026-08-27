#!/usr/bin/env python3
"""Round 3: cross-engine fill agreement on the canonical stream.

Loads per-order aggregates ([ref, side, vwap_price, total_qty]) from each
participant's result JSON and quantifies divergence against mog as ground
truth (exact FIFO reconstruction, conservation-audited):

    - order coverage: filled in both engines / either engine
    - quantity agreement: exact matches, |delta qty| stats
    - price agreement: |vwap delta| in ticks among quantity-matched orders

Usage: python bench/race/agree.py --a out/mog_native.json --b out/hft_njit.json
"""

from __future__ import annotations

import argparse
import json
import statistics
from pathlib import Path

MOG_STRAT_REF_BASE = 1 << 62


def load(path: str) -> dict[str, tuple[str, float, int]]:
    payload = json.loads(Path(path).read_text())
    out = {}
    for ref, side, vwap, qty in payload["fills_by_order"]:
        r = int(ref)
        if r >= MOG_STRAT_REF_BASE:
            r -= MOG_STRAT_REF_BASE
        elif r < 10**9:
            continue  # external liquidity is not a strategy fill
        out[r] = (side, float(vwap), int(qty))
    return out


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--a", required=True, help="ground truth (mog) JSON")
    ap.add_argument("--b", required=True, help="comparison engine JSON")
    args = ap.parse_args()

    a = load(args.a)
    b = load(args.b)
    common = sorted(set(a) & set(b))

    qty_exact = sum(1 for r in common if a[r][2] == b[r][2])
    deltas = [abs(a[r][2] - b[r][2]) for r in common]
    price_ok = sum(1 for r in common if a[r][2] == b[r][2] and abs(a[r][1] - b[r][1]) <= 1.0)

    print(f"ground truth orders : {len(a)}")
    print(f"comparison orders   : {len(b)}")
    print(f"common              : {len(common)}")
    print(f"only in truth       : {len(set(a) - set(b))}")
    print(f"only in comparison  : {len(set(b) - set(a))}")
    if common:
        print(
            f"qty exact matches   : {qty_exact}/{len(common)} "
            f"({100 * qty_exact / len(common):.1f}%)"
        )
        print(
            f"|dq| mean/median/p95/max : "
            f"{statistics.mean(deltas):.1f} / "
            f"{statistics.median(deltas):.0f} / "
            f"{sorted(deltas)[int(0.95 * len(deltas))]} / {max(deltas)}"
        )
        print(
            f"qty+price matched   : {price_ok}/{len(common)} ({100 * price_ok / len(common):.1f}%)"
        )

    tot_a = sum(v[2] for v in a.values())
    tot_b = sum(v[2] for v in b.values())
    print(
        f"total filled units  : {tot_a} vs {tot_b} "
        f"(engine B {'over' if tot_b > tot_a else 'under'}-fills by "
        f"{abs(tot_b - tot_a)} = {100 * abs(tot_b - tot_a) / max(tot_a, 1):.1f}%)"
    )


if __name__ == "__main__":
    main()
