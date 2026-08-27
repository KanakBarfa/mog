#!/usr/bin/env python3
"""Convert an official NASDAQ Daily Trade file to the canonical CSV that
`mog trades --diff` consumes. Accepts the common fixed-width layout
(match number @ col 0-8, shares @ 9-17, stock @ 18-25, price @ 26-33 as
4-decimal implied) OR a CSV with those named columns; timestamps absent from
the official file are left zero so the diff keys purely on match numbers."""

import csv
import sys


def main(src: str, dst: str) -> int:
    rows = []
    with open(src) as f:
        sample = f.readline()
        f.seek(0)
        if "," in sample:
            for r in csv.DictReader(f):
                rows.append((r["match_number"], r["shares"], round(float(r["price"]) * 10000)))
        else:
            for line in f:
                if len(line) < 34:
                    continue
                m = int(line[0:8])
                q = int(line[8:16])
                px = round(float(line[25:34]) * 10000)
                rows.append((m, q, px))
    rows.sort()
    with open(dst, "w", newline="") as f:
        w = csv.writer(f)
        # Canonical 7-column format; official files carry no locate/ts/
        # printable flags, so those fields are neutral-filled and the diff
        # keys purely on match numbers, prices and shares.
        w.writerow(
            ["match_number", "locate", "ts_ns", "price_ticks", "shares", "printable", "with_price"]
        )
        for m, q, p in rows:
            w.writerow([m, 0, 0, p, q, "Y", "N"])
    print(f"{len(rows)} prints -> {dst}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2]))
