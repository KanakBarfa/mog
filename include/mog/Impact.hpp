// Almgren-Chriss nonlinear market impact dynamics and optimal execution engine.
#pragma once

#include <mog/Contracts.hpp>
#include <mog/Types.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace mog::impact {

// Configuration parameters for Almgren-Chriss impact model.
struct ImpactConfig {
    double gamma = 0.10;               // Permanent impact scale factor
    double eta = 0.15;                 // Temporary impact scale factor
    double daily_volatility = 0.02;    // Daily asset volatility (sigma, e.g. 2%)
    double daily_volume = 1'000'000.0; // Baseline average daily volume
    double alpha = 0.50;               // Permanent impact power exponent (square-root law)
    double beta = 0.50;                // Temporary impact power exponent
    std::uint64_t decay_half_life_ns =
        60'000'000'000ULL;       // 60s half-life decay for temporary impact
    double risk_aversion = 1e-6; // Risk aversion parameter (lambda)
};

// Dynamic market impact state tracking instantaneous shifts over time.
struct ImpactState {
    double permanent_shift_ticks = 0.0;
    double temporary_shift_ticks = 0.0;
    std::uint64_t last_update_ts = 0;
};

// Computes permanent price impact shift in ticks according to power-law dynamics.
[[nodiscard]] inline double compute_permanent_impact(double signed_qty,
                                                     const ImpactConfig& cfg) noexcept {
    if (std::abs(signed_qty) <= 0.0 || cfg.daily_volume <= 0.0)
        return 0.0;
    const double sign = signed_qty > 0.0 ? 1.0 : -1.0;
    const double ratio = std::abs(signed_qty) / cfg.daily_volume;
    const double scaled_ratio = (cfg.alpha == 0.50) ? std::sqrt(ratio) : std::pow(ratio, cfg.alpha);
    return sign * cfg.gamma * cfg.daily_volatility * scaled_ratio * kPriceScale;
}

// Computes temporary price impact shift in ticks for instantaneous trade execution.
[[nodiscard]] inline double compute_temporary_impact(double signed_qty, double duration_sec,
                                                     const ImpactConfig& cfg) noexcept {
    if (std::abs(signed_qty) <= 0.0 || cfg.daily_volume <= 0.0 || duration_sec <= 0.0)
        return 0.0;
    const double sign = signed_qty > 0.0 ? 1.0 : -1.0;
    // Normalize volume rate to daily rate (assuming 6.5 trading hours = 23,400s)
    const double rate = (std::abs(signed_qty) / duration_sec) * 23400.0;
    const double ratio = rate / cfg.daily_volume;
    const double scaled_ratio = (cfg.beta == 0.50) ? std::sqrt(ratio) : std::pow(ratio, cfg.beta);
    return sign * cfg.eta * cfg.daily_volatility * scaled_ratio * kPriceScale;
}

// Decays temporary impact over elapsed time dt_ns according to half-life resilience.
inline void decay_temporary_impact(ImpactState& state, std::uint64_t current_ts,
                                   const ImpactConfig& cfg) noexcept {
    if (current_ts <= state.last_update_ts || cfg.decay_half_life_ns == 0) {
        state.last_update_ts = current_ts;
        return;
    }
    const double dt = static_cast<double>(current_ts - state.last_update_ts);
    const double lambda = std::log(2.0) / static_cast<double>(cfg.decay_half_life_ns);
    state.temporary_shift_ticks *= std::exp(-lambda * dt);
    state.last_update_ts = current_ts;
}

// Updates impact state upon execution of a trade.
inline void apply_execution(ImpactState& state, double signed_qty, double duration_sec,
                            std::uint64_t ts_ns, const ImpactConfig& cfg) noexcept {
    decay_temporary_impact(state, ts_ns, cfg);
    const double perm = compute_permanent_impact(signed_qty, cfg);
    const double temp = compute_temporary_impact(signed_qty, duration_sec, cfg);
    state.permanent_shift_ticks += perm;
    state.temporary_shift_ticks += temp;
}

// Returns total effective price adjustment in ticks.
[[nodiscard]] inline double total_impact_ticks(const ImpactState& state) noexcept {
    return state.permanent_shift_ticks + state.temporary_shift_ticks;
}

// Computes the optimal Almgren-Chriss execution trajectory across N time intervals.
[[nodiscard]] inline std::vector<double> compute_optimal_trajectory(double total_shares,
                                                                    double total_time_sec,
                                                                    std::size_t intervals,
                                                                    const ImpactConfig& cfg) {
    MOG_PRE(intervals > 0);
    MOG_PRE(total_time_sec > 0.0);
    std::vector<double> trajectory(intervals + 1, 0.0);
    trajectory[0] = total_shares;

    const double tau = total_time_sec / static_cast<double>(intervals);
    const double sigma = cfg.daily_volatility;
    const double eta = cfg.eta;
    const double lambda = cfg.risk_aversion;

    if (lambda <= 0.0 || eta <= 0.0) {
        // Pure TWAP (linear trajectory) when risk-neutral
        for (std::size_t k = 1; k <= intervals; ++k) {
            trajectory[k] =
                total_shares * (1.0 - static_cast<double>(k) / static_cast<double>(intervals));
        }
        return trajectory;
    }

    const double kappa_arg = 1.0 + 0.5 * lambda * sigma * sigma * tau * tau / eta;
    const double kappa = (1.0 / tau) * std::acosh(std::max(1.0, kappa_arg));

    const double sinh_kappa_T = std::sinh(kappa * total_time_sec);
    if (std::abs(sinh_kappa_T) < 1e-12) {
        for (std::size_t k = 1; k <= intervals; ++k)
            trajectory[k] =
                total_shares * (1.0 - static_cast<double>(k) / static_cast<double>(intervals));
        return trajectory;
    }

    const double coeff = total_shares / sinh_kappa_T;
    for (std::size_t k = 1; k <= intervals; ++k) {
        const double t_k = static_cast<double>(k) * tau;
        trajectory[k] = coeff * std::sinh(kappa * (total_time_sec - t_k));
    }
    return trajectory;
}

// Computes expected total execution shortfall (cost) for a given trade schedule.
[[nodiscard]] inline double compute_expected_shortfall(const std::vector<double>& schedule,
                                                       double tau_sec, const ImpactConfig& cfg) {
    MOG_PRE(schedule.size() >= 2);
    double total_cost = 0.0;
    double accumulated_q = 0.0;

    for (std::size_t i = 1; i < schedule.size(); ++i) {
        const double trade_qty = schedule[i - 1] - schedule[i];
        accumulated_q += trade_qty;
        const double perm_cost =
            cfg.gamma * cfg.daily_volatility * (accumulated_q / cfg.daily_volume) * trade_qty;
        const double rate = (trade_qty / tau_sec) * 23400.0;
        const double temp_cost =
            cfg.eta * cfg.daily_volatility * (rate / cfg.daily_volume) * trade_qty;
        total_cost += (perm_cost + temp_cost);
    }
    return total_cost;
}

} // namespace mog::impact
