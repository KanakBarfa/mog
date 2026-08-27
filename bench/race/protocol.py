"""Shared race protocol: op stream schema, loading, digests, result JSON.

The canonical workload is a single total-ordered op stream (CSV):

    kind,ts_ns,side,price_ticks,qty,ref

kinds:
    ext_add      external resting limit joins the book
    trade        external aggression consumes qty at price from aggressor side
    strat_limit  our passive day-limit order
    strat_ioc    our aggressive ioc order
    strat_cancel cancel of a prior strategy ref

Rows are replayed in file order by every engine; strategy rows are part of
the stream itself so the interleaving is identical everywhere. Adapters for
engines whose strategies are code rather than data (hftbacktest, nautilus)
derive a schedule keyed on "market rows consumed so far" and fire the same
actions at the same points.
"""

from __future__ import annotations

import csv
import hashlib
import json
from dataclasses import dataclass
from pathlib import Path

KINDS = ("ext_add", "trade", "strat_limit", "strat_ioc", "strat_cancel")

# mog contract: strategy refs live above external_ref_limit (default 1<<62).
MOG_STRAT_REF_BASE = 1 << 62


@dataclass(frozen=True)
class Op:
    kind: str
    ts_ns: int
    side: str
    price_ticks: int
    qty: int
    ref: int


@dataclass
class Stream:
    market: list[Op]
    schedule: list[tuple[int, Op]]  # (fire after this many market rows, strat op)


def load_stream(csv_path: str | Path) -> Stream:
    market: list[Op] = []
    schedule: list[tuple[int, Op]] = []
    with open(csv_path, newline="") as f:
        for i, row in enumerate(csv.reader(f)):
            if i == 0:
                continue
            op = Op(row[0], int(row[1]), row[2], int(row[3]), int(row[4]), int(row[5]))
            if op.kind.startswith("strat"):
                schedule.append((len(market), op))
            else:
                market.append(op)
    return Stream(market=market, schedule=schedule)


def digest_fills(rows: list[str]) -> str:
    h = hashlib.sha256()
    for r in sorted(rows):
        h.update(r.encode())
        h.update(b"\n")
    return h.hexdigest()


def write_result(path: Path, payload: dict) -> None:
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
