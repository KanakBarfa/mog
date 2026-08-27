#!/usr/bin/env python3
"""Independent bounds-check of reconstructed prints against stooq daily bars.

The official-file diff (O1) needs licensed NASDAQ Daily Trades; until that
exists, free consolidated daily OHLCV bars give a one-sided sanity band:
every reconstructed print price must lie within the symbol's official day
range (small tolerance for venue consolidation differences - our tape is
NASDAQ-book only, so extremes printed elsewhere legitimately exceed it).

Bars come from stooq.com's free unadjusted daily CSV endpoint (yfinance was
tried first and returned inconsistently split-adjusted rows in batch mode).

Usage: yahoo_bounds_check.py <stats.csv> [--date 2019-12-30] [--tol-pct 0.5]
                             [--top 150]
"""

import argparse
import csv
import time
import urllib.request


def fetch_bar(symbol: str, date: str, timeout: float = 15.0):
    url = (
        f"https://stooq.com/q/d/l/?s={symbol.lower()}.us"
        f"&d1={date.replace('-', '')}&d2={date.replace('-', '')}&i=d"
    )
    req = urllib.request.Request(url, headers={"User-Agent": "mog-fidelity/1.0"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            text = r.read().decode()
    except Exception as e:  # noqa: BLE001
        return None, f"fetch failed: {e}"
    lines = text.strip().splitlines()
    if len(lines) < 2:
        return None, "no data"
    parts = lines[1].split(",")
    # Date,Open,High,Low,Close,Volume
    try:
        return {
            "date": parts[0],
            "open": float(parts[1]),
            "high": float(parts[2]),
            "low": float(parts[3]),
            "close": float(parts[4]),
        }, None
    except (IndexError, ValueError):
        return None, f"bad row: {lines[1][:60]}"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("stats_csv")
    ap.add_argument("--date", default="2019-12-30")
    ap.add_argument("--tol-pct", type=float, default=0.5)
    ap.add_argument("--top", type=int, default=150)
    args = ap.parse_args()

    with open(args.stats_csv) as fh:
        rows = list(csv.DictReader(fh))
    rows.sort(key=lambda r: -int(r["shares"]))
    sample = [r for r in rows if int(r["prints"]) >= 20][: args.top]
    print(f"checking {len(sample)} symbols (of {len(rows)}) for {args.date}")

    checked = strict_ok = tol_ok = 0
    violations, misses = [], []
    for r in sample:
        sym = r["symbol"]
        bar, err = fetch_bar(sym, args.date)
        if bar is None:
            misses.append((sym, err))
            time.sleep(0.15)
            continue
        low, high = bar["low"], bar["high"]
        min_px = int(r["min_tick"]) / 10000.0
        max_px = int(r["max_tick"]) / 10000.0
        checked += 1
        tol = max(low, high) * args.tol_pct / 100.0
        s = min_px >= low - 1e-9 and max_px <= high + 1e-9
        t = min_px >= low - tol and max_px <= high + tol
        strict_ok += s
        tol_ok += t
        if not t:
            violations.append((sym, low, high, min_px, max_px))
        time.sleep(0.05)

    print(f"checked={checked} strict-in-range={strict_ok} within-{args.tol_pct}%={tol_ok}")
    print(f"data-misses={len(misses)} (delisted/symbol-map gaps)")
    for sym, low, high, min_px, max_px in violations[:10]:
        print(f"VIOLATION {sym} stooq[{low:.4f}..{high:.4f}] ours[{min_px:.4f}..{max_px:.4f}]")
    return 0


if __name__ == "__main__":
    main()
