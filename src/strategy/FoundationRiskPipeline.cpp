#include "strategy/FoundationRiskPipeline.h"

#include <algorithm>
#include <cmath>

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

    bool history_guard_active = false;
    if (signal.strategy_trade_count >= std::max(1, input.min_history_sample)) {
        if (signal.strategy_win_rate < input.min_history_win_rate ||
            signal.strategy_profit_factor < input.min_history_profit_factor) {
            history_guard_active = true;
            const double history_guard_scale = probabilistic_primary_signal
                ? std::clamp(1.0 - (0.55 * probabilistic_confidence), 0.35, 1.0)
                : 1.0;
            out.required_strength = std::min(0.95, out.required_strength + (0.05 * history_guard_scale));
            out.required_expected_value +=
                (input.core_signal_ownership ? 0.00005 : 0.00010) * history_guard_scale;
        }
    }

    bool rr_guard_active = false;
    if (out.reward_risk_ratio > 0.0 && out.reward_risk_ratio < 1.15) {
        const bool rr_guard_skip =
            probabilistic_high_confidence &&
            out.reward_risk_ratio >= 1.00 &&
            !input.hostile_regime;
        if (!rr_guard_skip) {
            rr_guard_active = true;
            const double rr_guard_scale = probabilistic_primary_signal
                ? std::clamp(1.0 - (0.50 * probabilistic_confidence), 0.35, 1.0)
                : 1.0;
            out.required_strength = std::min(0.95, out.required_strength + (0.05 * rr_guard_scale));
            out.required_expected_value +=
                (input.core_signal_ownership ? 0.00005 : 0.00009) * rr_guard_scale;
        }
    }

    if (input.no_trade_bias_active) {
        const double no_trade_bias_scale = probabilistic_primary_signal
            ? std::clamp(1.0 - (0.45 * probabilistic_confidence), 0.40, 1.0)
            : 1.0;
        out.required_strength = std::min(0.95, out.required_strength + (0.02 * no_trade_bias_scale));
        out.required_expected_value += 0.00005 * no_trade_bias_scale;
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

    const bool strength_pass = signal.strength >= out.required_strength;
    const bool expected_value_pass = signal.expected_value >= out.required_expected_value;
    out.pass = strength_pass && expected_value_pass;
    if (out.pass) {
        return out;
    }

    if (!strength_pass) {
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

    if (!expected_value_pass) {
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
