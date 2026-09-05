"""Integration and E2E tests for Apache Arrow C Data Interface, trades reconstruction, tearsheet analytics, and simrun execution."""

import unittest

import mog


class TestArrowAndAnalytics(unittest.TestCase):
    def test_run_trades_and_arrow_export(self):
        with open("data/sample.itch", "rb") as f:
            raw = f.read()
        summary = mog.run_trades(
            raw,
            arena_capacity=1 << 18,
            lo_tick=0,
            hi_tick=12_000_000,
            page_pool=256,
            format="raw",
        )

        self.assertGreater(summary.decoded, 0)
        self.assertEqual(len(summary.records()), summary.prints)

        # Verify Arrow C Data Interface capsules
        sch_cap, arr_cap = summary.to_arrow()
        self.assertIsNotNone(arr_cap)
        self.assertIsNotNone(sch_cap)

        # If PyArrow is installed, verify zero-copy RecordBatch import
        try:
            import pyarrow as pa

            batch = pa.record_batch(summary)
            self.assertEqual(batch.num_rows, summary.prints)
            self.assertEqual(batch.num_columns, 7)
            self.assertIn("match_number", batch.schema.names)
            self.assertIn("price_ticks", batch.schema.names)
            self.assertIn("shares", batch.schema.names)
            self.assertIn("printable", batch.schema.names)
            self.assertIn("from_execute_with_price", batch.schema.names)
        except ImportError:
            pass

        # If Polars is installed, verify DataFrame conversion
        try:
            import polars as pl

            df = pl.from_arrow(summary)
            self.assertEqual(len(df), summary.prints)
            self.assertIn("match_number", df.columns)
            self.assertIn("price_ticks", df.columns)
        except ImportError:
            pass

    def test_simrun_and_tearsheet_pipeline(self):
        script = """kind,ts_ns,side,price_ticks,qty,ref
ext_add,100,B,1998,400,11
ext_add,100,S,2002,380,12
strat_limit,150,B,1997,50,7001
strat_limit,150,S,2003,50,7002
trade,250,S,1998,120,9101
trade,350,B,2002,110,9102
strat_cancel,450,B,1997,1,7001
strat_limit,460,B,1996,50,7003
trade,550,S,1997,30,9103
strat_ioc,650,B,2010,40,7004
"""
        sim_summary = mog.run_simrun_script(
            script,
            arena_capacity=1 << 16,
            lo_tick=0,
            hi_tick=10_000,
            maker_fee_bps=-2,
            taker_fee_bps=5,
        )
        self.assertGreater(sim_summary.fills, 0)
        self.assertNotEqual(sim_summary.digest_high, 0)

        # Build sample CSV event log
        log_csv = """type,ts_ns,ref,side,price_ticks,qty,fee_cash,mid_ticks,ledger
P,100,1,B,2000,100,0,2000,M
F,150,7001,B,1997,50,-2,2000,M
P,250,2,S,1998,120,0,1998,M
F,650,7004,B,2002,40,4,2002,T
P,700,3,B,2005,50,0,2005,M
"""
        tearsheet = mog.compute_tearsheet_from_csv(log_csv)
        self.assertEqual(tearsheet.fills, 2)
        self.assertEqual(tearsheet.prints, 3)
        self.assertEqual(tearsheet.buy_qty, 90)
        self.assertEqual(tearsheet.fees_cash, 2)
        self.assertGreater(len(tearsheet.markouts()), 0)

    def test_simulator_arrow_exports(self):
        sim = mog.ExecutionSimulator(
            arena_capacity=1 << 16, lo_tick=0, hi_tick=10_000, page_pool=64
        )
        sim.seed_external(1, "B", 100, 2000)
        sim.seed_external(2, "S", 100, 2010)
        sim.submit(3, "B", 50, 2005, mog.SimOrderType.day_limit, 100)
        sim.advance_time(500)
        sim.drain()

        sch_rep, arr_rep = sim.reports_to_arrow()
        self.assertIsNotNone(arr_rep)
        self.assertIsNotNone(sch_rep)

        sch_dec, arr_dec = sim.decisions_to_arrow()
        self.assertIsNotNone(arr_dec)
        self.assertIsNotNone(sch_dec)

    def test_python_strategy_runner(self):
        class RecordingStrategy(mog.Strategy):
            def __init__(self):
                super().__init__()
                self.book_updates = []
                self.trades = []
                self.acks = []
                self.fills = []

            def on_order_book_update(self, update):
                self.book_updates.append(update)

            def on_trade(self, trade):
                self.trades.append(trade)

            def on_order_ack(self, ack):
                self.acks.append(ack)

            def on_order_fill(self, fill):
                self.fills.append(fill)

        strat = RecordingStrategy()
        runner = mog.StrategyRunner(strategy=strat, lo_tick=0, hi_tick=10000, page_pool=32)
        runner.seed_external(1, "B", 100, 2000)
        runner.seed_external(2, "S", 100, 2010)

        # Advance time to trigger book update hook
        runner.advance_time(100)
        self.assertGreater(len(strat.book_updates), 0)
        self.assertEqual(strat.book_updates[-1].bid_px, 2000)
        self.assertEqual(strat.book_updates[-1].ask_px, 2010)

        # Submit strategy order
        runner.submit(1001, "B", 20, 2000, mog.SimOrderType.day_limit)
        runner.advance_time(100)
        self.assertGreater(len(strat.acks), 0)

        # Apply external match against strategy resting order
        runner.apply_external("B", 2000, 110)
        runner.advance_time(100)
        self.assertGreater(len(strat.fills), 0)
        self.assertEqual(strat.fills[0].qty, 10)
        self.assertNotEqual(runner.trace_digest(), "")

    def test_read_log_and_arrow_conversion(self):
        import os
        import struct
        import tempfile

        with tempfile.NamedTemporaryFile(suffix=".moglog", delete=False) as tmp:
            tmp_path = tmp.name
        try:
            streams = [("fills", [("oid", 1), ("px", 3), ("qty", 1)])]
            groups = [(0, 2, {"oid": [7, 9], "px": [990, 1010], "qty": [50, 30]})]
            with open(tmp_path, "wb") as f:
                f.write(b"MOGLOG\x01\x00")
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
                        fmt = {0: "<B", 1: "<I", 2: "<Q", 3: "<q", 4: "<d"}[tcode]
                        f.writelines(struct.pack(fmt, v) for v in cols[fname])

            log = mog.read_log(tmp_path)
            self.assertEqual(log.stream_names(), ["fills"])
            self.assertEqual(log["fills"]["oid"], [7, 9])
            self.assertEqual(log["fills"]["px"], [990, 1010])

            try:
                table = log.to_arrow("fills")
                self.assertEqual(table.num_rows, 2)
                self.assertIn("oid", table.column_names)
            except ImportError:
                pass
        finally:
            if os.path.exists(tmp_path):
                os.unlink(tmp_path)

    def test_almgren_chriss_impact(self):
        cfg = mog.ImpactConfig(
            gamma=0.10, eta=0.15, daily_volatility=0.02, daily_volume=1_000_000.0
        )
        perm = mog.compute_permanent_impact(10_000.0, cfg)
        self.assertGreater(perm, 0.0)
        temp = mog.compute_temporary_impact(1_000.0, 60.0, cfg)
        self.assertGreater(temp, 0.0)

        traj = mog.compute_optimal_trajectory(50_000.0, 1800.0, 5, cfg)
        self.assertEqual(len(traj), 6)
        self.assertEqual(traj[0], 50_000.0)
        self.assertAlmostEqual(traj[-1], 0.0, places=2)

        cost = mog.compute_expected_shortfall(traj, 360.0, cfg)
        self.assertGreater(cost, 0.0)

    def test_ouch_gateway_and_framing(self):
        self.assertEqual(mog.ouch_inbound_frame_length("O"), 49)
        self.assertEqual(mog.ouch_inbound_frame_length("U"), 47)
        self.assertEqual(mog.ouch_inbound_frame_length("X"), 19)
        self.assertEqual(mog.ouch_outbound_frame_length("A"), 66)
        self.assertEqual(mog.ouch_outbound_frame_length("E"), 40)
        self.assertEqual(mog.ouch_outbound_frame_length("D"), 37)

        sim = mog.ExecutionSimulator(lo_tick=0, hi_tick=10000)
        gw = mog.OuchGateway(sim)
        self.assertNotEqual(gw.trace_hash(), 0)
        packets = gw.drain_outbound()
        self.assertEqual(len(packets), 0)

    def test_impact_state_dynamics(self):
        cfg = mog.ImpactConfig(decay_half_life_ns=1_000_000_000)
        state = mog.ImpactState()
        state.apply_execution(10_000.0, 10.0, 100, cfg)
        self.assertGreater(state.permanent_shift_ticks, 0.0)
        self.assertGreater(state.temporary_shift_ticks, 0.0)
        self.assertGreater(state.total_impact_ticks(), 0.0)

        initial_temp = state.temporary_shift_ticks
        state.decay_temporary_impact(100 + 1_000_000_000, cfg)
        self.assertAlmostEqual(state.temporary_shift_ticks, initial_temp * 0.5, delta=1e-3)

    def test_orchestrator_portfolio_analytics(self):
        orch = mog.Orchestrator()
        idx0 = orch.add_instrument(
            arena_capacity=1 << 16, lo_tick=0, hi_tick=10000, page_pool=32, name="AAPL"
        )
        idx1 = orch.add_instrument(
            arena_capacity=1 << 16, lo_tick=0, hi_tick=10000, page_pool=32, name="MSFT"
        )

        orch.seed_external(idx0, 1, "B", 100, 2000)
        orch.seed_external(idx0, 2, "S", 100, 2010)
        orch.seed_external(idx1, 3, "B", 100, 3000)
        orch.seed_external(idx1, 4, "S", 100, 3020)

        self.assertEqual(orch.best_bid(idx0), 2000)
        self.assertEqual(orch.best_ask(idx0), 2010)
        self.assertEqual(orch.mid_price(idx0), 2005.0)

        self.assertEqual(orch.best_bid(idx1), 3000)
        self.assertEqual(orch.best_ask(idx1), 3020)
        self.assertEqual(orch.mid_price(idx1), 3010.0)

        orch.run_until_all(1000)
        self.assertEqual(orch.total_fill_count(), 0)
        self.assertEqual(orch.total_filled_notional(), 0)
        self.assertEqual(orch.total_fees(), 0)
        self.assertEqual(orch.pending_decisions(idx0), 0)
        self.assertEqual(orch.queue_ahead_of(idx0, 1), -1)

    def test_fast_digest_acceleration(self):
        d1 = mog.FastDigest128()
        d2 = mog.FastDigest128()
        d1.update(b"FAST_DETERMINISM_TEST_BUFFER")
        d2.update(b"FAST_DETERMINISM_TEST_BUFFER")
        h1 = d1.finish_hex()
        h2 = d2.finish_hex()
        self.assertEqual(h1, h2)
        self.assertEqual(len(h1), 32)

        det_golden = mog.DeterminismDigest(mog.DigestMode.golden)
        det_fast = mog.DeterminismDigest(mog.DigestMode.fast)
        det_golden.update(b"TEST_PAYLOAD")
        det_fast.update(b"TEST_PAYLOAD")
        self.assertEqual(len(det_golden.finish_sha256_hex()), 64)
        self.assertEqual(len(det_fast.finish_fast128_hex()), 32)

        sim_fast = mog.ExecutionSimulator(lo_tick=0, hi_tick=10000, digest_mode=mog.DigestMode.fast)
        sim_fast.seed_external(1, "S", 100, 2000)
        sim_fast.submit(2, "B", 100, 2000, mog.SimOrderType.day_limit, 100)
        sim_fast.advance_time(50)
        sim_fast.drain()
        fast_hex = sim_fast.fast_trace_digest()
        self.assertEqual(len(fast_hex), 32)
        self.assertNotEqual(fast_hex, "0" * 32)

    def test_parse_apply_parity(self):
        data = mog.sample_corpus(
            seed=0xBEEF, count=10_000, price_center=4_000_000, price_span=400_000
        )

        def fresh():
            return mog.OrderBook(
                arena_capacity=1 << 20, lo_tick=0, hi_tick=12_000_000, page_pool=64
            )

        a, b = fresh(), fresh()
        n1 = mog.parse_itch(data, a.apply)
        n2 = mog.parse_apply(data, b)
        self.assertEqual(n1, n2)
        self.assertEqual(a.best_bid(), b.best_bid())
        self.assertEqual(a.best_ask(), b.best_ask())
        self.assertEqual(a.l2(), b.l2())
        c = fresh()
        self.assertEqual(mog.parse_apply(data, c), n2)
        self.assertEqual(c.l2(), b.l2())
        with self.assertRaises(mog.MogParseError):
            mog.parse_apply(data[:100], fresh())

    def test_digest_mode_refusal_and_taint(self):
        sim_fast = mog.ExecutionSimulator(lo_tick=0, hi_tick=10000, digest_mode=mog.DigestMode.fast)
        sim_fast.seed_external(1, "S", 100, 2000)
        sim_fast.submit(2, "B", 100, 2000, mog.SimOrderType.day_limit, 100)
        sim_fast.drain()
        with self.assertRaises(ValueError):
            sim_fast.trace_digest()
        self.assertEqual(len(sim_fast.fast_trace_digest()), 32)

        sim_golden = mog.ExecutionSimulator(lo_tick=0, hi_tick=10000)
        with self.assertRaises(ValueError):
            sim_golden.fast_trace_digest()
        self.assertEqual(len(sim_golden.trace_digest()), 64)

        script = """kind,ts_ns,side,price_ticks,qty,ref
ext_add,100,B,1998,400,11
ext_add,100,S,2002,380,12
strat_limit,150,B,1997,50,7001
trade,250,S,1998,120,9101
"""
        fast_summary = mog.run_simrun_script(
            script, lo_tick=0, hi_tick=10_000, digest_mode=mog.DigestMode.fast
        )
        self.assertEqual(fast_summary.digest_mode, mog.DigestMode.fast)
        golden_summary = mog.run_simrun_script(script, lo_tick=0, hi_tick=10_000)
        self.assertEqual(golden_summary.digest_mode, mog.DigestMode.golden)
        self.assertNotEqual(fast_summary.digest_high, 0)

    def test_parquet_export(self):
        import os
        import tempfile

        with open("data/sample.itch", "rb") as f:
            raw = f.read()
        summary = mog.run_trades(
            raw,
            arena_capacity=1 << 18,
            lo_tick=0,
            hi_tick=12_000_000,
            page_pool=256,
            format="raw",
        )
        with tempfile.TemporaryDirectory() as tmpdir:
            out_pq = os.path.join(tmpdir, "trades.parquet")
            mog.to_parquet(summary, out_pq, compression="zstd")
            self.assertTrue(os.path.exists(out_pq))
            self.assertGreater(os.path.getsize(out_pq), 0)

    def test_lead_lag_and_hasbrouck_metrics(self):
        # Create synthetic series where series 1 leads series 2 by 2 periods
        s1 = [0.0, 0.0, 1.0, 5.0, 2.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0]
        s2 = [0.0, 0.0, 0.0, 0.0, 1.0, 5.0, 2.0, 0.0, 0.0, 0.0, 0.0, 0.0]
        res = mog.compute_lead_lag(s1, s2, max_lag=4)
        self.assertEqual(res.optimal_lag, 2)
        self.assertAlmostEqual(res.max_correlation, 1.0, places=4)

        # Compute Hasbrouck Information Share bounds
        share = mog.compute_hasbrouck_share(var1=0.04, var2=0.04, cov12=0.02)
        self.assertGreaterEqual(share.lower_bound_1, 0.0)
        self.assertLessEqual(share.upper_bound_1, 1.0)
        self.assertAlmostEqual(share.mid_share_1 + share.mid_share_2, 1.0, places=5)


if __name__ == "__main__":
    unittest.main()
