"""End-to-end tests for tools/moglog.py against synthetic MOGLOG files."""

import json
import struct
import subprocess
import sys
from pathlib import Path

import pytest

MOGLOG = Path(__file__).resolve().parents[2] / "tools" / "moglog.py"

MAGIC = b"MOGLOG\x01\x00"
TYPE_FMT = {0: "<B", 1: "<I", 2: "<Q", 3: "<q", 4: "<d"}
TYPE_WIDTH = {0: 1, 1: 4, 2: 8, 3: 8, 4: 8}


def write_moglog(path):
    """Writes two streams across interleaved row groups, all five type codes."""
    streams = [
        ("fills", [("oid", 1), ("px", 3), ("qty", 1)]),
        ("mids", [("ts", 3), ("mid", 4)]),
    ]
    groups = [
        (0, 2, {"oid": [7, 9], "px": [990, 1010], "qty": [50, 30]}),
        (1, 2, {"ts": [100, 200], "mid": [1000.5, 1002.25]}),
        (0, 1, {"oid": [11], "px": [995], "qty": [10]}),
    ]
    with open(path, "wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<I", len(streams)))
        for name, fields in streams:
            f.write(struct.pack("<B", len(name)))
            f.write(name.encode())
            f.write(struct.pack("<I", len(fields)))
            for fname, tcode in fields:
                f.write(struct.pack("<BB", tcode, len(fname)))
                f.write(fname.encode())
        for si, nrows, cols in groups:
            f.write(struct.pack("<II", si, nrows))
            for fname, tcode in streams[si][1]:
                f.writelines(struct.pack(TYPE_FMT[tcode], v) for v in cols[fname])
    return streams, groups


def run(*argv):
    return subprocess.run(
        [sys.executable, str(MOGLOG), *argv], capture_output=True, text=True, check=False
    )


@pytest.fixture()
def log(tmp_path):
    p = tmp_path / "sample.moglog"
    write_moglog(p)
    return p


def test_info_reports_streams_and_rows(log):
    out = run("info", str(log))
    assert out.returncode == 0
    assert "[0] fills rows=3 (oid:u32, px:i64, qty:u32)" in out.stdout
    assert "[1] mids rows=2 (ts:i64, mid:f64)" in out.stdout


def test_dump_csv_roundtrips_values_across_groups(log):
    out = run("dump", str(log), "fills")
    assert out.returncode == 0
    lines = out.stdout.strip().splitlines()
    assert lines[0] == "oid,px,qty"
    assert lines[1:] == ["7,990,50", "9,1010,30", "11,995,10"]


def test_dump_json_parses_back(log):
    out = run("dump", str(log), "mids", "--format", "json")
    assert out.returncode == 0
    rows = json.loads(out.stdout)
    assert rows == [
        {"ts": 100, "mid": 1000.5},
        {"ts": 200, "mid": 1002.25},
    ]


def test_dump_missing_stream_fails_cleanly(log):
    out = run("dump", str(log), "nope")
    assert out.returncode == 1
    assert "not found or empty" in out.stderr


import pyarrow.parquet

pyarrow = pytest.importorskip("pyarrow")


def test_dump_parquet_preserves_types_and_values(log, tmp_path):
    out_path = tmp_path / "fills.parquet"
    out = run("dump", str(log), "fills", "--format", "parquet", "-o", str(out_path))
    assert out.returncode == 0
    t = pyarrow.parquet.read_table(out_path)
    assert t.column_names == ["oid", "px", "qty"]
    assert [str(c.type) for c in t.columns] == ["uint32", "int64", "uint32"]
    assert t.column("px").to_pylist() == [990, 1010, 995]


def test_dump_parquet_requires_out_path(log):
    out = run("dump", str(log), "fills", "--format", "parquet")
    assert out.returncode == 1
    assert "--out" in out.stderr
