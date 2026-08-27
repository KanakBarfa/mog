// M5 exit criterion: a sample market-maker runs end-to-end through the CRTP
// hooks, every emitted event lands in a columnar log, and the numbers the
// telemetry layer produces are recomputed independently from the log file
// by separate arithmetic. A background-thread logger must produce a
// logically identical log to the synchronous one.
#include <mog/ColumnLog.hpp>
#include <mog/Metrics.hpp>
#include <mog/Reflect.hpp>
#include <mog/Simulate.hpp>
#include <mog/SpscRing.hpp>
#include <mog/Strategy.hpp>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <unistd.h>

namespace {

int failures = 0;

// CI runners do not have the developer's scratch dirs; every artifact goes
// to the platform temp directory (TMPDIR-aware), isolated by PID.
std::string tmp_path(const char* name) {
    namespace fs = std::filesystem;
    const std::string unique_name = "mog_" + std::to_string(::getpid()) + "_" + name;
    return (fs::temp_directory_path() / unique_name).string();
}

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);                             \
            std::fflush(stdout);                                                                   \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

struct EquitySample {
    std::uint64_t ts = 0;
    double equity = 0;
    static constexpr auto mog_fields = std::tuple{mog::field("ts", &EquitySample::ts),
                                                  mog::field("equity", &EquitySample::equity)};
};

struct MidSample {
    std::uint64_t ts = 0;
    std::int64_t mid2 = 0;
    static constexpr auto mog_fields =
        std::tuple{mog::field("ts", &MidSample::ts), mog::field("mid2", &MidSample::mid2)};
};

enum class TestStatus : std::uint8_t { pending = 0, active = 1, filled = 2, cancelled = 3 };

struct EnumSample {
    std::uint64_t ts = 0;
    TestStatus status = TestStatus::pending;
    static constexpr auto mog_fields =
        std::tuple{mog::field("ts", &EnumSample::ts), mog::field("status", &EnumSample::status)};
};

// ---------------------------------------------------------------------------
// Reflect unit checks

void reflect_tests() {
    using namespace mog;
    CHECK(field_count<BookUpdate>() == 5);
    CHECK(field_count<MidSample>() == 2);
    CHECK(field_count<EnumSample>() == 2);
    std::string_view names[5];
    std::size_t i = 0;
    for_each_field<BookUpdate>([&](const auto& f) { names[i++] = f.name; });
    CHECK(names[0] == "ts" && names[1] == "bid_px" && names[4] == "ask_qty");
    CHECK(column_type_of<double>() == ColumnType::f64);
    CHECK(column_type_of<std::int64_t>() == ColumnType::i64);
    CHECK(column_type_of<std::uint8_t>() == ColumnType::u8);
    CHECK(column_type_of<TestStatus>() == ColumnType::u8);

    MidSample m{7, -3};
    bool saw_i64 = false;
    for_each_field<MidSample>([&](const auto& f) {
        using T = std::remove_reference_t<decltype(field_of(m, f))>;
        if constexpr (std::is_same_v<T, std::int64_t>) {
            CHECK(field_of(m, f) == -3);
            saw_i64 = true;
        }
    });
    CHECK(saw_i64);
}

// ---------------------------------------------------------------------------
// Columnar log round-trip with interleaved row groups

void column_log_tests(const char* path) {
    using namespace mog;
    if (std::FILE* f = std::fopen(path, "wb")) {
        ColumnLogWriter w(f);
        const auto s_mid = w.add_stream<MidSample>("mids");
        const auto s_trd = w.add_stream<SimTrade>("trades");
        const auto s_enm = w.add_stream<EnumSample>("enums");
        w.append(s_mid, MidSample{1, 2000});
        w.append(s_trd, SimTrade{5, 1, 1010, 30, 1});
        w.append(s_enm, EnumSample{7, TestStatus::active});
        w.flush();
        w.append(s_mid, MidSample{2, 2005});
        w.append(s_mid, MidSample{3, 1992});
        w.append(s_trd, SimTrade{9, 2, 998, 10, 0});
        w.append(s_enm, EnumSample{12, TestStatus::filled});
        w.flush();
        CHECK(w.rows_written(s_mid) == 3);
        CHECK(w.rows_written(s_trd) == 2);
        CHECK(w.rows_written(s_enm) == 2);
        w.close();
        std::fclose(f);
    }

    std::FILE* f = std::fopen(path, "rb");
    CHECK(f != nullptr);
    ColumnLogReader r(f);
    CHECK(r.error().empty());
    CHECK(r.stream_count() == 3);
    CHECK(r.schema_matches<MidSample>(0));
    CHECK(r.schema_matches<SimTrade>(1));
    CHECK(r.schema_matches<EnumSample>(2));
    CHECK(!r.schema_matches<EquitySample>(0));

    std::vector<MidSample> mids;
    CHECK(r.read_stream<MidSample>(0, [&](const MidSample& m) { mids.push_back(m); }));
    CHECK(r.error().empty());
    CHECK(mids.size() == 3);
    CHECK(mids[0].ts == 1 && mids[0].mid2 == 2000);
    CHECK(mids[2].mid2 == 1992);

    std::vector<SimTrade> trs;
    r.seek_to_data();
    CHECK(r.read_stream<SimTrade>(1, [&](const SimTrade& t) { trs.push_back(t); }));
    CHECK(trs.size() == 2);
    CHECK(trs[1].px_ticks == 998 && trs[1].qty == 10 && trs[1].aggressor_buy == 0);

    std::vector<EnumSample> enms;
    r.seek_to_data();
    CHECK(r.read_stream<EnumSample>(2, [&](const EnumSample& e) { enms.push_back(e); }));
    CHECK(enms.size() == 2);
    CHECK(enms[0].ts == 7 && enms[0].status == TestStatus::active);
    CHECK(enms[1].ts == 12 && enms[1].status == TestStatus::filled);
    std::fclose(f);
}

// ---------------------------------------------------------------------------
// SPSC ring basics; cross-thread behavior exercised by the E2E run

void spsc_tests() {
    using namespace mog;
    SpscRing<std::uint64_t, 256> ring;
    for (std::uint64_t i = 0; i < 256; ++i)
        CHECK(ring.try_push(i));
    CHECK(!ring.try_push(999));
    for (std::uint64_t i = 0; i < 128; ++i) {
        std::uint64_t v = 0;
        CHECK(ring.try_pop(v));
        CHECK(v == i);
    }
    for (std::uint64_t i = 256; i < 384; ++i)
        ring.push_spin(i);
    for (std::uint64_t i = 128; i < 384; ++i) {
        std::uint64_t v = 0;
        CHECK(ring.try_pop(v));
        CHECK(v == i);
    }
    std::uint64_t v = 0;
    CHECK(!ring.try_pop(v));
}

// ---------------------------------------------------------------------------
// Tick histogram bucket bounds and fetch_max watermarks

void histogram_tests() {
    using namespace mog;
    TickHistogram<16> h;
    h.record(1000); // [512,1024) b5
    h.record(3000); // [2048,4096) b7
    h.record(900);  // b5
    h.record(15);   // b0 [16,32) floor
    CHECK(h.bucket_count(5) == 2);
    CHECK(h.bucket_max(5) == 1000);
    CHECK(h.bucket_count(7) == 1);
    CHECK(h.bucket_max(7) == 3000);
    CHECK(h.bucket_count(0) == 1);
    // Watermark semantics: maxima never regress below observed values.
    CHECK(h.bucket_max(7) != 0);
}

} // namespace

// ---------------------------------------------------------------------------
// Sample market-maker over scripted external flow

namespace {

using namespace mog;

// Uniform telemetry envelope so one ring type carries every event kind.
struct TelemetryRec {
    std::uint8_t kind = 0; // 0 book, 1 trade, 2 ack, 3 fill, 4 mid, 5 equity
    BookUpdate b{};
    SimTrade t{};
    OrderAck a{};
    OrderFill f{};
    MidSample m{};
    EquitySample e{};
};
static_assert(std::is_trivially_copyable_v<TelemetryRec>);

class MarketMaker final : public Strategy<MarketMaker> {
public:
    static constexpr std::int64_t kHalfSpread = 2;
    static constexpr std::int64_t kSize = 10;

    void bind(StrategyRunner<MarketMaker>& r) {
        runner_ = &r;
        next_ref_ = r.sim().config().external_ref_limit + 1000;
    }
    void attach_ring(SpscRing<TelemetryRec, 4096>* ring) { ring_ = ring; }

    void on_order_book_update(const BookUpdate& u) {
        emit(TelemetryRec{.kind = 0, .b = u});
        ++book_updates_;
        first_bid_ = first_bid_ == 0 ? u.bid_px : first_bid_;
        first_ask_ = first_ask_ == 0 ? u.ask_px : first_ask_;
        if (u.bid_px == kNoTick || u.ask_px == kNoTick)
            return;
        const std::int64_t mid = (u.bid_px + u.ask_px) / 2;
        rebid(mid - kHalfSpread, mid + kHalfSpread);
    }
    void on_trade(const SimTrade& t) {
        emit(TelemetryRec{.kind = 1, .t = t});
        ++trades_seen_;
    }
    void on_order_ack(const OrderAck& a) {
        emit(TelemetryRec{.kind = 2, .a = a});
        ++acks_;
        // Aggressive executions report through acks with exact notional;
        // passive fills arrive separately as fills (no double counting).
        if (a.filled > 0 && (a.kind == 1 || a.kind == 2))
            acct_.on_aggressive_fill(a.notional, a.buy ? static_cast<std::int64_t>(a.filled)
                                                       : -static_cast<std::int64_t>(a.filled));
    }
    void on_order_fill(const OrderFill& f) {
        emit(TelemetryRec{.kind = 3, .f = f});
        ++fills_;
        const std::int64_t signed_qty =
            f.buy ? static_cast<std::int64_t>(f.qty) : -static_cast<std::int64_t>(f.qty);
        acct_.on_fill(f.px, signed_qty);
    }

    [[nodiscard]] const Account& account() const noexcept { return acct_; }
    void note_mark(double mark) { acct_.observe_equity(acct_.equity(mark)); }
    // Where our quotes currently sit (the scenario sweeps these exact levels).
    [[nodiscard]] std::int64_t last_bid_px() const noexcept { return last_bid_px_; }
    [[nodiscard]] std::int64_t last_ask_px() const noexcept { return last_ask_px_; }
    [[nodiscard]] std::uint64_t book_updates() const noexcept { return book_updates_; }
    [[nodiscard]] std::uint64_t fills() const noexcept { return fills_; }
    [[nodiscard]] std::uint64_t acks() const noexcept { return acks_; }
    [[nodiscard]] std::uint64_t trades_seen() const noexcept { return trades_seen_; }
    [[nodiscard]] std::int64_t first_bid() const noexcept { return first_bid_; }
    [[nodiscard]] std::int64_t first_ask() const noexcept { return first_ask_; }

private:
    void emit(const TelemetryRec& rec) {
        if (ring_ != nullptr)
            ring_->push_spin(rec);
    }

    void rebid(std::int64_t bid_px, std::int64_t ask_px) {
        cancel_outstanding();
        SimInbound b{};
        b.ref = OrderId{next_ref_++};
        b.side = Side::buy;
        b.qty = Qty{kSize};
        b.price = Price{bid_px};
        bid_ref_ = b.ref.value;
        last_bid_px_ = bid_px;
        static_cast<void>(runner_->submit(b));

        SimInbound a{};
        a.ref = OrderId{next_ref_++};
        a.side = Side::sell;
        a.qty = Qty{kSize};
        a.price = Price{ask_px};
        ask_ref_ = a.ref.value;
        last_ask_px_ = ask_px;
        static_cast<void>(runner_->submit(a));
    }

    void cancel_outstanding() {
        for (const std::uint64_t ref : {bid_ref_, ask_ref_})
            if (ref != 0)
                static_cast<void>(runner_->sim().cancel_strategy(OrderId{ref}));
    }

    StrategyRunner<MarketMaker>* runner_ = nullptr;
    SpscRing<TelemetryRec, 4096>* ring_ = nullptr;
    Account acct_{};
    std::uint64_t next_ref_ = 0;
    std::uint64_t bid_ref_ = 0;
    std::uint64_t ask_ref_ = 0;
    std::uint64_t book_updates_ = 0;
    std::uint64_t trades_seen_ = 0;
    std::uint64_t acks_ = 0;
    std::uint64_t fills_ = 0;
    std::int64_t first_bid_ = 0;
    std::int64_t first_ask_ = 0;
    std::int64_t last_bid_px_ = 0;
    std::int64_t last_ask_px_ = 0;
};

struct ScenarioResult {
    std::int64_t inventory = 0;
    std::int64_t cash = 0;
    double drawdown = 0;
    std::uint64_t fills = 0;
    std::uint64_t trades_seen = 0;
    std::uint64_t book_updates = 0;
    std::int64_t first_bid = 0;
    std::int64_t first_ask = 0;
};

// Runs the scripted scenario, streaming all telemetry through the ring to a
// consumer (background thread or inline). Writes the five-stream log.
ScenarioResult drive(const char* path, bool background_consumer) {
    SimConfig cfg{};
    cfg.book.arena_capacity = 4096;
    cfg.book.ladder = {0, 4000, 8};
    cfg.event_capacity = 4096;
    cfg.parse_latency_ns = 200;
    cfg.decision_latency_ns = 300;
    cfg.wire_latency_ns = 150;
    cfg.jitter_kind = JitterKind::none;

    std::FILE* file = std::fopen(path, "wb");
    CHECK(file != nullptr);
    ColumnLogWriter log(file);
    const auto s_fills = log.add_stream<OrderFill>("fills");
    const auto s_acks = log.add_stream<OrderAck>("acks");
    const auto s_trades = log.add_stream<SimTrade>("trades");
    const auto s_mids = log.add_stream<MidSample>("mids");
    const auto s_equity = log.add_stream<EquitySample>("equity");
    const auto s_book = log.add_stream<BookUpdate>("book");

    auto ring = std::make_unique<SpscRing<TelemetryRec, 4096>>();

    StrategyRunner<MarketMaker> runner(cfg, MarketMaker{});
    MarketMaker& mm = runner.strategy();
    mm.bind(runner);
    mm.attach_ring(ring.get());

    ExecutionSimulator& sim = runner.sim();

    // Seed: bids 990x50, 985x50; asks 1010x50, 1016x50.
    CHECK(ok(sim.seed_external(OrderId{100}, Side::buy, Qty{50}, Price{990})));
    CHECK(ok(sim.seed_external(OrderId{101}, Side::buy, Qty{50}, Price{985})));
    CHECK(ok(sim.seed_external(OrderId{200}, Side::sell, Qty{50}, Price{1010})));
    CHECK(ok(sim.seed_external(OrderId{201}, Side::sell, Qty{50}, Price{1016})));

    auto pump_ring_to_log = [&] {
        TelemetryRec rec{};
        while (ring->try_pop(rec)) {
            switch (rec.kind) {
            case 0:
                log.append(s_book, rec.b);
                break;
            case 1:
                log.append(s_trades, rec.t);
                break;
            case 2:
                log.append(s_acks, rec.a);
                break;
            case 3:
                log.append(s_fills, rec.f);
                break;
            case 4:
                log.append(s_mids, rec.m);
                break;
            default:
                log.append(s_equity, rec.e);
                break;
            }
        }
    };

    std::atomic<bool> producing{true};
    std::thread consumer;
    if (background_consumer) {
        consumer = std::thread([&] {
            while (producing.load(std::memory_order_acquire) || ring->size() > 0) {
                pump_ring_to_log();
                std::this_thread::yield();
            }
        });
    }

    auto step_to = [&](std::uint64_t t) {
        runner.advance_time(t - runner.now());
        const auto& bk = sim.book();
        const std::int64_t bb = bk.best_bid(), ba = bk.best_ask();
        const std::int64_t mid2 = (bb == kNoTick || ba == kNoTick) ? 0 : bb + ba;
        ring->push_spin(TelemetryRec{.kind = 4, .m = MidSample{t, mid2}});
        const double mark = static_cast<double>(mid2) / 2.0;
        ring->push_spin(TelemetryRec{.kind = 5, .e = EquitySample{t, mm.account().equity(mark)}});
        mm.note_mark(mark);
        if (!background_consumer)
            pump_ring_to_log();
    };

    // Phase A: aggressive market buy crosses the 1010 external offer.
    SimInbound mkt{};
    mkt.ref = OrderId{sim.config().external_ref_limit + 7};
    mkt.side = Side::buy;
    mkt.qty = Qty{30};
    mkt.type = SimOrderType::market;
    static_cast<void>(runner.submit(mkt));
    step_to(500);

    // Phase B: external sellers exhaust the remainder of the 1010 level.
    // Our resting ask (rested at t=950) becomes the best offer.
    sim.apply_external(Side::sell, Price{1010}, 20);
    step_to(1000);

    // The requote triggered by phase B's top-of-book change settles here,
    // re-pinning both sides around the unchanged 1000 mid.
    step_to(1500);

    // Phase C: external selling lifts our live bid wherever it rests
    // (passive fill stamped 1500 wire-side + 150 wire latency).
    sim.apply_external(Side::buy, Price{mm.last_bid_px()}, 10);
    step_to(2000);

    // Phase D: deeper bid erosion moves the mark against our inventory.
    sim.apply_external(Side::buy, Price{990}, 50);
    step_to(2500);

    // Settle: quotes rest, no further flow. The far boundaries give the
    // longer markout horizons mid samples to read against.
    step_to(3000);
    step_to(20000);
    step_to(200000);

    producing.store(false, std::memory_order_release);
    if (consumer.joinable())
        consumer.join();
    pump_ring_to_log(); // tail of the ring after the producer stopped

    log.flush();
    CHECK(log.rows_written(s_fills) == 1); // passive reports only (phase C)
    ScenarioResult res;
    res.inventory = mm.account().inventory();
    res.cash = mm.account().cash();
    res.drawdown = mm.account().drawdown();
    res.fills = mm.fills();
    res.trades_seen = mm.trades_seen();
    res.book_updates = mm.book_updates();
    res.first_bid = mm.first_bid();
    res.first_ask = mm.first_ask();
    log.close();
    std::fclose(file);
    return res;
}

template <Reflected S>
std::vector<S> read_all(const char* path, std::string_view name) {
    std::vector<S> out;
    if (std::FILE* f = std::fopen(path, "rb")) {
        ColumnLogReader r(f);
        for (std::uint32_t i = 0; i < r.stream_count(); ++i)
            if (r.stream_name(i) == name && r.schema_matches<S>(i))
                static_cast<void>(r.read_stream<S>(i, [&](const S& rec) { out.push_back(rec); }));
        std::fclose(f);
    }
    return out;
}

void verify_independent_recomputation(const char* path, const ScenarioResult& res) {
    // Replay executions through deliberately separate arithmetic, merging the
    // two disjoint sources of truth: aggressive legs live in acks (exact
    // integer notionals), passive fills in the fills stream.
    const auto fills = read_all<OrderFill>(path, "fills");
    CHECK(fills.size() == res.fills);
    const auto acks = read_all<OrderAck>(path, "acks");
    std::int64_t inv = 0, cash = 0;
    struct Exec {
        std::uint64_t ts;
        std::int64_t px;
        bool buy;
    };
    std::vector<Exec> execs;
    for (const auto& a : acks) {
        if (!(a.filled > 0 && (a.kind == 1 || a.kind == 2)))
            continue;
        const std::int64_t signed_qty =
            a.buy ? static_cast<std::int64_t>(a.filled) : -static_cast<std::int64_t>(a.filled);
        // Notional already embeds quantity; only its direction applies.
        const std::int64_t dir = signed_qty < 0 ? -1 : 1;
        cash -= dir * static_cast<std::int64_t>(a.notional);
        inv += signed_qty;
        execs.push_back(Exec{a.ts, static_cast<std::int64_t>(a.notional / a.filled), a.buy != 0});
    }
    for (const auto& f : fills) {
        const std::int64_t signed_qty =
            f.buy ? static_cast<std::int64_t>(f.qty) : -static_cast<std::int64_t>(f.qty);
        cash -= signed_qty * f.px;
        inv += signed_qty;
        execs.push_back(Exec{f.ts, f.px, f.buy != 0});
    }
    CHECK(inv == res.inventory);
    CHECK(cash == res.cash);
    CHECK(execs.size() == 2);

    // Drawdown recomputed from the equity curve by peak/trough scan.
    const auto curve = read_all<EquitySample>(path, "equity");
    double peak = 0, dd = 0;
    for (const auto& e : curve) {
        peak = peak > e.equity ? peak : e.equity;
        dd = dd > (peak - e.equity) ? dd : (peak - e.equity);
    }
    CHECK(dd > 0);
    CHECK(dd == res.drawdown);

    // Markouts from logged mids vs logged fills; the market fill's 1us
    // markout is hand-computable from the scripted moves.
    Markouts mk;
    for (const auto& m : read_all<MidSample>(path, "mids"))
        mk.add_mid(m.ts, m.mid2);
    for (const auto& e : execs)
        mk.add_fill(e.ts, e.px, e.buy ? 'B' : 'S');
    CHECK(mk.count(0) == 2);
    CHECK(mk.count(1) == 2);
    CHECK(mk.count(2) == 2);
    // Exec A: buy 30 @1010 at ts=650. Its T+1us sample lands mid-requote
    // (our ask cancelled, 1016 showing): mid 1003 -> -7. Exec B: buy 10 @998
    // at ts=1650 first reads the post-erode mid 996 -> -2. Longer horizons
    // see the settled mid 996 for both -> (-14 - 2)/2 = -8.
    CHECK(mk.mean_markout(0) == -4.5);
    CHECK(mk.mean_markout(1) == -8.0);
    CHECK(mk.mean_markout(2) == -8.0);
}

} // namespace

int main() {
    reflect_tests();
    column_log_tests(tmp_path("mog_collog_unit.bin").c_str());
    spsc_tests();
    histogram_tests();

    const ScenarioResult sync_res =
        drive(tmp_path("mog_e2e_sync.bin").c_str(), /*background_consumer=*/false);
    const ScenarioResult thr_res =
        drive(tmp_path("mog_e2e_thr.bin").c_str(), /*background_consumer=*/true);

    // Hand-computed expectations for the scripted phases. The aggressive
    // market buy accounts through its ack notional (30 * 1010, stamped 650);
    // the passive fill accounts through its report: our bid still rests at
    // 998 when phase C lifts it (the mid only moves to 996 after that), so
    // cash = -(30*1010 + 10*998) = -40280.
    CHECK(sync_res.inventory == 40);
    CHECK(sync_res.cash == -40280);
    CHECK(sync_res.fills == 1);
    CHECK(sync_res.trades_seen == 3);
    CHECK(sync_res.drawdown > 0);
    CHECK(sync_res.book_updates >= 4);
    CHECK(sync_res.first_bid == 990);
    CHECK(sync_res.first_ask == 1010);

    // Threaded logging must reproduce the synchronous log logically.
    CHECK(thr_res.inventory == sync_res.inventory);
    CHECK(thr_res.cash == sync_res.cash);
    CHECK(thr_res.drawdown == sync_res.drawdown);
    CHECK(thr_res.fills == sync_res.fills);
    CHECK(thr_res.trades_seen == sync_res.trades_seen);

    const std::string sync_log = tmp_path("mog_e2e_sync.bin");
    const std::string thr_log = tmp_path("mog_e2e_thr.bin");
    for (const char* path : {sync_log.c_str(), thr_log.c_str()})
        verify_independent_recomputation(path, sync_res);

    if (failures == 0)
        std::printf("strategy e2e OK\n");
    else
        std::printf("strategy e2e FAILURES=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
