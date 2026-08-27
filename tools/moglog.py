#!/usr/bin/env python3
"""moglog.py - dependency-free inspector for MOGLOG columnar event logs.

Mirrors include/mog/ColumnLog.hpp: magic, schema table, row groups. Schemas
are self-describing, so this tool never needs the C++ types that produced a
file; it validates structure and can dump any stream as CSV or JSON.

Usage:
  moglog.py info FILE
  moglog.py dump FILE [stream_name] [--format csv|json|parquet] [-o OUT]

Parquet output needs the optional extra: pip install "mog[parquet]"
"""

import argparse
import json
import struct
import sys

MAGIC = b"MOGLOG\x01\x00"
TYPE_WIDTH = {0: 1, 1: 4, 2: 8, 3: 8, 4: 8}
TYPE_NAME = {0: "u8", 1: "u32", 2: "u64", 3: "i64", 4: "f64"}
TYPE_FMT = {0: "<B", 1: "<I", 2: "<Q", 3: "<q", 4: "<d"}


class LogError(Exception):
    pass


def read_exact(f, n):
    b = f.read(n)
    if len(b) != n:
        raise LogError("truncated file")
    return b


def parse_header(f):
    if read_exact(f, 8) != MAGIC:
        raise LogError("bad magic")
    (count,) = struct.unpack("<I", read_exact(f, 4))
    streams = []
    for _ in range(count):
        (nlen,) = struct.unpack("<B", read_exact(f, 1))
        name = read_exact(f, nlen).decode()
        (fcount,) = struct.unpack("<I", read_exact(f, 4))
        fields = []
        for _ in range(fcount):
            (tcode,) = struct.unpack("<B", read_exact(f, 1))
            (flen,) = struct.unpack("<B", read_exact(f, 1))
            fname = read_exact(f, flen).decode()
            fields.append((fname, tcode))
        streams.append({"name": name, "fields": fields})
    return streams


def iter_groups(f, streams):
    """Yields (stream_index, rows-as-column-dicts) for every row group."""
    while True:
        head = f.read(8)
        if len(head) == 0:
            return
        if len(head) != 8:
            raise LogError("truncated group header")
        si, rows = struct.unpack("<II", head)
        if si >= len(streams):
            raise LogError(f"group references stream {si} beyond schema")
        cols = {}
        for fname, tcode in streams[si]["fields"]:
            w = TYPE_WIDTH[tcode]
            fmt = TYPE_FMT[tcode]
            vals = []
            for _ in range(rows):
                (v,) = struct.unpack(fmt, read_exact(f, w))
                vals.append(v)
            cols[fname] = vals
        yield si, cols


def write_parquet(path, names, tcodes, cols, rows):
    """Writes the merged stream as a typed Parquet file; pyarrow is optional."""
    try:
        import pyarrow as pa
        import pyarrow.parquet as pq
    except ImportError:
        print('error: parquet output needs pyarrow; pip install "mog[parquet]"', file=sys.stderr)
        return 1
    arrow_types = {
        0: pa.uint8(),
        1: pa.uint32(),
        2: pa.uint64(),
        3: pa.int64(),
        4: pa.float64(),
    }
    arrays = [
        pa.array(cols[name], type=arrow_types[tc]) for name, tc in zip(names, tcodes, strict=True)
    ]
    pq.write_table(pa.Table.from_arrays(arrays, names=names), path)
    print(f"wrote {rows} rows -> {path}", file=sys.stderr)
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)
    info = sub.add_parser("info", help="list streams and row counts")
    info.add_argument("file")
    dump = sub.add_parser("dump", help="dump a stream as csv, json, or parquet")
    dump.add_argument("file")
    dump.add_argument("stream")
    dump.add_argument("--format", choices=["csv", "json", "parquet"], default="csv")
    dump.add_argument("-o", "--out", help="output path (required for parquet)")
    args = ap.parse_args()

    try:
        with open(args.file, "rb") as f:
            streams = parse_header(f)
            groups = {}
            for si, cols in iter_groups(f, streams):
                merged = groups.setdefault(si, {k: [] for k in cols})
                for k, v in cols.items():
                    merged[k].extend(v)
    except (LogError, OSError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    if args.cmd == "info":
        for i, s in enumerate(streams):
            rows = len(next(iter(groups[i].values()))) if i in groups else 0
            spec = ", ".join(f"{fn}:{TYPE_NAME[tc]}" for fn, tc in s["fields"])
            print(f"[{i}] {s['name']} rows={rows} ({spec})")
        return 0

    idx = next((i for i, s in enumerate(streams) if s["name"] == args.stream), None)
    if idx is None or idx not in groups:
        print(f"error: stream {args.stream!r} not found or empty", file=sys.stderr)
        return 1
    cols = groups[idx]
    names = [fn for fn, _ in streams[idx]["fields"]]
    tcodes = [tc for _, tc in streams[idx]["fields"]]
    rows = len(cols[names[0]])
    if args.format == "parquet":
        if not args.out:
            print("error: parquet output requires --out PATH", file=sys.stderr)
            return 1
        return write_parquet(args.out, names, tcodes, cols, rows)
    rows_dict = [dict(zip(names, (cols[n][r] for n in names), strict=True)) for r in range(rows)]
    if args.format == "json":
        json.dump(rows_dict, sys.stdout, indent=1)
        print()
    else:
        print(",".join(names))
        for r in rows_dict:
            print(",".join(str(r[n]) for n in names))
    return 0


if __name__ == "__main__":
    sys.exit(main())
