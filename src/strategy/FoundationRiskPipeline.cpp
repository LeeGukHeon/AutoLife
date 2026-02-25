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
    const bool rescue_archetype =
        signal.entry_archetype.find("CORE_RESCUE") != std::string::npos;
    const bool range_pullback_archetype =
        signal.entry_archetype.find("FOUNDATION_RANGE_PULLBACK") != std::string::npos;
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

    if (input.policy_block) {
        out.reject_reason = "filtered_out_by_manager_policy_block";
        return out;
    }

    if (input.core_signal_ownership) {
        out.required_strength = std::max(0.0, out.required_strength - 0.02);
        out.required_expected_value = std::min(out.required_expected_value, -0.00005);
    }

    if (input.policy_hold) {
        out.required_strength = std::min(0.95, out.required_strength + 0.05);
        out.required_expected_value += input.core_signal_ownership ? 0.00010 : 0.00018;
    }

    if (input.off_trend_regime) {
        out.required_strength = std::min(0.95, out.required_strength + 0.06);
        out.required_expected_value += input.core_signal_ownership ? 0.00009 : 0.00015;
    }

    if (input.hostile_regime) {
        out.required_strength = std::min(0.95, out.required_strength + 0.03);
        out.required_expected_value += input.core_signal_ownership ? 0.00005 : 0.00008;
    }

    if (probabilistic_primary_signal) {
        // Probabilistic-primary signals already encode forward edge; keep guardrails,
        // but avoid stacking fully static penalties on top.
        out.required_strength = std::max(
            0.0,
            out.required_strength - (0.03 * probabilistic_confidence)
        );
        out.required_expected_value -= (0.00010 * probabilistic_confidence);
    }
    if (probabilistic_primary_signal && !input.hostile_regime) {
        if (rescue_archetype) {
            const double rescue_strength_floor = std::clamp(
                0.30 - (0.08 * probabilistic_confidence),
                0.22,
                0.30
            );
            const double rescue_ev_floor = std::clamp(
                -0.00004 + ((1.0 - probabilistic_confidence) * 0.00016),
                -0.00004,
                0.00012
            );
            out.required_strength = std::max(out.required_strength, rescue_strength_floor);
            out.required_expected_value = std::max(out.required_expected_value, rescue_ev_floor);
            if (signal.probabilistic_h5_calibrated < 0.55 || signal.probabilistic_h5_margin < 0.002) {
                out.required_strength = std::max(out.required_strength, 0.34);
                out.required_expected_value = std::max(out.required_expected_value, 0.00010);
            }
        } else if (range_pullback_archetype) {
            const double range_strength_floor = std::clamp(
                0.26 - (0.07 * probabilistic_confidence),
                0.18,
                0.26
            );
            const double range_ev_floor = std::clamp(
                -0.00008 + ((1.0 - probabilistic_confidence) * 0.00014),
                -0.00008,
                0.00008
            );
            out.required_strength = std::max(out.required_strength, range_strength_floor);
            out.required_expected_value = std::max(out.required_expected_value, range_ev_floor);
            if (signal.probabilistic_h5_calibrated < 0.54 || signal.probabilistic_h5_margin < 0.008) {
                out.required_strength = std::max(out.required_strength, 0.30);
                out.required_expected_value = std::max(out.required_expected_value, 0.00008);
            }
        }
    }

    const bool probabilistic_decision_path = probabilistic_primary_signal && (
        (
            input.hostile_regime &&
            signal.probabilistic_h5_calibrated >= 0.55 &&
            signal.probabilistic_h5_margin >= 0.005 &&
            signal.liquidity_score >= 45.0 &&
            signal.strength >= 0.24 &&
            (out.reward_risk_ratio <= 0.0 || out.reward_risk_ratio >= 1.00)
        ) ||
        (
            !input.hostile_regime &&
            signal.probabilistic_h5_calibrated >= 0.36 &&
            signal.probabilistic_h5_margin >= -0.030 &&
            signal.liquidity_score >= 10.0 &&
            signal.strength >= 0.08
        )
    );
    if (probabilistic_decision_path && !out.frontier_enabled) {
        out.pass = true;
        return out;
    }

    bool history_guard_active = false;
    int effective_history_sample = std::max(1, input.min_history_sample);
    if (probabilistic_primary_signal) {
        effective_history_sample = std::max(
            effective_history_sample,
            input.hostile_regime ? 18 : 36
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
                      0.45 - (0.35 * probabilistic_confidence),
                      input.hostile_regime ? 0.18 : 0.10,
                      input.hostile_regime ? 0.60 : 0.45
                  )
                : 1.0;
            const double strength_bump = probabilistic_primary_signal ? 0.012 : 0.05;
            const double edge_bump_core = probabilistic_primary_signal ? 0.00002 : 0.00005;
            const double edge_bump_other = probabilistic_primary_signal ? 0.00003 : 0.00010;
            out.required_strength = std::min(0.95, out.required_strength + (strength_bump * history_guard_scale));
            out.required_expected_value +=
                (input.core_signal_ownership ? edge_bump_core : edge_bump_other) * history_guard_scale;
            }
        }
    }

    bool rr_guard_active = false;
    const double rr_guard_floor = input.hostile_regime ? 1.12 : 1.08;
    if (out.reward_risk_ratio > 0.0 && out.reward_risk_ratio < rr_guard_floor) {
        const bool rr_guard_skip =
            probabilistic_high_confidence &&
            out.reward_risk_ratio >= 0.95 &&
            !input.hostile_regime;
        if (!rr_guard_skip) {
            rr_guard_active = true;
            const double rr_guard_scale = probabilistic_primary_signal
                ? std::clamp(0.90 - (0.60 * probabilistic_confidence), 0.20, 0.90)
                : 1.0;
            out.required_strength = std::min(0.95, out.required_strength + (0.03 * rr_guard_scale));
            out.required_expected_value +=
                (input.core_signal_ownership ? 0.00003 : 0.00006) * rr_guard_scale;
        }
    }

    if (input.no_trade_bias_active) {
        const double no_trade_bias_scale = probabilistic_primary_signal
            ? std::clamp(1.0 - (0.45 * probabilistic_confidence), 0.40, 1.0)
            : 1.0;
        out.required_strength = std::min(0.95, out.required_strength + (0.02 * no_trade_bias_scale));
        out.required_expected_value += 0.00005 * no_trade_bias_scale;
    }

    const bool low_vol_low_liq_context =
        !input.hostile_regime &&
        signal.liquidity_score >= 22.0 &&
        signal.liquidity_score <= 60.0 &&
        signal.volatility > 0.0 &&
        signal.volatility <= 2.8;
    const bool probabilistic_supply_relief =
        probabilistic_primary_signal &&
        !input.hostile_regime &&
        signal.probabilistic_h5_calibrated >= 0.54 &&
        signal.probabilistic_h5_margin >= -0.01 &&
        signal.liquidity_score >= 28.0;
    if ((low_vol_low_liq_context && probabilistic_primary_signal) || probabilistic_supply_relief) {
        const double relief_scale = probabilistic_supply_relief
            ? std::clamp(0.55 + (0.45 * probabilistic_confidence), 0.55, 1.0)
            : 0.70;
        out.required_strength = std::max(0.28, out.required_strength - (0.05 * relief_scale));
        out.required_expected_value -= (input.core_signal_ownership ? 0.00010 : 0.00007) * relief_scale;
    }

    const bool high_quality_relief =
        !input.hostile_regime &&
        signal.strength >= 0.72 &&
        signal.expected_value >= 0.00060 &&
        out.reward_risk_ratio >= 1.20;
    const bool probabilistic_quality_relief =
        probabilistic_primary_signal &&
        !input.hostile_regime &&
        signal.probabilistic_h5_margin >= 0.01 &&
        signal.probabilistic_h5_calibrated >= 0.58 &&
        signal.liquidity_score >= 54.0;
    if (high_quality_relief || probabilistic_quality_relief) {
        out.required_strength = std::max(0.40, out.required_strength - 0.03);
        out.required_expected_value -= input.core_signal_ownership ? 0.00004 : 0.00003;
    }

    if (probabilistic_primary_signal) {
        const bool calm_context =
            !input.hostile_regime &&
            signal.volatility > 0.0 &&
            signal.volatility <= 3.2 &&
            signal.liquidity_score >= 22.0;
        const double strength_cap = input.hostile_regime
            ? std::clamp(0.50 - (0.08 * probabilistic_confidence), 0.40, 0.50)
            : std::clamp(0.42 - (0.14 * probabilistic_confidence), 0.26, 0.42);
        double ev_cap = input.hostile_regime
            ? (0.00010 - (0.00008 * probabilistic_confidence))
            : (-0.00005 - (0.00028 * probabilistic_confidence));
        if (calm_context) {
            ev_cap -= 0.00010;
        }
        ev_cap = std::clamp(ev_cap, -0.00055, 0.00018);
        out.required_strength = std::min(out.required_strength, strength_cap);
        out.required_expected_value = std::min(out.required_expected_value, ev_cap);
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

    const bool probabilistic_primary_override =
        probabilistic_primary_signal &&
        !out.frontier_enabled &&
        !input.hostile_regime &&
        signal.probabilistic_h5_calibrated >= 0.56 &&
        signal.probabilistic_h5_margin >= -0.002 &&
        signal.liquidity_score >= 20.0 &&
        signal.strength >= 0.26 &&
        signal.expected_value >= -0.00035 &&
        (out.reward_risk_ratio <= 0.0 || out.reward_risk_ratio >= 0.92);
    if (probabilistic_primary_override) {
        out.pass = true;
        return out;
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
