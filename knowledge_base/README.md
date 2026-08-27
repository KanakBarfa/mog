# The mog Knowledge Base

A complete, plain-language tour of every idea - finance and programming -
used inside **mog**, a deterministic stock-market simulator.

## Who this is for

You do not need a finance degree or a computer science background. Every
chapter starts with an everyday analogy before showing the precise
technical detail. If you already know the basics, skim the analogies and
read the technical sections.

## What mog is, in one paragraph

mog replays a recorded day of stock-market activity - every order placed,
changed, cancelled, and executed on an exchange - and lets you test
trading strategies against it. Its defining promise is **determinism**:
run the same recording through twice and you get bit-for-bit identical
results, down to the last share and cent. That turns backtesting from
"roughly plausible" into "exactly repeatable", which is what makes results
trustable.

## How to read this

Three paths through the eleven chapters:

- **Curious about markets, new to code:** 01, 02, 07, 08 explain how
  markets, fills, fees, and profits work. Then browse whatever else looks
  interesting.
- **Programmer new to finance:** 01 and 02 give you the market model;
  03-06 and 09 are the systems engineering; 10 maps it all to files.
- **Full tour:** read in order, 00 through 10.

Each chapter follows the same shape: plain English first, exact detail
second, links into the real source code last.

## Chapters

| # | Chapter | You will learn |
|---|---------|----------------|
| 00 | [Overview & Architecture](00_overview_and_architecture.md) | What the engine is, why determinism matters, how the pieces fit |
| 01 | [Market Microstructure & L3 Books](01_market_microstructure_and_l3_book.md) | What an order book is; why mog watches every single order |
| 02 | [Order Types, STP & Auctions](02_exchange_facilities_and_auctions.md) | Icebergs, pegged orders, self-trade prevention, opening crosses |
| 03 | [Market Data Protocols](03_market_data_protocols_and_framing.md) | How exchanges transmit data in raw binary, and how we decode it |
| 04 | [Zero-Allocation Memory](04_zero_allocation_and_memory_architecture.md) | Why we never ask the OS for memory mid-replay, and what we do instead |
| 05 | [Price Ladders & Hash Tables](05_paged_price_ladders_and_hashtables.md) | The two data structures that make lookups instant |
| 06 | [SIMD, SWAR & ISA Dispatch](06_simd_swar_and_isa_dispatch.md) | Making CPUs chew eight bytes per instruction, safely on any machine |
| 07 | [Execution Simulation](07_execution_simulation_and_microstructure_modeling.md) | How a simulated exchange decides when your order gets filled |
| 08 | [Strategy Harness & PnL](08_strategy_harness_and_pnl_analytics.md) | How strategies plug in; how profit, fees, and execution quality are measured |
| 09 | [Scheduling & Tooling](09_discrete_event_scheduling_and_tooling.md) | Timing wheels, lock-free queues, time-travel debugging, Python bindings |
| 10 | [Codebase Map](10_codebase_mapping_and_build_roadmap.md) | Every file in the repo explained, grouped by subsystem |
| 11 | [Algorithms & Complexity Reference](11_algorithms_complexity_and_reference.md) | In-depth Big-O analysis, hardware mechanics, and naive comparisons |

## Mini-glossary

Terms used everywhere in these chapters:

- **Order**: an instruction to buy or sell a set number of shares at a set price or better.
- **Book (order book)**: the live list of all waiting buy orders (bids) and sell orders (asks).
- **Fill / execution**: an order (or part of it) actually trading.
- **Tick**: the smallest price step a venue allows; mog stores prices as integer counts of $0.0001.
- **Latency**: delay between cause and effect - your order leaving your server and reaching the exchange.
- **Determinism**: same input plus same settings always produce exactly the same output.
- **O(1)** ("order one"): computer-science shorthand for "takes constant time, no matter how much data".
- **Arena**: a large block of memory carved up manually, avoiding slow on-demand allocation.

Deeper terms are defined where they first appear; each chapter also links
the header file that implements its ideas.
