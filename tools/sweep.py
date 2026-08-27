#!/usr/bin/env python3
"""Parameter sweep and robustness harness (G6 + G10).

Fans a command template out across a parameter grid, one process per point,
cores in parallel, then aggregates the JSON lines each run prints on stdout.

    tools/sweep.py --cmd "build/portable/mog simrun script.csv --seed {seed}
                   --depletion {dep}" --param seed=1..8 --param dep=0.0,0.5
                   [--jobs N] [--bands seed]

Grid values: comma lists or a..b ranges. Every run's stdout must be one JSON
object; keys are aggregated across points: exact-match grouping for strings,
min/mean/max for numbers, and p5/p50/p95 quantile BANDS for any key named by
--bands (G10's sensitivity bands - e.g. --bands fills,pnl over seeds).
Determinism check comes free: sweeping a no-op parameter must leave every
digest identical.
"""

from __future__ import annotations

import argparse
import itertools
import json
import os
import subprocess
import sys
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor


def grid_values(spec: str) -> list[str]:
    if ".." in spec:
        lo, hi = spec.split("..", 1)
        step = "1"
        if ":" in hi:
            hi, step = hi.split(":", 1)
        return [str(v) for v in range(int(lo), int(hi) + 1, int(step))]
    return [v for v in spec.split(",") if v != ""]


def quantile(sorted_vals: list[float], q: float) -> float:
    if not sorted_vals:
        return 0.0
    idx = q * (len(sorted_vals) - 1)
    lo, hi = int(idx), min(int(idx) + 1, len(sorted_vals) - 1)
    frac = idx - lo
    return sorted_vals[lo] * (1 - frac) + sorted_vals[hi] * frac


def run_point(cmd: str, params: dict[str, str]) -> dict:
    filled = cmd
    for k, v in params.items():
        filled = filled.replace("{" + k + "}", v)
    proc = subprocess.run(
        filled,
        shell=True,
        capture_output=True,
        text=True,
        check=False,
        timeout=float(os.environ.get("MOG_SWEEP_TIMEOUT", "600")),
    )
    if proc.returncode != 0:
        return {"_error": f"exit {proc.returncode}: {proc.stderr.strip()[:200]}", "_params": params}
    try:
        out = json.loads(proc.stdout.strip().splitlines()[-1])
    except (json.JSONDecodeError, IndexError):
        return {"_error": "no json line on stdout", "_params": params}
    out["_params"] = params
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--cmd", required=True, help="command template")
    ap.add_argument("--param", action="append", default=[], help="key=a,b,c or key=lo..hi[:step]")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    ap.add_argument(
        "--bands", default="", help="comma list of numeric keys for quantile bands (p5/p50/p95)"
    )
    args = ap.parse_args()

    names = [p.split("=", 1)[0] for p in args.param]
    axes = [grid_values(p.split("=", 1)[1]) for p in args.param]
    points = [dict(zip(names, combo, strict=True)) for combo in itertools.product(*axes)]
    if not points:
        points = [{}]

    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as ex:
        results = list(ex.map(lambda pt: run_point(args.cmd, pt), points))

    failed = [r for r in results if "_error" in r]
    ok_rows = [r for r in results if "_error" not in r]
    for r in failed:
        print(f"FAIL {r['_params']}: {r['_error']}", file=sys.stderr)

    bands_keys = [k for k in args.bands.split(",") if k]
    numeric: dict[str, list[float]] = defaultdict(list)
    for r in ok_rows:
        for k, v in r.items():
            if k.startswith("_"):
                continue
            if isinstance(v, (int, float)):
                numeric[k].append(float(v))
            elif isinstance(v, str):
                pass

    # Per-key summary; digest-like string keys collapse to distinct count.
    print(f"points={len(results)} ok={len(ok_rows)} failed={len(failed)}")
    for k in sorted(numeric):
        vals = sorted(numeric[k])
        band = ""
        if k in bands_keys:
            band = (
                f"  band[p5={quantile(vals, 0.05):.4g} "
                f"p50={quantile(vals, 0.5):.4g} p95={quantile(vals, 0.95):.4g}]"
            )
        print(
            f"{k:24s} min={vals[0]:.6g} mean={sum(vals) / len(vals):.6g} max={vals[-1]:.6g}{band}"
        )
    strings: dict[str, set[str]] = defaultdict(set)
    for r in ok_rows:
        for k, v in r.items():
            if isinstance(v, str) and not k.startswith("_"):
                strings[k].add(v)
    for k in sorted(strings):
        marker = "  <- invariant OK" if k == "digest_high" and len(strings[k]) == 1 else ""
        print(f"{k:24s} distinct={len(strings[k])}{marker}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
