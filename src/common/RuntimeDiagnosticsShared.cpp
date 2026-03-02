#include "common/RuntimeDiagnosticsShared.h"

#include <cmath>

namespace autolife::common::runtime_diag {

const char* marketRegimeLabel(analytics::MarketRegime regime) {
    switch (regime) {
        case analytics::MarketRegime::TRENDING_UP: return "TRENDING_UP";
        case analytics::MarketRegime::TRENDING_DOWN: return "TRENDING_DOWN";
        case analytics::MarketRegime::RANGING: return "RANGING";
        case analytics::MarketRegime::HIGH_VOLATILITY: return "HIGH_VOLATILITY";
        default: return "UNKNOWN";
    }
}

std::string strengthBucket(double strength) {
    if (strength < 0.55) return "strength_low";
    if (strength < 0.70) return "strength_mid";
    return "strength_high";
}

std::string expectedValueBucket(double expected_value) {
    // expected_value is tracked as calibrated net edge after fee/slippage.
    // Use stricter cutoffs so ev_high reflects meaningful post-cost expectancy.
    if (!std::isfinite(expected_value)) return "ev_negative";
    if (expected_value < 0.0) return "ev_negative";
    if (expected_value < 0.0015) return "ev_neutral";
    if (expected_value < 0.0035) return "ev_positive";
    return "ev_high";
}

std::string rewardRiskBucket(double rr) {
    if (rr < 1.20) return "rr_low";
    if (rr < 1.60) return "rr_mid";
    return "rr_high";
}

std::string volatilityBucket(double volatility) {
    if (!std::isfinite(volatility) || volatility <= 0.0) return "vol_unknown";
    if (volatility < 1.8) return "vol_low";
    if (volatility < 3.5) return "vol_mid";
    return "vol_high";
}

std::string liquidityBucket(double liquidity_score) {
    if (!std::isfinite(liquidity_score) || liquidity_score <= 0.0) return "liq_unknown";
    if (liquidity_score < 55.0) return "liq_low";
    if (liquidity_score < 70.0) return "liq_mid";
    return "liq_high";
}

const char* classifySignalRejectionGroup(std::string_view reason) {
    if (reason == "no_signal_generated") {
        return "signal_generation";
    }
    if (reason == "skipped_due_to_open_position" || reason == "skipped_due_to_active_order") {
        return "position_state";
    }
    if (reason == "no_candle_data") {
        return "data_availability";
    }
    if (reason == "data_parity_window_insufficient") {
        return "data_availability";
    }
    if (reason.rfind("no_best_signal_", 0) == 0 || reason == "no_best_signal") {
        return "best_signal_selection";
    }
    if (reason.rfind("probabilistic_", 0) == 0) {
        return "signal_generation";
    }
    if (reason.rfind("blocked_", 0) == 0) {
        return "risk_or_execution_gate";
    }
    return "other";
}

} // namespace autolife::common::runtime_diag
