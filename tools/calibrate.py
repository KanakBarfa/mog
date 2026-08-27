#!/usr/bin/env python3
"""Latency calibration (G9): fit SimConfig jitter parameters from a simrun
events log.

Reads the events CSV, extracts (submit_ns -> ts_ns) samples from taker fills
(rows with a submit stamp; maker fills have none by construction - their
latency is stamped at consumption time), and fits the three supported jitter
shapes by method of moments:

    uniform:     max ~ 2*mean          -> --jitter uniform:<max>
    exponential: mean ~= std            -> exponential mode, mean = sample mean
    normal:      sigma from stddev      -> (center=mean) kept for reference

Emits ready-to-paste `mog simrun` flags plus a verdict on which shape the
sample dispersion most resembles (std/mean ratio near 0.577 suggests
uniform's sqrt(1/12)... reported as guidance, never as truth).

    tools/calibrate.py events.csv [--shape uniform|exponential|normal]
"""

from __future__ import annotations

import argparse
import csv
import json
import statistics
import sys


def samples_from_log(path: str) -> list[float]:
    out = []
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            if row.get("type") != "F" or row.get("ledger") != "T":
                continue
            try:
                submit = int(row["submit_ns"])
                fill = int(row["ts_ns"])
            except (KeyError, ValueError):
                continue
            if fill >= submit:
                out.append(float(fill - submit))
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--shape", choices=["auto", "uniform", "exponential", "normal"], default="auto")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    xs = samples_from_log(args.log)
    if len(xs) < 8:
        print(f"need >= 8 taker-fill samples, got {len(xs)}", file=sys.stderr)
        return 1

    mean = statistics.fmean(xs)
    std = statistics.pstdev(xs)
    ratio = std / mean if mean > 0 else 0.0

    # Dispersion fingerprints: uniform(ratio~0.577), normal(0.1-0.3 for tight),
    # exponential(1.0).
    if args.shape == "auto":
        if abs(ratio - 1.0) < 0.25:
            shape = "exponential"
        elif abs(ratio - 0.577) < 0.15:
            shape = "uniform"
        else:
            shape = "normal"
    else:
        shape = args.shape

    if shape == "uniform":
        est_max = 2.0 * mean
        flag = f"--jitter uniform:{est_max:.0f}"
        params = {"kind": "uniform", "max_ns": round(est_max)}
    elif shape == "exponential":
        flag = "--jitter exponential"  # mean wired via --jitter-mean in engine terms
        params = {"kind": "exponential", "mean_ns": round(mean)}
    else:
        flag = "--jitter normal"
        params = {"kind": "normal", "mean_ns": round(mean), "sigma_ns": round(std)}

    result = {
        "samples": len(xs),
        "mean_ns": round(mean),
        "std_ns": round(std),
        "std_over_mean": round(ratio, 3),
        "suggested_shape": shape,
        "flag": flag,
        "params": params,
        "caveat": (
            "method-of-moments on visible latency only; wire/parse/"
            "decision stage splits are not identifiable from this log"
        ),
    }
    if args.json:
        print(json.dumps(result))
    else:
        print(f"samples         {result['samples']}")
        print(
            f"latency ns      mean={result['mean_ns']} std={result['std_ns']}"
            f" (std/mean={ratio:.3f})"
        )
        print(f"suggested       {shape}: {flag}")
        print(f"caveat          {result['caveat']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
