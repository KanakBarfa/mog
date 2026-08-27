# Chapter 00: Engine Overview & Core Architecture

**What you will learn:** what mog is, why "same input, same output" is the
whole point, and how the major components connect.

---

## 1. The big picture: a flight simulator for stock trading

A pilot trains in a flight simulator: real physics, no risk, every flight
repeatable so instructors can say "do exactly what you did last time, but
raise the nose earlier". mog is that for stock trading.

It takes a recording of a real exchange day (NASDAQ's ITCH feed - see
chapter 03) and replays it event by event. A strategy you write buys and
sells against this replayed market, and mog reports exactly what would
have happened: which orders filled, when, at what price, and what profit
or loss resulted.

The simulator promise that makes this useful is **determinism**: the same
recording plus the same settings produces bit-for-bit identical results
every run. No randomness you did not ask for, no "it worked yesterday".
That property is what lets you compare two strategies fairly, or prove
that a code change did not silently alter behavior.

## 2. How the components connect

```
     [ recorded NASDAQ ITCH file ]        the raw recording
                |
                v
     [ ITCHParser  ]                     decodes binary messages (ch 03, 06)
                |
                v
     [ OrderBook / PriceLadder ]         the live list of all orders (ch 01, 05)
      |-- Arena allocator                pre-allocated memory (ch 04)
      '-- per-price FIFO queues          first-in-first-out waiting lines
                |
                v
     [ ExecutionSimulator ]              decides when YOUR orders fill (ch 07)
      |-- queue position tracking
      |-- latency + jitter modeling
      |-- icebergs, pegged orders, STP   special order types (ch 02)
                |
                v
     [ Strategy Harness (CRTP) ]         where your trading logic plugs in (ch 08)
                |
                v
     [ ColumnLog + Tearsheet ]           event log and profit report (ch 09, 10)
```

Reading bottom-up: your strategy reacts to book updates; the execution
simulator decides fills using queue mechanics; the order book tracks every
resting order; the parser feeds it decoded exchange messages.

## 3. Determinism, made concrete

Two runs of the same scenario must agree on every fill, every price,
every byte of the output log. mog enforces this with:

- **No hidden randomness**: random models exist (chapter 07), but each
  uses an explicit seed you control.
- **Trace hashing**: the engine continuously hashes its state with
  SHA-256 (the same cryptographic function used to fingerprint files).
  The final digest acts like a tamper-evident seal: change one tick of
  behavior anywhere and the hash changes.
- **Golden anchors in CI**: continuous-integration jobs pin known-good
  digests, so any accidental behavior change fails the build loudly.
  See [Sha256.hpp](../include/mog/Sha256.hpp) and DESIGN.md.

## 4. The ranked doctrine

Engineering involves trade-offs; mog resolves them by a fixed ranking
(live in [DESIGN.md](../DESIGN.md)). Short form:

1. Correctness before speed before features.
2. Every approximation is documented with its bias - silent shortcuts are bugs.
3. Determinism is absolute.
4. No cleverness without proof: exotic optimizations need a measured win of at least 15% and a plain-C++ twin proven identical by tests.
5. Reproducibility: published numbers always ship with hardware specs, data, and scripts.
6. Boring interfaces, exotic internals.
7. The newest C++ features are for contributors only; end users can consume artifacts on old tools.

## 5. Build profiles: one engine, three doors

Configured with CMake (`-DMOG_PROFILE=...`):

| Profile | Who it is for | What changes |
|---|---|---|
| `portable` | Users, packagers | Conservative dialect (C++23 fallback), checks off; builds almost anywhere |
| `frontier` | CI, releases, benchmarks | Full C++26, `-march=native`, contract checks **enforced**, strict warnings |
| `lab` | Contributors | Adds experimental compiler probes (static reflection) used to generate code |

The point of the ladder: contributors get the sharpest tools, while anyone
downloading the library never needs them. Contracts mentioned above are
`pre`/`post` condition checks - guardrail statements inside the code that
verify assumptions at runtime when enforcement is on (chapter 10,
[Contracts.hpp](../include/mog/Contracts.hpp)).
