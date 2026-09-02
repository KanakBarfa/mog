"""mog - deterministic L3 limit-order-book backtesting engine (Python shim).

Thin nanobind binding over the portable-profile C++ core: ITCH parsing,
order-book replay, and the execution simulator, with byte-exact determinism
guarantees shared across languages (see docs/EVALUATION.md).
"""

import struct

from ._core import (
    BookTick,
    BookUpdate,
    Decision,
    DeterminismDigest,
    DigestMode,
    ExecutionSimulator,
    FastDigest128,
    FillReport,
    HasbrouckShare,
    ImpactConfig,
    ImpactState,
    JitterKind,
    LeadLagResult,
    MarkoutReport,
    Message,
    MogDecodeError,
    MogParseError,
    Orchestrator,
    OrderAck,
    OrderBook,
    OrderFill,
    OuchGateway,
    SimOrderType,
    SimRunSummary,
    StpMode,
    Strategy,
    StrategyRunner,
    TearsheetReport,
    TimeTravelSession,
    Trade,
    TradePrintRecord,
    TradesSummary,
    __version__,
    build_info,
    compute_expected_shortfall,
    compute_hasbrouck_share,
    compute_lead_lag,
    compute_optimal_trajectory,
    compute_permanent_impact,
    compute_tearsheet_from_csv,
    compute_temporary_impact,
    decode_message,
    first_decision_divergence,
    frame_length,
    ouch_inbound_frame_length,
    ouch_outbound_frame_length,
    parse_itch,
    run_simrun_script,
    run_trades,
    sample_corpus,
    trace_hash,
)

_LOG_MAGIC = b"MOGLOG\x01\x00"
_TYPE_WIDTH = {0: 1, 1: 4, 2: 8, 3: 8, 4: 8}
_TYPE_FMT = {0: "<B", 1: "<I", 2: "<Q", 3: "<q", 4: "<d"}


class LogFile:
    """Reader for .moglog binary columnar event logs."""

    def __init__(self, streams, groups):
        self.streams = streams
        self.groups = groups

    def stream_names(self):
        return [s["name"] for s in self.streams]

    def __getitem__(self, name):
        idx = next((i for i, s in enumerate(self.streams) if s["name"] == name), None)
        if idx is None or idx not in self.groups:
            raise KeyError(f"stream {name!r} not found")
        return self.groups[idx]

    def to_arrow(self, name):
        """Convert stream to PyArrow Table."""
        import pyarrow as pa

        idx = next((i for i, s in enumerate(self.streams) if s["name"] == name), None)
        if idx is None or idx not in self.groups:
            raise KeyError(f"stream {name!r} not found")
        cols = self.groups[idx]
        arrow_types = {
            0: pa.uint8(),
            1: pa.uint32(),
            2: pa.uint64(),
            3: pa.int64(),
            4: pa.float64(),
        }
        arrays = [
            pa.array(cols[fname], type=arrow_types[tc]) for fname, tc in self.streams[idx]["fields"]
        ]
        names = [fname for fname, _ in self.streams[idx]["fields"]]
        return pa.Table.from_arrays(arrays, names=names)

    def to_polars(self, name):
        """Convert stream to Polars DataFrame."""
        import polars as pl

        return pl.from_arrow(self.to_arrow(name))

    def to_parquet(self, name, path, compression="zstd"):
        """Write stream directly to a Parquet file."""
        import pyarrow.parquet as pq

        pq.write_table(self.to_arrow(name), path, compression=compression)


def read_log(path):
    """Read a .moglog binary columnar log file into a LogFile object."""
    with open(path, "rb") as f:
        magic = f.read(8)
        if magic != _LOG_MAGIC:
            raise ValueError("invalid moglog magic")
        (count,) = struct.unpack("<I", f.read(4))
        streams = []
        for _ in range(count):
            (nlen,) = struct.unpack("<B", f.read(1))
            name = f.read(nlen).decode()
            (fcount,) = struct.unpack("<I", f.read(4))
            fields = []
            for _ in range(fcount):
                (tcode,) = struct.unpack("<B", f.read(1))
                (flen,) = struct.unpack("<B", f.read(1))
                fname = f.read(flen).decode()
                fields.append((fname, tcode))
            streams.append({"name": name, "fields": fields})
        groups = {}
        while True:
            head = f.read(8)
            if not head or len(head) < 8:
                break
            si, rows = struct.unpack("<II", head)
            if si >= len(streams):
                break
            cols = {}
            for fname, tcode in streams[si]["fields"]:
                w = _TYPE_WIDTH[tcode]
                fmt = _TYPE_FMT[tcode]
                vals = []
                for _ in range(rows):
                    (v,) = struct.unpack(fmt, f.read(w))
                    vals.append(v)
                cols[fname] = vals
            merged = groups.setdefault(si, {k: [] for k in cols})
            for k, v in cols.items():
                merged[k].extend(v)
    return LogFile(streams, groups)


def to_polars(obj):
    """Convert an Arrow-compatible mog object to a Polars DataFrame (zero-copy)."""
    import polars as pl

    return pl.from_arrow(obj)


def to_pandas(obj):
    """Convert an Arrow-compatible mog object to a Pandas DataFrame."""
    import pyarrow as pa

    return pa.record_batch(obj).to_pandas()


def to_parquet(obj, path, compression="zstd"):
    """Write an Arrow-compatible mog object directly to a Parquet file."""
    import pyarrow as pa
    import pyarrow.parquet as pq

    table = pa.Table.from_batches([pa.record_batch(obj)])
    pq.write_table(table, path, compression=compression)


__all__ = [
    "BookTick",
    "BookUpdate",
    "Decision",
    "DeterminismDigest",
    "DigestMode",
    "ExecutionSimulator",
    "FastDigest128",
    "FillReport",
    "HasbrouckShare",
    "ImpactConfig",
    "ImpactState",
    "JitterKind",
    "LeadLagResult",
    "LogFile",
    "MarkoutReport",
    "Message",
    "MogDecodeError",
    "MogParseError",
    "Orchestrator",
    "OrderAck",
    "OrderBook",
    "OrderFill",
    "OuchGateway",
    "SimOrderType",
    "SimRunSummary",
    "StpMode",
    "Strategy",
    "StrategyRunner",
    "TearsheetReport",
    "TimeTravelSession",
    "Trade",
    "TradePrintRecord",
    "TradesSummary",
    "__version__",
    "build_info",
    "compute_expected_shortfall",
    "compute_hasbrouck_share",
    "compute_lead_lag",
    "compute_optimal_trajectory",
    "compute_permanent_impact",
    "compute_tearsheet_from_csv",
    "compute_temporary_impact",
    "decode_message",
    "first_decision_divergence",
    "frame_length",
    "ouch_inbound_frame_length",
    "ouch_outbound_frame_length",
    "parse_itch",
    "read_log",
    "run_simrun_script",
    "run_trades",
    "sample_corpus",
    "to_pandas",
    "to_parquet",
    "to_polars",
    "trace_hash",
]
