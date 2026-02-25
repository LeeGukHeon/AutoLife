#include "strategy/FoundationRiskPipeline.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace autolife::strategy::foundation {

double rewardRiskRatio(const Signal& signal) {
    if (!(signal.entry_price > 0.0) || !(signal.stop_loss > 0.0)) {
        return 0.0;
    }
    const double tp = signal.getPrimaryTakeProfit();
    if (!(tp > 0.0)) {
        return 0.0;
    }
    const double risk = std::abs(signal.entry_price - signal.stop_loss);
    if (risk <= 1e-12) {
        return 0.0;
    }
    const double reward = std::abs(tp - signal.entry_price);
    return reward / risk;
}

FilterDecision evaluateFilter(const FilterInput& input) {
    FilterDecision out;
    if (input.signal == nullptr) {
        out.reject_reason = "filtered_out_by_manager_invalid_input";
        return out;
    }

    const Signal& signal = *input.signal;
    out.required_strength = input.min_strength;
    out.required_expected_value = input.min_expected_value;
    out.reward_risk_ratio = rewardRiskRatio(signal);
    const bool probabilistic_primary_signal = signal.probabilistic_runtime_applied;
    const double probabilistic_confidence = probabilistic_primary_signal
        ? std::clamp(
              (std::clamp((signal.probabilistic_h5_margin + 0.02) / 0.12, 0.0, 1.0) * 0.60) +
              (std::clamp((signal.probabilistic_h5_calibrated - 0.50) / 0.25, 0.0, 1.0) * 0.40),
              0.0,
              1.0
          )
        : 0.0;
    const bool probabilistic_high_confidence = probabilistic_confidence >= 0.65;
    out.probabilistic_confidence = probabilistic_confidence;
    out.ev_confidence = std::clamp(signal.phase3.ev_confidence, 0.0, 1.0);
    out.frontier_enabled = signal.phase3.frontier_enabled;
    const auto& manager_policy = signal.phase3.manager_filter;
    const bool use_manager_policy = manager_policy.enabled;
    const auto manager_pick = [&](double policy_value, double legacy_value) {
        return use_manager_policy ? policy_value : legacy_value;
    };
    const auto manager_pick_int = [&](int policy_value, int legacy_value) {
        return use_manager_policy ? policy_value : legacy_value;
    };
    const double required_strength_cap = std::clamp(
        manager_pick(manager_policy.required_strength_cap, 0.95),
        0.0,
        1.0
    );

    if (input.policy_block) {
        out.reject_reason = "filtered_out_by_manager_policy_block";
        return out;
    }

    if (input.core_signal_ownership) {
        out.required_strength = std::max(
            0.0,
            out.required_strength -
                manager_pick(manager_policy.core_signal_ownership_strength_relief, 0.02)
        );
        out.required_expected_value = std::min(
            out.required_expected_value,
            manager_pick(manager_policy.core_signal_ownership_expected_value_floor, -0.00005)
        );
    }

    if (input.policy_hold) {
        out.required_strength = std::min(
            required_strength_cap,
            out.required_strength + manager_pick(manager_policy.policy_hold_strength_add, 0.05)
        );
        out.required_expected_value += input.core_signal_ownership
            ? manager_pick(manager_policy.policy_hold_expected_value_add_core, 0.00010)
            : manager_pick(manager_policy.policy_hold_expected_value_add_other, 0.00018);
    }

    if (input.off_trend_regime) {
        out.required_strength = std::min(
            required_strength_cap,
            out.required_strength + manager_pick(manager_policy.off_trend_strength_add, 0.06)
        );
        out.required_expected_value += input.core_signal_ownership
            ? manager_pick(manager_policy.off_trend_expected_value_add_core, 0.00009)
            : manager_pick(manager_policy.off_trend_expected_value_add_other, 0.00015);
    }

    if (input.hostile_regime) {
        out.required_strength = std::min(
            required_strength_cap,
            out.required_strength + manager_pick(manager_policy.hostile_regime_strength_add, 0.03)
        );
        out.required_expected_value += input.core_signal_ownership
            ? manager_pick(manager_policy.hostile_regime_expected_value_add_core, 0.00005)
            : manager_pick(manager_policy.hostile_regime_expected_value_add_other, 0.00008);
    }

    if (probabilistic_primary_signal) {
        // Probabilistic-primary signals already encode forward edge; keep guardrails,
        // but avoid stacking fully static penalties on top.
        out.required_strength = std::max(
            0.0,
            out.required_strength -
                (manager_pick(
                     manager_policy.probabilistic_confidence_strength_relief_scale,
                     0.03
                 ) *
                 probabilistic_confidence)
        );
        out.required_expected_value -=
            manager_pick(
                manager_policy.probabilistic_confidence_expected_value_relief_scale,
                0.00010
            ) *
            probabilistic_confidence;
    }

    bool history_guard_active = false;
    int effective_history_sample = std::max(1, input.min_history_sample);
    if (probabilistic_primary_signal) {
        effective_history_sample = std::max(
            effective_history_sample,
            input.hostile_regime
                ? manager_pick_int(manager_policy.history_min_sample_hostile, 18)
                : manager_pick_int(manager_policy.history_min_sample_calm, 36)
        );
    }
    if (signal.strategy_trade_count >= effective_history_sample) {
        const double win_rate_shortfall = std::max(
            0.0,
            input.min_history_win_rate - signal.strategy_win_rate
        );
        const double profit_factor_shortfall = std::max(
            0.0,
            input.min_history_profit_factor - signal.strategy_profit_factor
        );
        const bool severe_history_underperformance =
            win_rate_shortfall >= 0.08 ||
            profit_factor_shortfall >= 0.30;
        const bool probabilistic_history_relief =
            probabilistic_primary_signal &&
            !input.hostile_regime &&
            signal.strategy_trade_count < 52 &&
            signal.probabilistic_h5_calibrated >= 0.48 &&
            signal.probabilistic_h5_margin >= -0.012;
        if (signal.strategy_win_rate < input.min_history_win_rate ||
            signal.strategy_profit_factor < input.min_history_profit_factor) {
            if (probabilistic_primary_signal &&
                !input.hostile_regime &&
                (!severe_history_underperformance || probabilistic_history_relief)) {
                // In probabilistic-primary mode, early/mild history drag should not fully
                // shut down candidate supply.
            } else {
                history_guard_active = true;
                const double history_guard_scale = probabilistic_primary_signal
                    ? std::clamp(
                          manager_pick(manager_policy.history_guard_scale_base, 0.45) -
                              (manager_pick(
                                   manager_policy.history_guard_scale_confidence_scale,
                                   0.35
                               ) *
                               probabilistic_confidence),
                          input.hostile_regime
                              ? manager_pick(manager_policy.history_guard_scale_min_hostile, 0.18)
                              : manager_pick(manager_policy.history_guard_scale_min_calm, 0.10),
                          input.hostile_regime
                              ? manager_pick(manager_policy.history_guard_scale_max_hostile, 0.60)
                              : manager_pick(manager_policy.history_guard_scale_max_calm, 0.45)
                      )
                    : 1.0;
                const double strength_bump = probabilistic_primary_signal
                    ? manager_pick(manager_policy.history_strength_bump_prob, 0.012)
                    : manager_pick(manager_policy.history_strength_bump_non_prob, 0.05);
                const double edge_bump_core = probabilistic_primary_signal
                    ? manager_pick(manager_policy.history_edge_bump_core_prob, 0.00002)
                    : manager_pick(manager_policy.history_edge_bump_core_non_prob, 0.00005);
                const double edge_bump_other = probabilistic_primary_signal
                    ? manager_pick(manager_policy.history_edge_bump_other_prob, 0.00003)
                    : manager_pick(manager_policy.history_edge_bump_other_non_prob, 0.00010);
                out.required_strength = std::min(
                    required_strength_cap,
                    out.required_strength + (strength_bump * history_guard_scale)
                );
                out.required_expected_value +=
                    (input.core_signal_ownership ? edge_bump_core : edge_bump_other) * history_guard_scale;
            }
        }
    }

    bool rr_guard_active = false;
    const double rr_guard_floor = input.hostile_regime
        ? manager_pick(manager_policy.rr_guard_floor_hostile, 1.12)
        : manager_pick(manager_policy.rr_guard_floor_calm, 1.08);
    if (out.reward_risk_ratio > 0.0 && out.reward_risk_ratio < rr_guard_floor) {
        const bool rr_guard_skip =
            probabilistic_high_confidence &&
            out.reward_risk_ratio >= manager_pick(manager_policy.rr_guard_skip_min_rr, 0.95) &&
            !input.hostile_regime;
        if (!rr_guard_skip) {
            rr_guard_active = true;
            const double rr_guard_scale = probabilistic_primary_signal
                ? std::clamp(
                      manager_pick(manager_policy.rr_guard_scale_base, 0.90) -
                          (manager_pick(
                               manager_policy.rr_guard_scale_confidence_scale,
                               0.60
                           ) *
                           probabilistic_confidence),
                      manager_pick(manager_policy.rr_guard_scale_min, 0.20),
                      manager_pick(manager_policy.rr_guard_scale_max, 0.90)
                  )
                : 1.0;
            out.required_strength = std::min(
                required_strength_cap,
                out.required_strength +
                    (manager_pick(manager_policy.rr_guard_strength_add, 0.03) * rr_guard_scale)
            );
            out.required_expected_value +=
                (input.core_signal_ownership
                     ? manager_pick(manager_policy.rr_guard_expected_value_add_core, 0.00003)
                     : manager_pick(manager_policy.rr_guard_expected_value_add_other, 0.00006)) *
                rr_guard_scale;
        }
    }

    out.base_required_expected_value = out.required_expected_value;
    out.frontier_required_expected_value = out.required_expected_value;
    out.margin_pass = true;
    out.ev_confidence_pass = true;
    out.cost_tail_pass = true;
    if (out.frontier_enabled) {
        const double margin = std::clamp(signal.probabilistic_h5_margin, -1.0, 1.0);
        const double margin_floor = std::clamp(signal.phase3.frontier_margin_floor, -1.0, 1.0);
        out.margin_pass = margin >= margin_floor;
        out.uncertainty_term = std::clamp(
            ((1.0 - probabilistic_confidence) * 0.60) +
                ((1.0 - out.ev_confidence) * 0.40),
            0.0,
            1.0
        );
        out.cost_tail_term = std::max(0.0, signal.phase3.cost_tail_pct - signal.phase3.cost_used_pct);
        double required_ev =
            out.base_required_expected_value -
            (signal.phase3.frontier_k_margin * margin) +
            (signal.phase3.frontier_k_uncertainty * out.uncertainty_term) +
            (signal.phase3.frontier_k_cost_tail * out.cost_tail_term);
        required_ev = std::clamp(
            required_ev,
            signal.phase3.frontier_min_required_ev,
            signal.phase3.frontier_max_required_ev
        );
        out.frontier_required_expected_value = required_ev;
        out.required_expected_value = required_ev;

        const double ev_confidence_floor =
            std::clamp(signal.phase3.frontier_ev_confidence_floor, 0.0, 1.0);
        out.ev_confidence_pass = out.ev_confidence >= ev_confidence_floor;

        const double cost_tail_threshold =
            std::clamp(signal.phase3.frontier_cost_tail_reject_threshold_pct, 0.0, 1.0);
        out.cost_tail_pass = out.cost_tail_term <= cost_tail_threshold;
    }

    out.strength_pass = signal.strength >= out.required_strength;
    out.expected_value_pass = signal.expected_value >= out.required_expected_value;
    out.frontier_pass =
        out.margin_pass &&
        out.ev_confidence_pass &&
        out.cost_tail_pass &&
        out.expected_value_pass;
    out.pass = out.frontier_enabled
        ? (out.strength_pass && out.frontier_pass)
        : (out.strength_pass && out.expected_value_pass);
    if (out.pass) {
        return out;
    }

    if (out.frontier_enabled && !out.margin_pass) {
        out.reject_reason = "filtered_out_by_manager_margin_insufficient";
        return out;
    }

    if (out.frontier_enabled && !out.ev_confidence_pass) {
        out.reject_reason = "filtered_out_by_manager_ev_confidence_low";
        return out;
    }

    if (out.frontier_enabled && signal.phase3.cost_tail_enabled && !out.cost_tail_pass) {
        out.reject_reason = "filtered_out_by_manager_cost_tail_fail";
        return out;
    }

    if (!out.strength_pass) {
        if (rr_guard_active) {
            out.reject_reason = "filtered_out_by_manager_rr_guard_strength";
        } else if (history_guard_active) {
            out.reject_reason = "filtered_out_by_manager_history_guard_strength";
        } else if (input.off_trend_regime) {
            out.reject_reason = "filtered_out_by_manager_trend_off_regime_strength";
        } else {
            out.reject_reason = "filtered_out_by_manager_strength";
        }
        return out;
    }

    if (out.frontier_enabled && !out.frontier_pass) {
        out.reject_reason = "filtered_out_by_manager_frontier";
        return out;
    }

    if (!out.expected_value_pass) {
        if (rr_guard_active) {
            out.reject_reason = "filtered_out_by_manager_rr_guard_ev";
        } else if (history_guard_active) {
            out.reject_reason = "filtered_out_by_manager_history_guard_ev";
        } else if (input.off_trend_regime) {
            out.reject_reason = "filtered_out_by_manager_trend_off_regime_ev";
        } else {
            out.reject_reason = "filtered_out_by_manager_ev_quality_floor";
        }
        return out;
    }

    out.reject_reason = "filtered_out_by_manager_unknown";
    return out;
}

}  // namespace autolife::strategy::foundation
