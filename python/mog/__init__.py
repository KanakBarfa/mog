"""mog - deterministic L3 limit-order-book backtesting engine (Python shim).

Thin nanobind binding over the portable-profile C++ core: ITCH parsing,
order-book replay, and the execution simulator, with byte-exact determinism
guarantees shared across languages (see docs/EVALUATION.md).
"""

from ._core import (
    BookTick,
    Decision,
    ExecutionSimulator,
    FillReport,
    JitterKind,
    Message,
    MogDecodeError,
    MogParseError,
    Orchestrator,
    OrderBook,
    SimOrderType,
    StpMode,
    TimeTravelSession,
    Trade,
    __version__,
    build_info,
    decode_message,
    first_decision_divergence,
    frame_length,
    parse_itch,
    sample_corpus,
    trace_hash,
)

__all__ = [
    "BookTick",
    "Decision",
    "ExecutionSimulator",
    "FillReport",
    "JitterKind",
    "Message",
    "MogDecodeError",
    "MogParseError",
    "Orchestrator",
    "OrderBook",
    "SimOrderType",
    "StpMode",
    "TimeTravelSession",
    "Trade",
    "__version__",
    "build_info",
    "decode_message",
    "first_decision_divergence",
    "frame_length",
    "parse_itch",
    "sample_corpus",
    "trace_hash",
]
