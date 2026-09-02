# Fidelity guide

What MOG models, exactly, and where fidelity ends. This page is the map;
the linked design notes are the territory.

## Market data: ITCH 5.0 subset

The parser handles message types **A, F, E, C, X, D, U** (add, add with
attribution, execute, execute-with-price, cancel, delete, replace) with the
shared 11-byte big-endian header. Every wire offset is pinned by
`static_assert` layout audits - drift fails the build. See `docs/PARSER.md`
for the byte table.

Framing formats supported: BinaryFILE, MoldUDP64, and zero-copy
Ethernet-IPv4-UDP PCAP packet stream playback.

## The book: full depth-3 semantics

`OrderBook` maintains price-time priority with real FIFO chains per level:

- Orders live in a generational arena (no ABA); refs below
  `external_ref_limit` belong to external liquidity, above it to strategy.
- Replace is atomic (validate-then-mutate): a replace that would violate any
  invariant leaves the book untouched.
- Prices outside the configured `[lo_tick, hi_tick)` band reject with an
  explicit error rather than allocating pages on garbage.
- `audit()` walks every level and both ladders against aggregate state.

Design rationale and evidence trail: `docs/BOOK.md`.

## Execution: fills come from the real FIFO

The simulator layers on the same book - there is no shadow book:

1. Fills happen **only** when observed flow reaches your order as a FIFO
   head. The model never invents quantity.
2. Queue position is exact, not estimated: after every mutation touching a
   level, each tracked order's units-ahead is recomputed by walking the real
   chain (scenario S9 pins stacked positions through sequential partial
   consumptions).
3. Pegged facilities (`midpoint_peg`, `primary_peg`, `market_peg`) dynamically
   recalculate discrete limit tick prices on BBO changes while preserving
   time priority behind displayed depth.
4. Depth-decay power-law depletion models non-uniform cancellation intensity
   away from the touch via $(1 + \text{level})^{-\alpha}$.
5. Icebergs replenish display slices at the tail of the level under
   price-time priority (S10 verifies slice accounting by hand).
6. Decisions inherit stable `(ts, seq)` ordering from the M3 scheduler.
7. Every run folds into a SHA-256 trace digest; identical scripts give
   identical digests across machines and languages.

Specification and verification: `docs/FILLMODEL.md`; the statistical flow
driver and its assumptions are catalogued in
[Model bias reference](model-bias-reference.md).

## Deliberately out of scope

| Not modeled | Why |
|---|---|
| Multi-venue fragmentation/routing | belongs to a venue-graph layer that does not exist yet |
| Hidden size inside displayed orders | no partial-display granularity for externals |
| Regime switching / adverse-selection asymmetry | higher-order flow structure, future work |

The doctrine: each layer answers one question exactly - given visible depth
and flow, what fills and when - instead of many questions approximately.

## Determinism contract

- Fixed 64-bit seeds everywhere (`std::mt19937_64`, splitmix corpus gen).
- No wall-clock, no address-hash dependence in any traced value.
- Parser and book traces are pinned by golden hashes; sim scripts by
  digests; hot kernels additionally by differential disassembly in CI.
- Contracts (`MOG_PRE/MOG_POST`) are enforced in frontier/lab and compiled
  out of portable builds/wheels.
