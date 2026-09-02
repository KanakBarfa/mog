// Almgren-Chriss market impact E2E integration test: verifies power-law
// permanent and temporary price impact, exponential resilience decay, and
// optimal execution trajectories.
#include <mog/Contracts.hpp>
#include <mog/Impact.hpp>

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expr);        \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)

void test_permanent_and_temporary_impact() {
    mog::impact::ImpactConfig cfg{};
    cfg.gamma = 0.10;
    cfg.eta = 0.15;
    cfg.daily_volatility = 0.02;
    cfg.daily_volume = 1'000'000.0;
    cfg.alpha = 0.50;
    cfg.beta = 0.50;

    const double perm = mog::impact::compute_permanent_impact(10'000.0, cfg);
    CHECK(perm > 0.0);

    const double perm_neg = mog::impact::compute_permanent_impact(-10'000.0, cfg);
    CHECK(perm_neg == -perm);

    const double temp = mog::impact::compute_temporary_impact(1'000.0, 60.0, cfg);
    CHECK(temp > 0.0);
}

void test_impact_state_and_resilience_decay() {
    mog::impact::ImpactConfig cfg{};
    cfg.decay_half_life_ns = 1'000'000'000ULL; // 1 second half-life

    mog::impact::ImpactState state{};
    mog::impact::apply_execution(state, 5'000.0, 10.0, 100, cfg);

    const double initial_temp = state.temporary_shift_ticks;
    CHECK(initial_temp > 0.0);
    const double initial_perm = state.permanent_shift_ticks;
    CHECK(initial_perm > 0.0);

    // Advance by 1 half-life (1 second = 1e9 ns)
    mog::impact::decay_temporary_impact(state, 100 + 1'000'000'000ULL, cfg);

    CHECK(std::abs(state.temporary_shift_ticks - (initial_temp * 0.5)) < 1e-3);
    // Permanent impact should remain unchanged across time
    CHECK(state.permanent_shift_ticks == initial_perm);
}

void test_optimal_trajectory_and_cost() {
    mog::impact::ImpactConfig cfg{};
    cfg.risk_aversion = 1e-5;

    const auto trajectory = mog::impact::compute_optimal_trajectory(100'000.0, 3600.0, 10, cfg);
    CHECK(trajectory.size() == 11);
    CHECK(trajectory[0] == 100'000.0);
    CHECK(trajectory[10] < 1e-3);

    // Monotonically decreasing remaining shares
    for (std::size_t i = 1; i < trajectory.size(); ++i) {
        CHECK(trajectory[i] <= trajectory[i - 1]);
    }

    const double cost = mog::impact::compute_expected_shortfall(trajectory, 360.0, cfg);
    CHECK(cost > 0.0);
}

} // namespace

int main() {
    test_permanent_and_temporary_impact();
    test_impact_state_and_resilience_decay();
    test_optimal_trajectory_and_cost();
    std::printf("impact_e2e passed\n");
    return 0;
}
