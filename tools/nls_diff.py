#!/usr/bin/env python3
"""Compares reconstructed prints against an official Nasdaq Last Sale 4.0 tape.

NLS carries no ITCH match numbers, so the comparison is a multiset join on
(symbol, ts_ns, price_ticks, shares) over our capture's time window:

- ours-without-ref: fidelity failures - every one must be explained
- ref-only: official-tape extras, broken down by originating market center
  (Q=Nasdaq book, L/2=TRF off-exchange reports, B=BX, X=PSX)

Trade Cancel/Error ('o') and Correction ('b') messages are applied to the
reference set before comparison. Full-day tapes are streamed twice instead
of being materialized: pass one records control numbers targeted by o/b,
pass two counts Q-center window rows directly into the Counter and keeps
mutable state only for those targeted reports.

Usage: nls_diff.py <nls40-binary> <prints.csv>
"""

import argparse
import sys
from collections import Counter


def u48(b):
    return int.from_bytes(b, "big")


def iter_frames(path):
    """Yields memoryview slices of each BinaryFILE frame."""
    with open(path, "rb") as fh:
        header = fh.read(2)
        while len(header) == 2:
            ln = int.from_bytes(header, "big")
            if ln == 0:
                return
            body = fh.read(ln)
            if len(body) != ln:
                return
            yield body
            header = fh.read(2)


def targeted_controls(path):
    """Control numbers referenced by cancel ('o') or correction ('b')."""
    touched = set()
    centers = {}
    for m in iter_frames(path):
        t = chr(m[0])
        if t in ("o", "b"):
            center = chr(m[17])
            key = (center, bytes(m[27:37]))
            touched.add(key)
            centers.setdefault(key, center)
    return touched


def count_reference(path, lo, hi, divisor, touched):
    """Streams the tape once; returns (ref_counter, all_center_counts).

    Untouched Q-center window rows are counted on sight. Rows targeted by a
    later o/b are held minimally until their event resolves them inline -
    cancel drops them, correction rewrites price/size before counting.
    """
    ref_c = Counter()
    all_center = Counter()
    live = {}  # (center, control) -> [sym, ts, px, sz] pending o/b

    def emit(center, sym, ts, px, sz):
        if lo <= ts <= hi:
            all_center[center] += 1
            if center == "Q":
                ref_c[(sym, ts, px, sz // divisor)] += 1

    for m in iter_frames(path):
        t = chr(m[0])
        if t == "e":
            ts = u48(m[3:9])
            center = chr(m[17])
            control = bytes(m[27:37])
            px = int.from_bytes(m[37:45], "big")
            size = int.from_bytes(m[45:53], "big")
            key = (center, control)
            if key not in touched:
                emit(center, m[18:26].decode("latin-1").rstrip(), ts, px, size)
            else:
                live[key] = [m[18:26].decode("latin-1").rstrip(), ts, px, size]
        elif t == "o":
            center = chr(m[17])
            control = bytes(m[27:37])
            live.pop((center, control), None)
        elif t == "b" and len(m) >= 61:
            center = chr(m[17])
            control = bytes(m[27:37])
            row = live.get((center, control))
            if row is not None:
                row[2] = int.from_bytes(m[47:55], "big")
                row[3] = int.from_bytes(m[55:63], "big")

    for (center, _), (sym, ts, px, sz) in live.items():
        emit(center, sym, ts, px, sz)

    return ref_c, all_center


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("nls_binary")
    ap.add_argument("prints_csv")
    ap.add_argument("--report-csv", default=None, help="write ours-without-ref rows for inspection")
    ap.add_argument(
        "--buckets-prefix",
        default=None,
        help="write per-(symbol,price) volume CSVs <prefix>.ref.csv / .ours.csv",
    )
    ap.add_argument(
        "--size-divisor",
        type=int,
        default=1,
        help="divide NLS sizes by this before comparing (Size(6) tapes need 1000000)",
    )
    args = ap.parse_args()

    ours = []
    with open(args.prints_csv) as f:
        next(f)
        for line in f:
            _, sym, ts, px, q = line.rstrip("\n").split(",")
            ours.append((sym, int(ts), int(px), int(q)))
    if not ours:
        print("no prints", file=sys.stderr)
        return 2
    lo, hi = min(o[1] for o in ours), max(o[1] for o in ours)
    print(f"ours: {len(ours)} prints, window {lo}..{hi}", file=sys.stderr)

    print("pass 1: locating cancelled/corrected controls...", file=sys.stderr)
    touched = targeted_controls(args.nls_binary)
    print(f"touched by o/b: {len(touched)}", file=sys.stderr)

    print("pass 2: counting reference rows...", file=sys.stderr)
    ref_c, all_center = count_reference(args.nls_binary, lo, hi, args.size_divisor, touched)

    ours_c = Counter(ours)
    del ours

    missing = ours_c - ref_c
    extra = ref_c - ours_c
    matched = sum((ours_c & ref_c).values())

    if args.buckets_prefix:

        def dump(path, counter):
            vol = Counter()
            for (sym, _ts, px, sz), k in counter.items():
                vol[(sym, px)] += sz * k
            with open(path, "w") as f:
                f.write("symbol,price_ticks,volume\n")
                for (sym, px), v in sorted(vol.items()):
                    f.write(f"{sym},{px},{v}\n")

        dump(args.buckets_prefix + ".ref.csv", ref_c)
        dump(args.buckets_prefix + ".ours.csv", ours_c)
        print(f"bucket volumes -> {args.buckets_prefix}.{{ref,ours}}.csv", file=sys.stderr)

    print(f"\nwindow-restricted reference (market center Q only): {sum(ref_c.values())}")
    print(f"matched exactly:            {matched}")
    print(f"OURS WITHOUT REF (fails):   {sum(missing.values())}")
    print(f"ref-only (Q-center):        {sum(extra.values())}")

    print("\nall-center reference rows in window:")
    for c, k in sorted(all_center.items()):
        print(f"  {c}: {k}")

    if args.report_csv and missing:
        with open(args.report_csv, "w") as f:
            f.write("symbol,ts_ns,price_ticks,shares,count\n")
            for (s, ts, px, q), k in missing.most_common():
                f.write(f"{s},{ts},{px},{q},{k}\n")
        print(f"failing rows -> {args.report_csv}")


if __name__ == "__main__":
    sys.exit(main())
