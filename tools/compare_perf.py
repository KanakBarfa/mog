#!/usr/bin/env python3
"""Compare interleaved Google Benchmark JSON captures from a perf gate.

Usage: compare_perf.py BASE.json HEAD.json [--threshold 1.10]

Each file holds one benchmark suite capture (aggregates-only format).
Medians are compared per benchmark name; exit 1 if any HEAD median exceeds
BASE median by more than threshold. Prints a table either way.
"""

import argparse
import json
import sys


def medians(path):
    try:
        with open(path) as f:
            data = json.load(f)
    except (OSError, json.JSONDecodeError) as exc:
        print(f"ERROR: cannot read benchmark data from {path}: {exc}")
        sys.exit(3)
    out = {}
    for b in data["benchmarks"]:
        if b.get("aggregate_name") == "median":
            name = b["name"].split("_median", 1)[0]
            out[name] = {
                "cpu_ns": float(b["cpu_time"]),
                "ips": float(b.get("items_per_second", 0.0)),
            }
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("base")
    ap.add_argument("head")
    ap.add_argument("--threshold", type=float, default=1.10)
    args = ap.parse_args()

    base, head = medians(args.base), medians(args.head)
    names = sorted(set(base) | set(head))
    regressions = []
    print(f"{'benchmark':34s} {'base ns':>14s} {'head ns':>14s} {'ratio':>7s}  verdict")
    for n in names:
        b, h = base.get(n), head.get(n)
        if not b or not h:
            print(f"{n:34s} {'missing':>14s} {'missing':>14s} {'-':>7s}  SKIP")
            continue
        ratio = h["cpu_ns"] / b["cpu_ns"] if b["cpu_ns"] > 0 else float("inf")
        verdict = "ok"
        if ratio > args.threshold:
            verdict = f"REGRESSION >{args.threshold:.2f}"
            regressions.append(n)
        print(f"{n:34s} {b['cpu_ns']:14.1f} {h['cpu_ns']:14.1f} {ratio:7.3f}  {verdict}")

    if regressions:
        print(f"\nPERF GATE FAILED: {len(regressions)} regression(s):")
        for n in regressions:
            print(f"  {n}")
        return 1
    print(f"\nperf gate ok (threshold {args.threshold:.2f})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
