#!/usr/bin/env python3
"""Race orchestrator: runs each participant in its own subprocess, collects
result JSONs, and emits a combined report.

Usage (repo root):
    /path/to/venv/bin/python -m bench.race.run_race \
        --feed bench/race/out/feed.csv --npy bench/race/out/feed.npy
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent


def run_participant(
    module: str, extra: list[str], out_json: Path, cooldown: int = 0
) -> dict | None:
    if cooldown:
        time.sleep(cooldown)  # let turbo/thermal state settle between
        # participants - desktop ordering bias is real
    cmd = [sys.executable, "-m", module, *extra, "--out", str(out_json)]
    t0 = time.perf_counter()
    proc = subprocess.run(cmd, cwd=HERE.parents[1], capture_output=True, text=True, check=False)
    wall = time.perf_counter() - t0
    tail = (proc.stdout.strip().splitlines() or [""])[-1]
    if proc.returncode != 0:
        print(f"[skip/fail] {module}: rc={proc.returncode}\n{proc.stderr.strip()[-400:]}")
        return None
    if not out_json.exists():
        print(f"[skip] {module}: no result json ({tail})")
        return None
    payload = json.loads(out_json.read_text())
    payload["_wall_incl_startup_s"] = round(wall, 3)
    print(f"[ok] {payload['participant']}: {payload['events_per_sec']:,.0f} ops/s")
    return payload


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--feed", default="bench/race/out/feed.csv")
    ap.add_argument("--npy", default="bench/race/out/feed.npy")
    ap.add_argument("--quotes", default="bench/race/out/quotes.csv")
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument(
        "--cooldown",
        type=int,
        default=20,
        help="seconds to idle before each participant so turbo/"
        "thermal state does not bias ordering",
    )
    args = ap.parse_args()

    out_dir = HERE / "out"
    out_dir.mkdir(exist_ok=True)

    results = []

    native = out_dir / "mog_native.json"
    bin_path = out_dir / "mog_native"
    if bin_path.exists():
        time.sleep(args.cooldown)
        proc = subprocess.run(
            [str(bin_path), "--feed", args.feed, "--runs", str(args.runs), "--out", str(native)],
            check=False,
            capture_output=True,
            text=True,
        )
        if proc.returncode == 0 and native.exists():
            p = json.loads(native.read_text())
            results.append(p)
            print(f"[ok] {p['participant']}: {p['events_per_sec']:,.0f} ops/s")
        else:
            print(
                f"[fail] mog_native rc={proc.returncode}: "
                f"{(proc.stderr or proc.stdout).strip()[-300:]}"
            )
    else:
        print("[skip] mog_native binary missing - see participants/mog_native.cpp")

    r = run_participant(
        "bench.race.participants.mog_race",
        ["--feed", args.feed, "--runs", str(max(3, args.runs // 2))],
        out_dir / "mog_shim.json",
        cooldown=args.cooldown,
    )
    if r:
        r["participant"] = "mog (python shim)"
        results.append(r)

    r = run_participant(
        "bench.race.participants.hftbacktest_race",
        [
            "--feed",
            args.feed,
            "--npy",
            args.npy,
            "--runs",
            str(max(3, args.runs // 2)),
        ],
        out_dir / "hft.json",
        cooldown=args.cooldown,
    )
    if r:
        results.append(r)

    r = run_participant(
        "bench.race.participants.nautilus_race",
        ["--feed", args.feed, "--quotes", args.quotes, "--runs", "2"],
        out_dir / "nautilus.json",
        cooldown=args.cooldown,
    )
    if r:
        results.append(r)

    r = run_participant(
        "bench.race.participants.hftbacktest_njit_race",
        ["--feed", args.feed, "--npy", args.npy, "--runs", str(max(3, args.runs // 2))],
        out_dir / "hft_njit.json",
        cooldown=args.cooldown,
    )
    if r:
        r["participant"] = "hftbacktest (njit)"
        results.append(r)

    (out_dir / "race_results.json").write_text(json.dumps(results, indent=2) + "\n")

    print("\n=== Round 1: throughput & self-determinism ===")
    for p in results:
        det = "yes" if p.get("deterministic") else "NO"
        print(f"{p['participant']:24s} {p['events_per_sec']:>12,.0f} ops/s   deterministic={det}")


if __name__ == "__main__":
    main()
