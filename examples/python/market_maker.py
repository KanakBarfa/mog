"""Market-maker: straddle the mid, work scripted flow, report the account.

Deterministic by construction: same script, same digest, every run.
"""

import mog

KEXT = 1 << 62
CENTER = 4_000_000


def main():
    sim = mog.ExecutionSimulator(
        arena_capacity=1 << 16,
        lo_tick=0,
        hi_tick=12_000_000,
        page_pool=256,
        event_capacity=4096,
        seed=7,
        maker_fee_bps=-2,
        taker_fee_bps=5,
    )

    # Two-sided external liquidity around CENTER.
    sim.seed_external(1, "B", 400, CENTER - 20)
    sim.seed_external(2, "B", 300, CENTER - 30)
    sim.seed_external(3, "S", 400, CENTER + 20)
    sim.seed_external(4, "S", 300, CENTER + 30)

    state = {"now": 1_000, "next_ref": KEXT + 1000, "live": []}

    def requote(size=50, half_spread=5):
        """Cancel outstanding quotes and straddle the current mid."""
        for ref in state["live"]:
            sim.cancel_strategy(ref)
        state["live"].clear()
        book = sim.book()
        bid, ask = book.best_bid(), book.best_ask()
        if bid is None or ask is None:
            return None
        mid = (bid + ask) // 2
        for side, px in (("B", mid - half_spread), ("S", mid + half_spread)):
            sim.submit(state["next_ref"], side, size, px, mog.SimOrderType.day_limit, state["now"])
            state["live"].append(state["next_ref"])
            state["next_ref"] += 1
        return mid

    # t=1000: initial quotes rest inside the spread.
    requote()
    sim.advance_time(500)
    sim.drain()
    state["now"] += 500

    bid_ref = state["live"][0]
    print(f"queue ahead of our bid (ref {bid_ref - KEXT}): {sim.queue_ahead_of(bid_ref)}")

    # t=1500: selling pressure works the bid side and lifts our top quote.
    our_bid = sim.book().best_bid()  # our quote is now best bid
    sim.apply_external("B", our_bid, 30)
    sim.drain()
    state["now"] += 500

    fills = sim.reports()
    for f in fills:
        print(f"fill: ts={f.visible_ts} side={f.side} qty={f.qty} px={f.price_ticks} fee={f.fee}")

    # Re-center on the post-flow mid and settle.
    requote(half_spread=6)
    sim.advance_time(500)
    sim.drain()

    inventory = sum(f.qty if f.side == "B" else -f.qty for f in fills)
    cash = sum(-(f.qty * f.price_ticks) if f.side == "B" else f.qty * f.price_ticks for f in fills)
    fees = sum(f.fee for f in fills)

    print(f"fills={len(fills)} inventory={inventory:+d} cash={cash:+d} fees={fees:+d}")
    print(f"trace digest: {sim.trace_digest()}")


if __name__ == "__main__":
    main()
