// M8 time-travel debugger e2e: record/replay determinism, seek equivalence,
// counterfactual forks, deterministic report export, and the milestone exit
// criterion - an adversarial fill scenario debugged end-to-end using only
// the shipped TimeTravelSession API.
#include <mog/TimeTravel.hpp>

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace {

using mog::ExecutionSimulator;
using mog::IcebergSpec;
using mog::kNoDivergence;
using mog::ok;
using mog::OrderId;
using mog::Price;
using mog::Qty;
using mog::Side;
using mog::SimConfig;
using mog::SimDecision;
using mog::SimInbound;
using mog::SimOpKind;
using mog::StpMode;
using mog::TimeTravelSession;

constexpr std::uint64_t kExt = std::uint64_t{1} << 62;

int failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
            ++failures;                                                                            \
        }                                                                                          \
    } while (false)

SimConfig base_config() noexcept {
    SimConfig cfg{};
    cfg.book.arena_capacity = 256;
    cfg.book.ladder.lo_tick = 0;
    cfg.book.ladder.hi_tick = 4000; // exclusive
    cfg.book.ladder.page_pool = 8;
    cfg.event_capacity = 1024;
    return cfg;
}

// The adversarial scenario, built entirely through recorded ops. Strategy
// buy 900 rests near the touch; external flow eats into it; a same-account
// aggressive sell then arrives and must die on cancel-newest STP against
// the resting victim. Iceberg replenishment keeps the ask alive so the
// sweep has somewhere to go. Every quantity below is hand-computable.
// External liquidity lives BELOW external_ref_limit; strategy refs above.
constexpr std::uint64_t kIceberg = 1;
constexpr std::uint64_t kExtAsk = 2;
constexpr std::uint64_t kRest = kExt + 10;
constexpr std::uint64_t kAttack = kExt + 11;

// Ops 6..8 of the adversarial scenario: time advance, the same-account
// aggressive sell submission, and its drain.
void finish_with_attack(TimeTravelSession& s) {
    s.advance_time(1000);
    const SimInbound attack{OrderId{kAttack}, Side::sell, Qty{150}, Price{998},
                            mog::SimOrderType::ioc};
    static_cast<void>(s.submit(attack, 500));
    s.drain();
}

void build_adversarial(TimeTravelSession& s) {
    static_cast<void>(
        s.seed_iceberg(IcebergSpec{OrderId{kIceberg}, Side::sell, Price{1002}, 30, 90}));
    static_cast<void>(s.seed_external(OrderId{kExtAsk}, Side::sell, Qty{40}, Price{1003}));
    s.set_stp_mode(StpMode::cancel_newest);

    // Resting tracked victim: 900 @ 998 (best bid once it lands).
    const SimInbound rest{OrderId{kRest}, Side::buy, Qty{900}, Price{998}};
    static_cast<void>(s.submit(rest, 100));
    s.drain();

    // External flow depletes the front of our queue: 200 units at the bid.
    s.apply_external(Side::buy, Price{998}, 200);
    finish_with_attack(s);
}

// Exit criterion demo: using ONLY the public session API, locate the step
// that produced the surprising rejected_stp decision, rewind to just before
// it, inspect the cause (a live tracked order on the opposite side under
// cancel-newest), and confirm via a counterfactual fork that removing the
// resting victim eliminates the rejection.
bool debug_adversarial_fill() {
    TimeTravelSession base(base_config());
    build_adversarial(base);

    // 1. Find the surprising decision in the shipped telemetry.
    const std::vector<SimDecision>& decisions = base.sim().decisions();
    std::size_t stp_idx = SIZE_MAX;
    for (std::size_t i = 0; i < decisions.size(); ++i)
        if (decisions[i].kind == SimDecision::Kind::rejected_stp)
            stp_idx = i;
    CHECK(stp_idx != SIZE_MAX);
    if (stp_idx == SIZE_MAX)
        return false;

    // 2. Rewind to the last op whose cursor precedes the offending drain.
    //    Ops alternate seed/stp/submit/drain/apply/time/submit/drain; the
    //    attack decision happens inside the final drain, op index
    //    log().size()-1. Seek there and inspect.
    const std::size_t before_attack_drain = base.op_count() - 1;
    base.seek(before_attack_drain);
    CHECK(base.cursor() == before_attack_drain);

    // The resting victim is live at this point with exact queue state.
    const std::int64_t ahead = base.queue_ahead_of(OrderId{kRest});
    CHECK(ahead == 0); // 200 external units consumed the entire external prefix
    CHECK(base.sim().book().remaining_of(OrderId{kRest}) == 700);

    // 3. Counterfactual: replay identical history but without the resting
    //    order; the attack must no longer be STP-rejected.
    TimeTravelSession cf(base_config());
    base.fork_at(before_attack_drain, cf);
    // Diverge: drop the resting order, then run the attack exactly as before.
    CHECK(cf.cancel_strategy(OrderId{kRest}));
    finish_with_attack(cf);

    bool saw_stp_reject = false;
    for (const SimDecision& d : cf.sim().decisions())
        if (d.kind == SimDecision::Kind::rejected_stp)
            saw_stp_reject = true;
    CHECK(!saw_stp_reject);

    // The branches genuinely diverged, and the divergence locator agrees.
    const auto dig_base = base.sim().trace_digest();
    const auto dig_cf = cf.sim().trace_digest();
    CHECK(dig_base != dig_cf);
    CHECK(TimeTravelSession::first_decision_divergence(base, cf) != kNoDivergence);
    CHECK(base.audit());
    CHECK(cf.audit());
    const int before = failures;
    return failures == before;
}

} // namespace

int main() {
    int local = 0;

    // --- replay determinism -------------------------------------------------
    {
        TimeTravelSession a(base_config());
        build_adversarial(a);
        TimeTravelSession b(base_config());
        build_adversarial(b);
        CHECK(a.sim().trace_digest() == b.sim().trace_digest());
        {
            char ha[64], hb[64];
            const auto va = mog::Sha256::hex(a.sim().trace_digest(), ha);
            const auto vb = mog::Sha256::hex(b.sim().trace_digest(), hb);
            CHECK(va == vb); // 64-char views; no NUL terminator is written
        }
        CHECK(TimeTravelSession::first_decision_divergence(a, b) == kNoDivergence);
    }

    // --- seek equivalence at every checkpoint --------------------------------
    {
        TimeTravelSession seq(base_config());
        build_adversarial(seq);
        const auto final_digest = seq.sim().trace_digest();

        for (std::size_t k = 0; k <= seq.op_count(); ++k) {
            TimeTravelSession s(base_config());
            build_adversarial(s); // run to the end first...
            s.seek(k);            // ...then rewind to k
            CHECK(s.cursor() == k);
            // Fast-forward back to the end; state must be indistinguishable.
            s.seek(seq.op_count());
            CHECK(s.sim().trace_digest() == final_digest);
            CHECK(s.audit());
        }
    }

    // --- rewind to zero, then full forward replay ------------------------------
    {
        TimeTravelSession s(base_config());
        build_adversarial(s);
        const auto d0 = s.sim().trace_digest();
        s.seek(0);
        CHECK(s.cursor() == 0);
        CHECK(s.sim().book().live_orders() == 0);
        CHECK(s.sim().decisions().empty());
        s.seek(s.op_count());
        CHECK(s.sim().trace_digest() == d0);
    }

    // --- counterfactual fork controls ------------------------------------------
    {
        TimeTravelSession base(base_config());
        build_adversarial(base);

        // Control: fork after the first drain, replay the identical tail;
        // the branch must reproduce the base trace bit-for-bit. This proves
        // rebuild-from-prefix + identical suffix == original run.
        TimeTravelSession control(base_config());
        base.fork_at(5, control);
        control.apply_external(Side::buy, Price{998}, 200);
        finish_with_attack(control);
        CHECK(control.sim().trace_digest() == base.sim().trace_digest());
        CHECK(TimeTravelSession::first_decision_divergence(control, base) == kNoDivergence);
        CHECK(control.log().size() == base.log().size());

        // Real divergence: one different apply_external quantity changes the
        // trace even though the same attack follows.
        TimeTravelSession divergent(base_config());
        base.fork_at(5, divergent);
        // Real divergence: deplete 950 instead of 200 - that exceeds the
        // victim's 900 resting units, so it is consumed outright and the
        // attack no longer dies on STP: its decision flips from
        // rejected_stp to partial_then_cancelled.
        divergent.apply_external(Side::buy, Price{998}, 950); // base used 200
        finish_with_attack(divergent);
        CHECK(divergent.sim().trace_digest() != base.sim().trace_digest());
        CHECK(TimeTravelSession::first_decision_divergence(divergent, base) != kNoDivergence);
        // The base branch is untouched by the fork's history.
        CHECK(base.log().size() == 9);
    }

    // --- deterministic export ----------------------------------------------------
    {
        TimeTravelSession a(base_config());
        build_adversarial(a);
        TimeTravelSession b(base_config());
        build_adversarial(b);
        const std::string ra = a.export_report();
        const std::string rb = b.export_report();
        CHECK(ra == rb);
        CHECK(ra.find("# mog time-travel report") == 0);
        CHECK(ra.find("## timeline") != std::string::npos);
        CHECK(ra.find("## decisions") != std::string::npos);
        CHECK(ra.find("rejected_stp") != std::string::npos);
        CHECK(ra.find("## book L2") != std::string::npos);
    }

    // --- exit criterion: adversarial scenario debugged with shipped tooling ----
    {
        const bool okd = debug_adversarial_fill();
        CHECK(okd);
    }

    local = failures;
    if (failures == 0)
        std::printf("timetravel_e2e: all checks passed\n");
    else
        std::printf("timetravel_e2e: %d failure(s)\n", failures);
    return local == 0 ? 0 : 1;
}
