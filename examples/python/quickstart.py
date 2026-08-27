"""Quickstart: parse a deterministic corpus, build the book, run a mini sim.

Run against an installed wheel (pip install mog) or the in-tree module.
"""

import mog


def main():
    # 1. Deterministic synthetic capture, or bring real ITCH bytes.
    data = mog.sample_corpus(seed=0xBEEF, count=100_000, price_center=4_000_000, price_span=400_000)

    # 2. Stream-parse; every record lands in your callback.
    book = mog.OrderBook(arena_capacity=1 << 20, lo_tick=0, hi_tick=12_000_000, page_pool=512)

    applied = 0

    def sink(msg):
        nonlocal applied
        if book.apply(msg).ok:
            applied += 1

    consumed = mog.parse_itch(data, sink)
    assert consumed == len(data), "parser stopped early"

    print(f"parsed {consumed} bytes; {applied} order-lifecycle events accepted")
    print(f"top of book: bid={book.best_bid()} ask={book.best_ask()} live={book.live_orders()}")
    print(f"L2 depth levels: {len(book.l2())}")

    # 3. Execution simulation with byte-exact determinism.
    sim = mog.ExecutionSimulator(
        arena_capacity=1 << 16, lo_tick=0, hi_tick=12_000_000, page_pool=256, event_capacity=4096
    )
    KEXT = 1 << 62
    sim.seed_external(9, "S", 70, 4_010_000)
    sim.submit(KEXT + 50, "B", 25, 4_010_001, mog.SimOrderType.day_limit, 5)
    sim.advance_time(500)
    sim.drain()

    for f in sim.reports():
        print(f"fill: ref={f.ref} side={f.side} qty={f.qty} px={f.price_ticks} fee={f.fee}")
    print(f"trace digest: {sim.trace_digest()}")


if __name__ == "__main__":
    main()
