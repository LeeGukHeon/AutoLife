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
    const auto& manager_policy = signal.phase3.manager_filter;
    const auto defaults = Signal::Phase3PolicySnapshot::ManagerFilterPolicy{};
    const bool use_manager_policy = manager_policy.enabled;
    const auto manager_pick = [&](double policy_value, double fallback_value) {
        return use_manager_policy ? policy_value : fallback_value;
    };
    const auto manager_pick_int = [&](int policy_value, int fallback_value) {
        return use_manager_policy ? policy_value : fallback_value;
    };
    const bool probabilistic_primary_signal = signal.probabilistic_runtime_applied;
    const double probabilistic_confidence = probabilistic_primary_signal
        ? std::clamp(
              ((std::clamp(
                    (signal.probabilistic_h5_margin +
                     manager_pick(
                         manager_policy.probabilistic_confidence_margin_shift,
                         defaults.probabilistic_confidence_margin_shift
                     )) /
                        std::max(
                            1e-6,
                            manager_pick(
                                manager_policy.probabilistic_confidence_margin_scale,
                                defaults.probabilistic_confidence_margin_scale
                            )
                        ),
                    0.0,
                    1.0
                ) *
                std::max(
                    0.0,
                    manager_pick(
                        manager_policy.probabilistic_confidence_margin_weight,
                        defaults.probabilistic_confidence_margin_weight
                    )
                )) +
               (std::clamp(
                    (signal.probabilistic_h5_calibrated -
                     manager_pick(
                         manager_policy.probabilistic_confidence_prob_shift,
                         defaults.probabilistic_confidence_prob_shift
                     )) /
                        std::max(
                            1e-6,
                            manager_pick(
                                manager_policy.probabilistic_confidence_prob_scale,
                                defaults.probabilistic_confidence_prob_scale
                            )
                        ),
                    0.0,
                    1.0
                ) *
                std::max(
                    0.0,
                    manager_pick(
                        manager_policy.probabilistic_confidence_prob_weight,
                        defaults.probabilistic_confidence_prob_weight
                    )
                ))) /
                  std::max(
                      1e-6,
                      std::max(
                          0.0,
                          manager_pick(
                              manager_policy.probabilistic_confidence_margin_weight,
                              defaults.probabilistic_confidence_margin_weight
                          )
                      ) +
                          std::max(
                              0.0,
                              manager_pick(
                                  manager_policy.probabilistic_confidence_prob_weight,
                                  defaults.probabilistic_confidence_prob_weight
                              )
                          )
                  ),
              0.0,
              1.0
          )
        : 0.0;
    const bool probabilistic_high_confidence =
        probabilistic_confidence >= manager_pick(
            manager_policy.probabilistic_high_confidence_threshold,
            defaults.probabilistic_high_confidence_threshold
        );
    out.probabilistic_confidence = probabilistic_confidence;
    out.ev_confidence = std::clamp(signal.phase3.ev_confidence, 0.0, 1.0);
    out.frontier_enabled = signal.phase3.frontier_enabled;
    const double required_strength_cap = std::clamp(
        manager_pick(manager_policy.required_strength_cap, defaults.required_strength_cap),
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
                manager_pick(
                    manager_policy.core_signal_ownership_strength_relief,
                    defaults.core_signal_ownership_strength_relief
                )
        );
        out.required_expected_value = std::min(
            out.required_expected_value,
            manager_pick(
                manager_policy.core_signal_ownership_expected_value_floor,
                defaults.core_signal_ownership_expected_value_floor
            )
        );
    }

    if (input.policy_hold) {
        out.required_strength = std::min(
            required_strength_cap,
            out.required_strength + manager_pick(
                manager_policy.policy_hold_strength_add,
                defaults.policy_hold_strength_add
            )
        );
        out.required_expected_value += input.core_signal_ownership
            ? manager_pick(
                manager_policy.policy_hold_expected_value_add_core,
                defaults.policy_hold_expected_value_add_core
            )
            : manager_pick(
                manager_policy.policy_hold_expected_value_add_other,
                defaults.policy_hold_expected_value_add_other
            );
    }

    if (input.off_trend_regime) {
        out.required_strength = std::min(
            required_strength_cap,
            out.required_strength + manager_pick(
                manager_policy.off_trend_strength_add,
                defaults.off_trend_strength_add
            )
        );
        out.required_expected_value += input.core_signal_ownership
            ? manager_pick(
                manager_policy.off_trend_expected_value_add_core,
                defaults.off_trend_expected_value_add_core
            )
            : manager_pick(
                manager_policy.off_trend_expected_value_add_other,
                defaults.off_trend_expected_value_add_other
            );
    }

    if (input.hostile_regime) {
        out.required_strength = std::min(
            required_strength_cap,
            out.required_strength + manager_pick(
                manager_policy.hostile_regime_strength_add,
                defaults.hostile_regime_strength_add
            )
        );
        out.required_expected_value += input.core_signal_ownership
            ? manager_pick(
                manager_policy.hostile_regime_expected_value_add_core,
                defaults.hostile_regime_expected_value_add_core
            )
            : manager_pick(
                manager_policy.hostile_regime_expected_value_add_other,
                defaults.hostile_regime_expected_value_add_other
            );
    }

    if (probabilistic_primary_signal) {
        // Probabilistic-primary signals already encode forward edge; keep guardrails,
        // but avoid stacking fully static penalties on top.
        out.required_strength = std::max(
            0.0,
            out.required_strength -
                (manager_pick(
                     manager_policy.probabilistic_confidence_strength_relief_scale,
                     defaults.probabilistic_confidence_strength_relief_scale
                 ) *
                 probabilistic_confidence)
        );
        out.required_expected_value -=
            manager_pick(
                manager_policy.probabilistic_confidence_expected_value_relief_scale,
                defaults.probabilistic_confidence_expected_value_relief_scale
            ) *
            probabilistic_confidence;
    }

    bool history_guard_active = false;
    int effective_history_sample = std::max(1, input.min_history_sample);
    if (probabilistic_primary_signal) {
        effective_history_sample = std::max(
            effective_history_sample,
            input.hostile_regime
                ? manager_pick_int(
                    manager_policy.history_min_sample_hostile,
                    defaults.history_min_sample_hostile
                )
                : manager_pick_int(
                    manager_policy.history_min_sample_calm,
                    defaults.history_min_sample_calm
                )
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
            win_rate_shortfall >= manager_pick(
                manager_policy.history_severe_win_rate_shortfall,
                defaults.history_severe_win_rate_shortfall
            ) ||
            profit_factor_shortfall >= manager_pick(
                manager_policy.history_severe_profit_factor_shortfall,
                defaults.history_severe_profit_factor_shortfall
            );
        const bool probabilistic_history_relief =
            probabilistic_primary_signal &&
            !input.hostile_regime &&
            signal.strategy_trade_count <
                manager_pick_int(
                    manager_policy.history_relief_max_trade_count,
                    defaults.history_relief_max_trade_count
                ) &&
            signal.probabilistic_h5_calibrated >= manager_pick(
                manager_policy.history_relief_min_h5_calibrated,
                defaults.history_relief_min_h5_calibrated
            ) &&
            signal.probabilistic_h5_margin >= manager_pick(
                manager_policy.history_relief_min_h5_margin,
                defaults.history_relief_min_h5_margin
            );
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
                          manager_pick(
                              manager_policy.history_guard_scale_base,
                              defaults.history_guard_scale_base
                          ) -
                              (manager_pick(
                                   manager_policy.history_guard_scale_confidence_scale,
                                   defaults.history_guard_scale_confidence_scale
                               ) *
                               probabilistic_confidence),
                          input.hostile_regime
                              ? manager_pick(
                                  manager_policy.history_guard_scale_min_hostile,
                                  defaults.history_guard_scale_min_hostile
                              )
                              : manager_pick(
                                  manager_policy.history_guard_scale_min_calm,
                                  defaults.history_guard_scale_min_calm
                              ),
                          input.hostile_regime
                              ? manager_pick(
                                  manager_policy.history_guard_scale_max_hostile,
                                  defaults.history_guard_scale_max_hostile
                              )
                              : manager_pick(
                                  manager_policy.history_guard_scale_max_calm,
                                  defaults.history_guard_scale_max_calm
                              )
                      )
                    : 1.0;
                const double strength_bump = probabilistic_primary_signal
                    ? manager_pick(
                        manager_policy.history_strength_bump_prob,
                        defaults.history_strength_bump_prob
                    )
                    : manager_pick(
                        manager_policy.history_strength_bump_non_prob,
                        defaults.history_strength_bump_non_prob
                    );
                const double edge_bump_core = probabilistic_primary_signal
                    ? manager_pick(
                        manager_policy.history_edge_bump_core_prob,
                        defaults.history_edge_bump_core_prob
                    )
                    : manager_pick(
                        manager_policy.history_edge_bump_core_non_prob,
                        defaults.history_edge_bump_core_non_prob
                    );
                const double edge_bump_other = probabilistic_primary_signal
                    ? manager_pick(
                        manager_policy.history_edge_bump_other_prob,
                        defaults.history_edge_bump_other_prob
                    )
                    : manager_pick(
                        manager_policy.history_edge_bump_other_non_prob,
                        defaults.history_edge_bump_other_non_prob
                    );
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
        ? manager_pick(manager_policy.rr_guard_floor_hostile, defaults.rr_guard_floor_hostile)
        : manager_pick(manager_policy.rr_guard_floor_calm, defaults.rr_guard_floor_calm);
    if (out.reward_risk_ratio > 0.0 && out.reward_risk_ratio < rr_guard_floor) {
        const bool rr_guard_skip =
            probabilistic_high_confidence &&
            out.reward_risk_ratio >= manager_pick(
                manager_policy.rr_guard_skip_min_rr,
                defaults.rr_guard_skip_min_rr
            ) &&
            !input.hostile_regime;
        if (!rr_guard_skip) {
            rr_guard_active = true;
            const double rr_guard_scale = probabilistic_primary_signal
                ? std::clamp(
                      manager_pick(
                          manager_policy.rr_guard_scale_base,
                          defaults.rr_guard_scale_base
                      ) -
                          (manager_pick(
                                manager_policy.rr_guard_scale_confidence_scale,
                                defaults.rr_guard_scale_confidence_scale
                            ) *
                            probabilistic_confidence),
                      manager_pick(manager_policy.rr_guard_scale_min, defaults.rr_guard_scale_min),
                      manager_pick(manager_policy.rr_guard_scale_max, defaults.rr_guard_scale_max)
                  )
                : 1.0;
            out.required_strength = std::min(
                required_strength_cap,
                out.required_strength +
                    (manager_pick(manager_policy.rr_guard_strength_add, defaults.rr_guard_strength_add) *
                     rr_guard_scale)
            );
            out.required_expected_value +=
                (input.core_signal_ownership
                     ? manager_pick(
                         manager_policy.rr_guard_expected_value_add_core,
                         defaults.rr_guard_expected_value_add_core
                     )
                     : manager_pick(
                         manager_policy.rr_guard_expected_value_add_other,
                         defaults.rr_guard_expected_value_add_other
                     )) *
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
        const double uncertainty_prob_weight = std::max(
            0.0,
            manager_pick(
                manager_policy.frontier_uncertainty_prob_weight,
                defaults.frontier_uncertainty_prob_weight
            )
        );
        const double uncertainty_ev_weight = std::max(
            0.0,
            manager_pick(
                manager_policy.frontier_uncertainty_ev_weight,
                defaults.frontier_uncertainty_ev_weight
            )
        );
        const double uncertainty_weight_denom = std::max(1e-6, uncertainty_prob_weight + uncertainty_ev_weight);
        out.margin_pass = margin >= margin_floor;
        out.uncertainty_term = std::clamp(
            (((1.0 - probabilistic_confidence) * uncertainty_prob_weight) +
             ((1.0 - out.ev_confidence) * uncertainty_ev_weight)) /
                uncertainty_weight_denom,
            0.0,
            1.0
        );
        out.cost_tail_term = std::max(0.0, signal.phase3.cost_tail_pct - signal.phase3.cost_used_pct);
        const double k_margin_effective =
            signal.phase3.frontier_k_margin * std::clamp(signal.phase3.frontier_k_margin_scale, 0.0, 10.0);
        double required_ev =
            out.base_required_expected_value -
            (k_margin_effective * margin) +
            (signal.phase3.frontier_k_uncertainty * out.uncertainty_term) +
            (signal.phase3.frontier_k_cost_tail * out.cost_tail_term) +
            signal.phase3.required_ev_offset;
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
