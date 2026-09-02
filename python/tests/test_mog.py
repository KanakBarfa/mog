"""Cross-language parity tests for the mog nanobind shim.

The parser-golden constants and the simulator trace digest below are the
same anchors pinned by the C++ test suite (tests/parser_golden.cpp,
tests/sim_scenarios.cpp S8). Passing here means the Python shim produces
byte-identical results to native replay.
"""

import pathlib
import unittest

import mog

REPO = pathlib.Path(__file__).resolve().parents[2]

# Anchors from tests/parser_golden.cpp.
SAMPLE_HASH = 0x308E35B105CFE9D0
SAMPLE_COUNT = 96
FNV_BASIS = 0xCBF29CE484222325

# Anchor from tests/sim_scenarios.cpp scenario S8 (trace_digest of the fixed
# script: seed_external + two submits with time advancement + external flow).
S8_DIGEST = "498cda3c71a74cb86fd569bb6c4b7e0c3e55ba71b61982bb786b3fc3ce642a6e"

KEXT = 1 << 62


def run_s8():
    sim = mog.ExecutionSimulator(
        arena_capacity=256, lo_tick=0, hi_tick=4000, page_pool=8, event_capacity=1024
    )
    assert sim.seed_external(9, "S", 70, 1000).ok
    sim.submit(KEXT + 50, "B", 25, 1001, mog.SimOrderType.day_limit, 5)
    sim.advance_time(500)
    sim.submit(KEXT + 51, "B", 45, 1002, mog.SimOrderType.ioc, 600)
    sim.advance_time(700)
    sim.drain()
    sim.apply_external("S", 1000, 30)
    return sim.trace_digest()


class BuildInfo(unittest.TestCase):
    def test_identity(self):
        info = mog.build_info()
        self.assertEqual(info["version"], mog.__version__)
        self.assertIn(info["profile"], ("portable", "frontier", "lab"))
        self.assertTrue(info["compiler"])


class ParserParity(unittest.TestCase):
    def setUp(self):
        path = REPO / "data" / "sample.itch"
        if not path.exists():
            self.skipTest("repo checkout required (no data/sample.itch)")
        self.data = path.read_bytes()

    def test_golden_hash_and_count(self):
        count = 0
        state = FNV_BASIS

        def sink(msg):
            nonlocal count, state
            count += 1
            state = mog.trace_hash(msg, state)

        consumed = mog.parse_itch(self.data, sink)
        self.assertEqual(consumed, len(self.data))
        self.assertEqual(count, SAMPLE_COUNT)
        self.assertEqual(state, SAMPLE_HASH)

    def test_parse_error_raises_with_offset(self):
        truncated = self.data[:20]
        with self.assertRaises(mog.MogParseError) as ctx:
            mog.parse_itch(truncated, lambda m: None)
            _ = ctx.exception.offset  # attribute exists


class Decode(unittest.TestCase):
    def setUp(self):
        self.corpus = mog.sample_corpus(
            seed=0xBEEF, count=16, price_center=4_000_000, price_span=400_000
        )

    def test_frame_walk_and_decode(self):
        off = 0
        types = []
        while off < len(self.corpus):
            t = chr(self.corpus[off])
            length = mog.frame_length(t)
            self.assertIsNotNone(length)
            msg = mog.decode_message(self.corpus[off : off + length])
            self.assertEqual(msg.type, t)
            if t in ("A", "F"):
                self.assertIn(msg.side, ("B", "S"))
                self.assertTrue(3 <= len(msg.stock) <= 8)
            types.append(t)
            off += length
        # CorpusGen cycles A,F,E,C,X,D,U in order.
        self.assertEqual(types[:7], ["A", "F", "E", "C", "X", "D", "U"])

    def test_unknown_frame_length_is_none(self):
        self.assertIsNone(mog.frame_length("Z"))

    def test_corpus_deterministic(self):
        again = mog.sample_corpus(seed=0xBEEF, count=16, price_center=4_000_000, price_span=400_000)
        other = mog.sample_corpus(
            seed=0xBEEFF, count=16, price_center=4_000_000, price_span=400_000
        )
        self.assertEqual(self.corpus, again)
        self.assertNotEqual(self.corpus, other)


class Book(unittest.TestCase):
    def make_book(self):
        return mog.OrderBook(arena_capacity=256, lo_tick=0, hi_tick=12_000_000, page_pool=64)

    def test_add_execute_cancel_lifecycle(self):
        book = self.make_book()
        tick = book.add(1, "B", 50, 9_980_000)
        self.assertTrue(tick.ok)
        self.assertEqual(book.best_bid(), 9_980_000)
        self.assertEqual(book.qty_at("B", 9_980_000), 50)

        self.assertTrue(book.execute(1, 20).ok)
        self.assertEqual(book.remaining_of(1), 30)

        self.assertTrue(book.cancel(1, 30).ok)
        self.assertEqual(book.remaining_of(1), 0)
        self.assertIsNone(book.best_bid())
        self.assertTrue(book.audit())

    def test_replace_atomicity(self):
        book = self.make_book()
        book.add(2, "S", 10, 10_020_000)
        tick = book.replace(2, 3, 15, 10_025_000)
        self.assertTrue(tick.ok)
        self.assertEqual(book.best_ask(), 10_025_000)
        self.assertEqual(book.live_orders(), 1)

    def test_out_of_band_rejection(self):
        book = self.make_book()
        tick = book.add(4, "B", 5, 99_999_999)  # above hi_tick=12M band
        self.assertFalse(tick.ok)
        self.assertEqual(tick.error, 5)  # BookError::out_of_band

    def test_apply_message_route(self):
        corpus = mog.sample_corpus(seed=7, count=14, price_center=4_000_000, price_span=400_000)
        book = self.make_book()
        off = 0
        applied = 0
        while off < len(corpus):
            t = chr(corpus[off])
            n = mog.frame_length(t)
            msg = mog.decode_message(corpus[off : off + n])
            tick = book.apply(msg)
            self.assertIsInstance(tick, mog.BookTick)
            applied += 1
            off += n
        self.assertEqual(applied, 14)
        self.assertTrue(book.audit())
        rows = book.l2()
        for side, _px, qty in rows:
            self.assertIn(side, ("B", "S"))
            self.assertGreater(qty, 0)


class SimulatorParity(unittest.TestCase):
    def test_s8_digest_matches_cpp_anchor(self):
        self.assertEqual(run_s8(), S8_DIGEST)

    def test_s8_digest_deterministic(self):
        self.assertEqual(run_s8(), run_s8())

    def test_decisions_reports_shapes(self):
        # Aggressive leg: crossing day-limit fills fully; surfaces as a
        # decision, not a passive fill report.
        sim = mog.ExecutionSimulator(
            arena_capacity=256, lo_tick=0, hi_tick=4000, page_pool=8, event_capacity=1024
        )
        assert sim.seed_external(9, "S", 70, 1000).ok
        sim.submit(KEXT + 50, "B", 25, 1001, mog.SimOrderType.day_limit, 5)
        sim.drain()
        decisions = sim.decisions()
        self.assertEqual(len(decisions), 1)
        d = decisions[0]
        self.assertEqual(d.kind, 1)  # fully filled
        self.assertEqual(d.filled_qty, 25)
        self.assertEqual(sim.reports(), [])
        self.assertTrue(sim.audit())

    def test_passive_fill_report(self):
        # Passive leg: resting order joined by external flow mirrors C++
        # tests/sim_scenarios.cpp S1 exactly.
        sim = mog.ExecutionSimulator(
            arena_capacity=256, lo_tick=0, hi_tick=4000, page_pool=8, event_capacity=1024
        )
        self.assertTrue(sim.seed_external(1, "B", 50, 998).ok)
        sim.submit(KEXT + 1, "B", 20, 998, mog.SimOrderType.day_limit, 100)
        sim.drain()
        self.assertEqual(sim.book().qty_at("B", 998), 70)

        sim.apply_external("B", 998, 30)  # eats the 50 ahead, not ours
        self.assertEqual(sim.book().remaining_of(KEXT + 1), 20)

        sim.apply_external("B", 998, 25)  # clears last 20 ahead, fills 5
        self.assertEqual(sim.book().remaining_of(KEXT + 1), 15)
        reports = sim.reports()
        self.assertGreaterEqual(len(reports), 1)
        last = reports[-1]
        self.assertEqual(last.ref, KEXT + 1)
        self.assertEqual(last.price_ticks, 998)
        self.assertEqual(last.qty, 5)


class TimeTravelParity(unittest.TestCase):
    KEXT = 1 << 62

    def session(self):
        return mog.TimeTravelSession(
            arena_capacity=256, lo_tick=0, hi_tick=4000, page_pool=8, event_capacity=1024
        )

    def adversarial(self):
        # Mirrors tests/timetravel_e2e.cpp: resting victim, external
        # depletion, then a same-account attack that dies on cancel-newest.
        # External liquidity refs sit BELOW external_ref_limit.
        s = self.session()
        self.assertTrue(s.seed_iceberg(1, "S", 1002, 30, 90).ok)
        self.assertTrue(s.seed_external(2, "S", 40, 1003).ok)
        s.set_stp_mode(mog.StpMode.cancel_newest)
        rest = self.KEXT + 10
        s.submit(rest, "B", 900, 998, mog.SimOrderType.day_limit, 100)
        s.drain()
        s.apply_external("B", 998, 200)
        s.advance_time(1000)
        s.submit(self.KEXT + 11, "S", 150, 998, mog.SimOrderType.ioc, 500)
        s.drain()
        return s

    def test_replay_determinism_and_report_stability(self):
        a = self.adversarial()
        b = self.adversarial()
        self.assertEqual(a.sim().trace_digest(), b.sim().trace_digest())
        ra, rb = a.export_report(), b.export_report()
        self.assertEqual(ra, rb)
        self.assertTrue(ra.startswith("# mog time-travel report"))
        self.assertIn("rejected_stp", ra)
        self.assertIn("## book L2", ra)

    def test_seek_equivalence(self):
        base = self.adversarial()
        final = base.sim().trace_digest()
        for k in range(base.op_count() + 1):
            base.seek(k)
            self.assertEqual(base.cursor(), k)
        base.seek(base.op_count())
        self.assertEqual(base.sim().trace_digest(), final)
        self.assertTrue(base.audit())

    def test_counterfactual_fork_divergence(self):
        base = self.adversarial()
        # Control: identical suffix reproduces the base trace exactly.
        control = base.fork_at(5)
        self.assertEqual(control.cursor(), 5)
        control.apply_external("B", 998, 200)
        control.advance_time(1000)
        control.submit(self.KEXT + 11, "S", 150, 998, mog.SimOrderType.ioc, 500)
        control.drain()
        self.assertEqual(control.sim().trace_digest(), base.sim().trace_digest())
        self.assertEqual(mog.first_decision_divergence(control, base), -1)

        # Divergence: over-deplete so the victim dies; the attack then
        # survives as partial_then_cancelled instead of rejected_stp.
        divergent = base.fork_at(5)
        divergent.apply_external("B", 998, 950)
        divergent.advance_time(1000)
        divergent.submit(self.KEXT + 11, "S", 150, 998, mog.SimOrderType.ioc, 500)
        divergent.drain()
        self.assertNotEqual(divergent.sim().trace_digest(), base.sim().trace_digest())
        idx = mog.first_decision_divergence(divergent, base)
        self.assertGreaterEqual(idx, 0)
        kinds_before = [d.kind for d in divergent.sim().decisions()[:idx]]
        self.assertEqual([d.kind for d in base.sim().decisions()[:idx]], kinds_before)
        self.assertNotEqual(divergent.sim().decisions()[idx].kind, base.sim().decisions()[idx].kind)

    def test_inspect_after_rewind(self):
        s = self.adversarial()
        before_drain = s.op_count() - 1
        s.seek(before_drain)
        rest = self.KEXT + 10
        self.assertEqual(s.queue_ahead_of(rest), 0)
        self.assertEqual(s.sim().book().remaining_of(rest), 700)
        rows = {tick: qty for _, tick, qty in s.l2_rows()}
        self.assertEqual(rows.get(998), 700)
        names = s.op_names()
        self.assertEqual(names[0], "seed_iceberg")
        self.assertEqual(names[-1], "drain")


class OrchestrationParity(unittest.TestCase):
    KEXT = 1 << 62

    def config_kwargs(self, seed=1):
        return {
            "arena_capacity": 256,
            "lo_tick": 0,
            "hi_tick": 4000,
            "page_pool": 8,
            "event_capacity": 1024,
            "seed": seed,
        }

    def drive(self, o, idx):
        # Mirrors tests/orchestrate_e2e.cpp drive(): per-instrument refs so
        # the same values may legally repeat across instruments.
        ext = self.KEXT + idx * 100 + 1
        strat = self.KEXT + idx * 100 + 2
        self.assertTrue(o.seed_external(idx, ext, "S", 50, 1000).ok)
        o.submit(idx, strat, "B", 30, 999, mog.SimOrderType.day_limit, 10)
        o.drain(idx)
        o.advance_time(idx, 500)
        o.apply_external(idx, "S", 1000, 20)

    def test_replay_determinism_and_fold_identity(self):
        a, b = mog.Orchestrator(), mog.Orchestrator()
        ia = a.add_instrument(name="ES", **self.config_kwargs())
        ib = b.add_instrument(name="ES", **self.config_kwargs())
        self.assertEqual(ia, ib)
        self.drive(a, ia)
        self.drive(b, ib)
        self.assertEqual(a.global_digest(), b.global_digest())
        self.assertTrue(a.audit())
        # Portfolio identity covers every member, touched or not.
        b.add_instrument(name="NQ", **self.config_kwargs())
        self.assertNotEqual(a.global_digest(), b.global_digest())

    def test_isolation_against_solo_simulator(self):
        o = mog.Orchestrator()
        o.add_instrument(**self.config_kwargs())
        i1 = o.add_instrument(**self.config_kwargs())
        self.drive(o, 0)
        self.drive(o, i1)

        solo = mog.ExecutionSimulator(**self.config_kwargs())
        self.assertTrue(solo.seed_external(i1 * 100 + 1, "S", 50, 1000).ok)
        solo.submit(self.KEXT + i1 * 100 + 2, "B", 30, 999, mog.SimOrderType.day_limit, 10)
        solo.drain()
        solo.advance_time(500)
        solo.apply_external("S", 1000, 20)
        self.assertEqual(o.sim(i1).trace_digest(), solo.trace_digest())
        self.assertEqual(o.sim(i1).book().qty_at("S", 1000), 30)

    def test_broadcast_equals_routed_in_index_order(self):
        a, c = mog.Orchestrator(), mog.Orchestrator()
        for _ in range(3):
            a.add_instrument(**self.config_kwargs())
            c.add_instrument(**self.config_kwargs())
        for i in range(3):
            self.drive(a, i)
            self.drive(c, i)
        a.drain_all()
        a.advance_all(250)
        a.drain_all()
        for i in range(3):
            c.drain(i)
            c.advance_time(i, 250)
            c.drain(i)
        self.assertEqual(a.global_digest(), c.global_digest())

    def test_independent_ref_spaces_and_names(self):
        o = mog.Orchestrator()
        i0 = o.add_instrument(name="A", **self.config_kwargs())
        i1 = o.add_instrument(name="B", **self.config_kwargs())
        same = self.KEXT + 7
        o.submit(i0, same, "B", 10, 1000, mog.SimOrderType.day_limit, 1)
        o.submit(i1, same, "B", 11, 1001, mog.SimOrderType.day_limit, 1)
        o.drain_all()
        self.assertEqual(o.sim(i0).book().remaining_of(same), 10)
        self.assertEqual(o.sim(i1).book().remaining_of(same), 11)
        self.assertEqual(o.name(i0), "A")
        self.assertEqual(o.count(), 2)


class FeeParity(unittest.TestCase):
    KEXT = 1 << 62

    def test_taker_charge_on_aggressive_leg(self):
        # Mirrors tests/fees_e2e.cpp: market buy 20 @1000 with taker 25bps.
        sim = mog.ExecutionSimulator(
            arena_capacity=256,
            lo_tick=0,
            hi_tick=4000,
            page_pool=8,
            event_capacity=1024,
            taker_fee_bps=25,
        )
        self.assertTrue(sim.seed_external(1, "S", 50, 1000).ok)
        sim.submit(self.KEXT + 1, "B", 20, 1000, mog.SimOrderType.market, 10)
        sim.drain()
        d = sim.decisions()[0]
        self.assertEqual(d.filled_qty, 20)
        self.assertEqual(d.filled_notional, 20_000)
        self.assertEqual(d.fee, 50)  # 20000 * 25 / 10000, exact

    def test_maker_rebate_truncates_toward_zero(self):
        # Resting bid filled by external flow; rebate = trunc(9980*2/10000)=1.
        sim = mog.ExecutionSimulator(
            arena_capacity=256,
            lo_tick=0,
            hi_tick=4000,
            page_pool=8,
            event_capacity=1024,
            maker_fee_bps=-2,
        )
        self.assertTrue(sim.seed_external(1, "B", 50, 998).ok)
        sim.submit(self.KEXT + 1, "B", 20, 998, mog.SimOrderType.day_limit, 100)
        sim.drain()
        sim.apply_external("B", 998, 60)
        r = sim.reports()[0]
        self.assertEqual(r.qty, 10)
        self.assertEqual(r.fee, -1)

    def test_zero_rates_leave_digest_unchanged(self):
        def run(**kw):
            sim = mog.ExecutionSimulator(
                arena_capacity=256, lo_tick=0, hi_tick=4000, page_pool=8, event_capacity=1024, **kw
            )
            self.assertTrue(sim.seed_external(1, "S", 40, 1002).ok)
            sim.submit(self.KEXT + 7, "B", 55, 1002, mog.SimOrderType.day_limit, 5)
            sim.drain()
            sim.apply_external("B", 1002, 45)
            return sim

        plain = run()
        zeroed = run(maker_fee_bps=0, taker_fee_bps=0)
        charged = run(taker_fee_bps=30, maker_fee_bps=-3)
        # Fees never enter the trace digest; only the fee fields move.
        self.assertEqual(plain.trace_digest(), zeroed.trace_digest())
        self.assertEqual(plain.trace_digest(), charged.trace_digest())
        self.assertEqual(plain.decisions()[0].fee, 0)
        self.assertGreater(charged.decisions()[0].fee, 0)


class PhaseOneAnalytics(unittest.TestCase):
    def test_run_trades_and_arrow(self):
        path = REPO / "data" / "sample.itch"
        if not path.exists():
            self.skipTest("repo checkout required (no data/sample.itch)")
        raw = path.read_bytes()
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
        sch_cap, arr_cap = summary.to_arrow()
        self.assertIsNotNone(sch_cap)
        self.assertIsNotNone(arr_cap)

    def test_simrun_and_tearsheet(self):
        script = """kind,ts_ns,side,price_ticks,qty,ref
ext_add,100,B,1998,400,11
ext_add,100,S,2002,380,12
strat_limit,150,B,1997,50,7001
trade,250,S,1998,120,9101
"""
        sim_summary = mog.run_simrun_script(
            script, arena_capacity=1 << 16, lo_tick=0, hi_tick=10_000
        )
        self.assertEqual(sim_summary.script_rows, 4)

        log_csv = """type,ts_ns,ref,side,price_ticks,qty,fee_cash,mid_ticks,ledger
P,100,1,B,2000,100,0,2000,M
F,150,7001,B,1997,50,-2,2000,M
P,250,2,S,1998,120,0,1998,M
"""
        tearsheet = mog.compute_tearsheet_from_csv(log_csv)
        self.assertEqual(tearsheet.fills, 1)
        self.assertEqual(tearsheet.prints, 2)
        self.assertEqual(tearsheet.buy_qty, 50)

    def test_strategy_runner(self):
        class StatStrategy(mog.Strategy):
            def __init__(self):
                super().__init__()
                self.book_count = 0
                self.fill_count = 0

            def on_order_book_update(self, update):
                self.book_count += 1

            def on_order_fill(self, fill):
                self.fill_count += 1

        strat = StatStrategy()
        runner = mog.StrategyRunner(strategy=strat, lo_tick=0, hi_tick=10000)
        self.assertTrue(runner.seed_external(1, "B", 100, 2000))
        self.assertTrue(runner.seed_external(2, "S", 100, 2010))
        runner.advance_time(100)
        self.assertGreater(strat.book_count, 0)
        runner.submit(7001, "B", 20, 2000, mog.SimOrderType.day_limit)
        runner.advance_time(100)
        runner.apply_external("B", 2000, 110)
        runner.advance_time(100)
        self.assertEqual(strat.fill_count, 1)


class PhaseTwoInstitutionalFidelity(unittest.TestCase):
    def test_almgren_chriss_dynamics(self):
        cfg = mog.ImpactConfig()
        self.assertGreater(cfg.gamma, 0.0)
        self.assertGreater(cfg.eta, 0.0)

        perm = mog.compute_permanent_impact(25_000.0, cfg)
        temp = mog.compute_temporary_impact(5_000.0, 60.0, cfg)
        self.assertGreater(perm, 0.0)
        self.assertGreater(temp, 0.0)

        traj = mog.compute_optimal_trajectory(100_000.0, 3600.0, 10, cfg)
        self.assertEqual(len(traj), 11)
        self.assertEqual(traj[0], 100_000.0)
        cost = mog.compute_expected_shortfall(traj, 360.0, cfg)
        self.assertGreater(cost, 0.0)

    def test_ouch_protocol_framing(self):
        self.assertEqual(mog.ouch_inbound_frame_length("O"), 49)
        self.assertEqual(mog.ouch_inbound_frame_length("U"), 47)
        self.assertEqual(mog.ouch_inbound_frame_length("X"), 19)
        self.assertEqual(mog.ouch_outbound_frame_length("A"), 66)
        self.assertEqual(mog.ouch_outbound_frame_length("E"), 40)
        self.assertEqual(mog.ouch_outbound_frame_length("C"), 28)

    def test_orchestrator_portfolio_extensions(self):
        orch = mog.Orchestrator()
        idx = orch.add_instrument(arena_capacity=1 << 16, lo_tick=0, hi_tick=10000, name="SPY")
        orch.seed_external(idx, 1, "B", 100, 5000)
        orch.seed_external(idx, 2, "S", 100, 5010)

        self.assertEqual(orch.best_bid(idx), 5000)
        self.assertEqual(orch.best_ask(idx), 5010)
        self.assertEqual(orch.mid_price(idx), 5005.0)

        orch.run_until_all(500)
        self.assertEqual(orch.total_fill_count(), 0)
        self.assertEqual(orch.total_filled_notional(), 0)


class PhaseThreeFastDeterminism(unittest.TestCase):
    def test_fast_digest_parity(self):
        d1 = mog.FastDigest128()
        d2 = mog.FastDigest128()
        d1.update(b"FAST_HASH_PAYLOAD")
        d2.update(b"FAST_HASH_PAYLOAD")
        self.assertEqual(d1.finish_hex(), d2.finish_hex())

        d_diff = mog.FastDigest128()
        d_diff.update(b"FAST_HASH_PAYLOAD_DIFFERENT")
        self.assertNotEqual(d1.finish_hex(), d_diff.finish_hex())

    def test_determinism_digest_modes(self):
        g = mog.DeterminismDigest(mog.DigestMode.golden)
        f = mog.DeterminismDigest(mog.DigestMode.fast)
        self.assertEqual(g.mode(), mog.DigestMode.golden)
        self.assertEqual(f.mode(), mog.DigestMode.fast)

        g.update(b"PAYLOAD")
        f.update(b"PAYLOAD")
        self.assertEqual(len(g.finish_sha256_hex()), 64)
        self.assertEqual(len(f.finish_fast128_hex()), 32)


if __name__ == "__main__":
    unittest.main()
