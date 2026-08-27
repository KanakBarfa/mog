# MOG documentation site

Deterministic L3 limit-order-book backtesting engine - start here.

## Start

- [Getting started](getting-started.md) - build, install the wheel, run a
  full experiment in C++ or Python.

## Trust

- [Fidelity guide](fidelity-guide.md) - what is modeled, exactly, and what
  is deliberately out of scope.
- [Model bias reference](model-bias-reference.md) - every known way the
  execution model is wrong, on one page.
- [Benchmark methodology](benchmark-methodology.md) - how numbers are
  produced and gated; reproduce them yourself.

## Understand

- [C++26 ledger explainer](cpp26-ledger-explainer.md) - features, probes,
  fallbacks, and why wheels build on stock toolchains.
- [Knowledge Base & Algorithms Reference](../knowledge_base/README.md) - complete 12-chapter
  breakdown of market microstructure, systems engineering, Big-O complexity, and hardware optimizations.

Design notes (deeper, component-level): `docs/PARSER.md`,
`docs/BOOK.md`, `docs/SCHEDULER.md`, `docs/API.md`, `docs/FILLMODEL.md`,
`docs/EVALUATION.md`, `docs/COMPATIBILITY.md`.
