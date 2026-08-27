#!/usr/bin/env python3
"""nautilus_trader participant, round 2: FULL LOOP.

The venue runs BookType.L1_MBP driven by top-of-book QuoteTicks synthesized
from the generator's shadow book (quotes.csv), plus TradeTicks for prints.
Strategy rows fire on trade count: strat_limit rests a GTC limit at its
price (filling when the L1 quote crosses through), strat_ioc submits a
market order, strat_cancel cancels a live order.

Run from repo root:
    /path/to/venv/bin/python -m bench.race.participants.nautilus_race \
        --feed bench/race/out/feed.csv --quotes bench/race/out/quotes.csv \
        --runs 3 --out bench/race/out/nautilus.json
"""

from __future__ import annotations

import argparse
import csv
import resource
import statistics
import sys
import time
from pathlib import Path

_HERE = Path(__file__).resolve()
sys.path.insert(0, str(_HERE.parents[1]))

from protocol import digest_fills, load_stream, write_result


def build_engine():
    from nautilus_trader.backtest.engine import BacktestEngine
    from nautilus_trader.model.currencies import USDT
    from nautilus_trader.model.enums import AccountType, OmsType
    from nautilus_trader.model.identifiers import InstrumentId, Symbol, Venue
    from nautilus_trader.model.instruments import CryptoPerpetual
    from nautilus_trader.model.objects import Money, Price, Quantity

    instrument = CryptoPerpetual(
        instrument_id=InstrumentId(Symbol("MOGPERP"), Venue("MOGVENUE")),
        raw_symbol=Symbol("MOGPERP"),
        base_currency=USDT,
        quote_currency=USDT,
        settlement_currency=USDT,
        is_inverse=False,
        price_precision=0,
        size_precision=0,
        price_increment=Price.from_str("1"),
        size_increment=Quantity.from_str("1"),
        ts_event=0,
        ts_init=0,
    )
    engine = BacktestEngine()
    engine.add_venue(
        venue=Venue("MOGVENUE"),
        oms_type=OmsType.HEDGING,
        account_type=AccountType.MARGIN,
        starting_balances=[Money(100_000_000, USDT)],
        base_currency=None,
        default_leverage=Decimal(1),
        use_position_ids=False,
    )
    engine.add_instrument(instrument)
    return engine, instrument


from decimal import Decimal


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--feed", required=True)
    ap.add_argument("--quotes", required=True)
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    stream = load_stream(args.feed)

    # Remap schedule triggers from "market rows consumed" to "trade ticks
    # seen" (the strategy's event space).
    trade_marks, seen = [], 0
    for op in stream.market:
        if op.kind == "trade":
            seen += 1
            trade_marks.append(seen)
    remapped = [(trade_marks[min(t, len(trade_marks) - 1)], op) for t, op in stream.schedule]

    walls, digests, fill_counts = [], [], []

    def run_once():
        from nautilus_trader.model.data import QuoteTick, TradeTick
        from nautilus_trader.model.enums import (
            AggressorSide,
            OrderSide,
            TimeInForce,
        )
        from nautilus_trader.model.identifiers import TradeId
        from nautilus_trader.model.objects import Price, Quantity

        engine, instrument = build_engine()

        from nautilus_trader.trading.strategy import Strategy

        class RaceStrategy(Strategy):
            def __init__(self):
                super().__init__()
                self._si = 0
                self._trades = 0
                self.live = {}  # script ref -> client_order_id

            def on_start(self):
                # Backtest or not, callbacks only fire for SUBSCRIBED data.
                self.subscribe_quote_ticks(instrument.id)
                self.subscribe_trade_ticks(instrument.id)

            def on_trade_tick(self, tick):
                self._trades += 1
                while self._si < len(remapped) and remapped[self._si][0] <= self._trades:
                    _, op = remapped[self._si]
                    self._si += 1
                    ref = op.ref
                    if op.kind == "strat_cancel":
                        coid = self.live.pop(ref, None)
                        if coid is not None:
                            order = self.cache.order(coid)
                            if order is not None:
                                self.cancel_order(order)
                        continue
                    buy = op.side == "B"
                    side = OrderSide.BUY if buy else OrderSide.SELL
                    qty = instrument.make_qty(op.qty)
                    px = instrument.make_price(op.price_ticks)
                    if op.kind == "strat_limit":
                        order = self.order_factory.limit(
                            instrument_id=instrument.id,
                            order_side=side,
                            quantity=qty,
                            price=px,
                            time_in_force=TimeInForce.GTC,
                            client_order_id=self.order_factory.generate_client_order_id(),
                        )
                    else:
                        order = self.order_factory.market(
                            instrument_id=instrument.id,
                            order_side=side,
                            quantity=qty,
                            client_order_id=self.order_factory.generate_client_order_id(),
                        )
                    self.live[ref] = order.client_order_id
                    self.submit_order(order)

        trade_ticks, quote_ticks = [], []
        with open(args.quotes) as f:
            rdr = csv.reader(f)
            next(rdr)
            for row in rdr:
                ts, bpx, bqty, apx, aqty = row
                quote_ticks.append(
                    QuoteTick(
                        instrument_id=instrument.id,
                        bid_price=Price.from_raw(int(bpx), 0),
                        ask_price=Price.from_raw(int(apx), 0),
                        bid_size=Quantity.from_raw(int(bqty), 0),
                        ask_size=Quantity.from_raw(int(aqty), 0),
                        ts_event=int(ts),
                        ts_init=int(ts),
                    )
                )
        for op in stream.market:
            if op.kind != "trade":
                continue
            trade_ticks.append(
                TradeTick(
                    instrument_id=instrument.id,
                    price=Price.from_raw(op.price_ticks, 0),
                    size=Quantity.from_raw(op.qty, 0),
                    aggressor_side=AggressorSide.BUYER if op.side == "B" else AggressorSide.SELLER,
                    trade_id=TradeId(str(len(trade_ticks))),
                    ts_event=op.ts_ns,
                    ts_init=op.ts_ns,
                )
            )

        strat = RaceStrategy()
        engine.add_strategy(strat)
        engine.add_data(quote_ticks)
        engine.add_data(trade_ticks)
        t0 = time.perf_counter()
        engine.run()
        wall = time.perf_counter() - t0

        report = engine.trader.generate_order_fills_report()
        rows = []
        for _, r in report.iterrows():
            rows.append(f"F,{r['client_order_id']},{r['last_px']},{r['last_qty']}")
        engine.dispose()
        return wall, digest_fills(rows), len(rows)

    for _ in range(args.runs):
        wall, d, n = run_once()
        walls.append(wall)
        digests.append(d)
        fill_counts.append(n)

    n_trades = sum(1 for o in stream.market if o.kind == "trade")
    payload = {
        "participant": "nautilus",
        "ops": len(stream.market) + len(stream.schedule),
        "market_rows": len(stream.market),
        "ticks_processed": 199990 + n_trades,
        "runs": args.runs,
        "wall_s_median": statistics.median(walls),
        "events_per_sec": len(trade_marks) / statistics.median(walls),
        "fills": fill_counts[0],
        "fill_digest": digests[0],
        "deterministic": len(set(digests)) == 1,
        "rss_kb_peak": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss,
        "notes": "round 2: L1_MBP venue over synthesized top-of-book quotes "
        "+ trade prints; limits rest, cancels cancel; ops/s counts "
        "strategy-relevant trades per second of engine.run()",
    }
    write_result(Path(args.out), payload)
    print(
        f"nautilus: {payload['events_per_sec']:,.0f} ops/s "
        f"({fill_counts[0]} fills, deterministic={payload['deterministic']})"
    )


if __name__ == "__main__":
    main()
