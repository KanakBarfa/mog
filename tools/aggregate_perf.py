#!/usr/bin/env python3
"""Aggregate perf-gate verdict across rounds.

Usage: aggregate_perf.py --threshold 1.10 BASE_R1.json HEAD_R1.json [BASE_R2.json HEAD_R2.json ...]

For each benchmark name, takes the median of per-round medians on each side,
then compares. A true regression shifts every round; runner noise does not
survive the median. Exit 1 if any head/base ratio exceeds threshold.
"""

import argparse
import json
import statistics
import sys


def round_medians(path):
    try:
        with open(path) as f:
            data = json.load(f)
    except (OSError, json.JSONDecodeError) as exc:
        print(f"ERROR: cannot read {path}: {exc}")
        sys.exit(3)
    out = {}
    for b in data["benchmarks"]:
        if b.get("aggregate_name") == "median":
            out[b["name"].split("_median", 1)[0]] = float(b["cpu_time"])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--threshold", type=float, default=1.10)
    ap.add_argument("pairs", nargs="+", help="base head [base head ...]")
    args = ap.parse_args()

    if len(args.pairs) % 2 != 0:
        print("ERROR: expected base/head JSON pairs")
        return 2

    base_by_name, head_by_name = {}, {}
    for i in range(0, len(args.pairs), 2):
        for name, vals in round_medians(args.pairs[i]).items():
            base_by_name.setdefault(name, []).append(vals)
        for name, vals in round_medians(args.pairs[i + 1]).items():
            head_by_name.setdefault(name, []).append(vals)

    names = sorted(set(base_by_name) | set(head_by_name))
    print(f"{'benchmark':34s} {'base ns':>14s} {'head ns':>14s} {'ratio':>7s}  verdict")
    regressions = []
    for n in names:
        bs, hs = base_by_name.get(n), head_by_name.get(n)
        if not bs or not hs:
            print(f"{n:34s} {'missing':>14s} {'missing':>14s} {'-':>7s}  FAIL(no data)")
            regressions.append(n)
            continue
        b, h = statistics.median(bs), statistics.median(hs)
        ratio = h / b if b > 0 else float("inf")
        verdict = "ok"
        if ratio > args.threshold:
            verdict = f"REGRESSION >{args.threshold:.2f}"
            regressions.append(n)
        print(f"{n:34s} {b:14.1f} {h:14.1f} {ratio:7.3f}  {verdict}")

    if regressions:
        print(f"\nPERF GATE FAILED: {len(regressions)} regression(s):")
        for n in regressions:
            print(f"  {n}")
        return 1
    print(
        f"\nperf gate ok (threshold {args.threshold:.2f}, "
        f"{len(args.pairs)} base/head captures aggregated)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
