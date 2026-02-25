#include "common/ExecutionGuardPolicyShared.h"

#include <algorithm>
#include <cmath>
#include <cctype>

namespace autolife {
namespace common {
namespace execution_guard {

namespace {

std::string toLowerCopy(const std::string& value) {
    std::string out = value;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

double computeQuantile(std::vector<double> values, double quantile) {
    if (values.empty()) {
        return 0.0;
    }
    quantile = std::clamp(quantile, 0.0, 1.0);
    const size_t idx = static_cast<size_t>(
        std::round(quantile * static_cast<double>(values.size() - 1))
    );
    std::nth_element(values.begin(), values.begin() + idx, values.end());
    return values[idx];
}

} // namespace

double computeHostilityTightenPressure(
    const engine::EngineConfig& cfg,
    double hostility_ewma
) {
    const double hostile_threshold = std::clamp(cfg.hostility_hostile_threshold, 0.0, 1.0);
    const double severe_gap_min = std::max(0.0, cfg.execution_guard_tighten_severe_gap_min);
    const double severe_clamp_min = std::clamp(cfg.execution_guard_tighten_severe_clamp_min, 0.0, 1.0);
    const double severe_clamp_max = std::clamp(cfg.execution_guard_tighten_severe_clamp_max, 0.0, 1.0);
    const double severe_threshold = std::clamp(
        std::max(cfg.hostility_severe_threshold, hostile_threshold + severe_gap_min),
        std::min(severe_clamp_min, severe_clamp_max),
        std::max(severe_clamp_min, severe_clamp_max)
    );
    const double hostility = std::clamp(hostility_ewma, 0.0, 1.0);
    if (hostility <= hostile_threshold) {
        return 0.0;
    }
    const double denom = std::max(1e-6, severe_threshold - hostile_threshold);
    return std::clamp((hostility - hostile_threshold) / denom, 0.0, 1.0);
}

double computeMarketHostilityScore(
    const engine::EngineConfig& cfg,
    const analytics::CoinMetrics& metrics,
    analytics::MarketRegime regime
) {
    double hostility = 0.0;
    switch (regime) {
        case analytics::MarketRegime::HIGH_VOLATILITY:
            hostility += std::clamp(cfg.hostility_score_regime_high_vol, 0.0, 1.0);
            break;
        case analytics::MarketRegime::TRENDING_DOWN:
            hostility += std::clamp(cfg.hostility_score_regime_trending_down, 0.0, 1.0);
            break;
        case analytics::MarketRegime::RANGING:
            hostility += std::clamp(cfg.hostility_score_regime_ranging, 0.0, 1.0);
            break;
        case analytics::MarketRegime::TRENDING_UP:
            hostility += std::clamp(cfg.hostility_score_regime_trending_up, 0.0, 1.0);
            break;
        case analytics::MarketRegime::UNKNOWN:
        default:
            hostility += std::clamp(cfg.hostility_score_regime_unknown, 0.0, 1.0);
            break;
    }

    if (metrics.volatility > 0.0) {
        const double vol_pivot = std::max(0.0, cfg.hostility_score_volatility_pivot);
        const double vol_divisor = std::max(1e-6, cfg.hostility_score_volatility_divisor);
        const double vol_cap = std::max(0.0, cfg.hostility_score_volatility_cap);
        hostility += std::clamp((metrics.volatility - vol_pivot) / vol_divisor, 0.0, vol_cap);
    }
    if (metrics.liquidity_score > 0.0) {
        const double liq_pivot = std::max(0.0, cfg.hostility_score_liquidity_pivot);
        const double liq_divisor = std::max(1e-6, cfg.hostility_score_liquidity_divisor);
        const double liq_cap = std::max(0.0, cfg.hostility_score_liquidity_cap);
        hostility += std::clamp((liq_pivot - metrics.liquidity_score) / liq_divisor, 0.0, liq_cap);
    }
    if (metrics.orderbook_snapshot.valid) {
        const double spread_pct = metrics.orderbook_snapshot.spread_pct * 100.0;
        const double spread_pivot = std::max(0.0, cfg.hostility_score_spread_pct_pivot);
        const double spread_divisor = std::max(1e-6, cfg.hostility_score_spread_pct_divisor);
        const double spread_cap = std::max(0.0, cfg.hostility_score_spread_pct_cap);
        hostility += std::clamp((spread_pct - spread_pivot) / spread_divisor, 0.0, spread_cap);
    }

    return std::clamp(hostility, 0.0, 1.0);
}

LiveScanPrefilterThresholds computeLiveScanPrefilterThresholds(
    const engine::EngineConfig& cfg,
    const std::vector<analytics::CoinMetrics>& markets,
    double hostility_ewma
) {
    LiveScanPrefilterThresholds thresholds;
    const double base_volume = std::max(1.0, static_cast<double>(cfg.min_volume_krw));
    const double base_spread_clamp_min = std::max(
        0.0,
        cfg.execution_guard_live_scan_base_spread_clamp_min
    );
    const double base_spread_clamp_max = std::max(
        base_spread_clamp_min,
        cfg.execution_guard_live_scan_base_spread_clamp_max
    );
    const double base_spread = std::clamp(
        cfg.realtime_entry_veto_max_spread_pct *
            std::max(0.0, cfg.execution_guard_live_scan_base_spread_multiplier),
        base_spread_clamp_min,
        base_spread_clamp_max
    );
    const double tighten_pressure = computeHostilityTightenPressure(cfg, hostility_ewma);

    std::vector<double> volumes;
    volumes.reserve(markets.size());
    for (const auto& m : markets) {
        if (m.volume_24h > 0.0 && std::isfinite(m.volume_24h)) {
            volumes.push_back(m.volume_24h);
        }
    }

    const double base_floor = std::max(
        std::max(0.0, cfg.execution_guard_live_scan_base_floor_krw),
        cfg.min_order_krw * std::max(0.0, cfg.execution_guard_live_scan_base_floor_min_order_multiplier)
    );
    double dynamic_min_volume =
        base_volume *
        (std::max(0.0, cfg.execution_guard_live_scan_volume_tighten_base) +
         (std::max(0.0, cfg.execution_guard_live_scan_volume_tighten_scale) * tighten_pressure));
    if (!volumes.empty()) {
        const double median_volume = computeQuantile(
            volumes,
            std::clamp(cfg.execution_guard_live_scan_volume_quantile_median, 0.0, 1.0)
        );
        const double p70_volume = computeQuantile(
            volumes,
            std::clamp(cfg.execution_guard_live_scan_volume_quantile_p70, 0.0, 1.0)
        );
        const double universe_anchor =
            median_volume *
            (std::max(0.0, cfg.execution_guard_live_scan_universe_anchor_base) +
             (std::max(0.0, cfg.execution_guard_live_scan_universe_anchor_tighten_scale) *
              tighten_pressure));
        dynamic_min_volume = std::max(dynamic_min_volume, universe_anchor);
        const double dynamic_ceiling = std::max(
            base_volume *
                std::max(0.0, cfg.execution_guard_live_scan_dynamic_ceiling_base_volume_multiplier),
            p70_volume *
                std::max(0.0, cfg.execution_guard_live_scan_dynamic_ceiling_p70_multiplier)
        );
        dynamic_min_volume = std::min(dynamic_min_volume, dynamic_ceiling);
    }
    thresholds.min_volume_krw = std::max(base_floor, dynamic_min_volume);

    thresholds.max_spread_pct = base_spread * (
        1.0 -
        (std::max(0.0, cfg.execution_guard_live_scan_spread_tighten_scale) * tighten_pressure)
    );
    const double final_spread_clamp_min = std::max(
        0.0,
        cfg.execution_guard_live_scan_final_spread_clamp_min
    );
    const double final_spread_clamp_max = std::max(
        final_spread_clamp_min,
        cfg.execution_guard_live_scan_final_spread_clamp_max
    );
    thresholds.max_spread_pct = std::clamp(
        thresholds.max_spread_pct,
        final_spread_clamp_min,
        final_spread_clamp_max
    );

    const double ask_notional_min_multiplier = std::max(
        0.0,
        cfg.execution_guard_live_scan_ask_notional_min_multiplier
    );
    const double ask_notional_max_multiplier = std::max(
        ask_notional_min_multiplier,
        cfg.execution_guard_live_scan_ask_notional_max_multiplier
    );
    thresholds.min_ask_notional_krw = std::clamp(
        cfg.min_order_krw *
            (std::max(0.0, cfg.execution_guard_live_scan_ask_notional_base_multiplier) +
             (std::max(0.0, cfg.execution_guard_live_scan_ask_notional_tighten_scale) *
              tighten_pressure)),
        cfg.min_order_krw * ask_notional_min_multiplier,
        cfg.min_order_krw * ask_notional_max_multiplier
    );

    return thresholds;
}

RealtimeEntryVetoThresholds computeRealtimeEntryVetoThresholds(
    const engine::EngineConfig& cfg,
    analytics::MarketRegime regime,
    double signal_strength,
    double liquidity_score,
    double hostility_ewma
) {
    RealtimeEntryVetoThresholds thresholds;
    thresholds.max_drop_pct = std::clamp(cfg.realtime_entry_veto_max_drop_pct, 0.0002, 0.05);
    thresholds.max_spread_pct = std::clamp(cfg.realtime_entry_veto_max_spread_pct, 0.0001, 0.05);
    thresholds.min_orderbook_imbalance =
        std::clamp(cfg.realtime_entry_veto_min_orderbook_imbalance, -1.0, 1.0);

    const double quality_strength_center = std::clamp(
        cfg.execution_guard_veto_quality_strength_center,
        0.0,
        1.0
    );
    const double quality_strength_scale = std::max(
        1e-6,
        cfg.execution_guard_veto_quality_strength_scale
    );
    const double quality_liquidity_center = std::max(
        0.0,
        cfg.execution_guard_veto_quality_liquidity_center
    );
    const double quality_liquidity_scale = std::max(
        1e-6,
        cfg.execution_guard_veto_quality_liquidity_scale
    );
    const double quality_strength_weight = std::max(
        0.0,
        cfg.execution_guard_veto_quality_strength_weight
    );
    const double quality_liquidity_weight = std::max(
        0.0,
        cfg.execution_guard_veto_quality_liquidity_weight
    );
    const double quality_weight_sum = std::max(
        1e-6,
        quality_strength_weight + quality_liquidity_weight
    );

    double tighten_pressure = computeHostilityTightenPressure(cfg, hostility_ewma);

    double quality_relief = 0.0;
    if (tighten_pressure <= 1e-9) {
        const double strength = std::clamp(
            (signal_strength - quality_strength_center) / quality_strength_scale,
            0.0,
            1.0
        );
        const double liquidity = std::clamp(
            (liquidity_score - quality_liquidity_center) / quality_liquidity_scale,
            0.0,
            1.0
        );
        quality_relief =
            ((quality_strength_weight * strength) + (quality_liquidity_weight * liquidity)) /
            quality_weight_sum;
    }

    if (regime == analytics::MarketRegime::HIGH_VOLATILITY ||
        regime == analytics::MarketRegime::TRENDING_DOWN) {
        tighten_pressure = std::min(
            1.0,
            tighten_pressure + std::clamp(cfg.execution_guard_veto_hostile_tighten_add, -1.0, 1.0)
        );
    } else if (regime == analytics::MarketRegime::TRENDING_UP &&
               signal_strength >=
                   std::clamp(cfg.execution_guard_veto_uptrend_relief_strength_threshold, 0.0, 1.0) &&
               liquidity_score >=
                   std::max(0.0, cfg.execution_guard_veto_uptrend_relief_liquidity_threshold)) {
        quality_relief = std::min(
            1.0,
            quality_relief + std::clamp(cfg.execution_guard_veto_uptrend_relief_add, -1.0, 1.0)
        );
    }

    thresholds.max_drop_pct *= (
        1.0 -
        (std::max(0.0, cfg.execution_guard_veto_max_drop_tighten_scale) * tighten_pressure) +
        (std::max(0.0, cfg.execution_guard_veto_max_drop_relief_scale) * quality_relief)
    );
    thresholds.max_spread_pct *= (
        1.0 -
        (std::max(0.0, cfg.execution_guard_veto_max_spread_tighten_scale) * tighten_pressure) +
        (std::max(0.0, cfg.execution_guard_veto_max_spread_relief_scale) * quality_relief)
    );
    thresholds.min_orderbook_imbalance +=
        (std::max(0.0, cfg.execution_guard_veto_min_imbalance_tighten_scale) * tighten_pressure) -
        (std::max(0.0, cfg.execution_guard_veto_min_imbalance_relief_scale) * quality_relief);

    thresholds.max_drop_pct = std::clamp(
        thresholds.max_drop_pct,
        std::min(
            cfg.execution_guard_veto_max_drop_clamp_min,
            cfg.execution_guard_veto_max_drop_clamp_max
        ),
        std::max(
            cfg.execution_guard_veto_max_drop_clamp_min,
            cfg.execution_guard_veto_max_drop_clamp_max
        )
    );
    thresholds.max_spread_pct = std::clamp(
        thresholds.max_spread_pct,
        std::min(
            cfg.execution_guard_veto_max_spread_clamp_min,
            cfg.execution_guard_veto_max_spread_clamp_max
        ),
        std::max(
            cfg.execution_guard_veto_max_spread_clamp_min,
            cfg.execution_guard_veto_max_spread_clamp_max
        )
    );
    thresholds.min_orderbook_imbalance = std::clamp(
        thresholds.min_orderbook_imbalance,
        std::min(
            cfg.execution_guard_veto_min_imbalance_clamp_min,
            cfg.execution_guard_veto_min_imbalance_clamp_max
        ),
        std::max(
            cfg.execution_guard_veto_min_imbalance_clamp_min,
            cfg.execution_guard_veto_min_imbalance_clamp_max
        )
    );
    return thresholds;
}

RealtimeEntryVetoThresholds computeRealtimeEntryVetoThresholds(
    const engine::EngineConfig& cfg,
    const strategy::Signal& signal,
    double hostility_ewma
) {
    return computeRealtimeEntryVetoThresholds(
        cfg,
        signal.market_regime,
        signal.strength,
        signal.liquidity_score,
        hostility_ewma
    );
}

DynamicSlippageThresholds computeDynamicSlippageThresholds(
    const engine::EngineConfig& cfg,
    double hostility_ewma,
    bool is_buy,
    analytics::MarketRegime regime,
    double signal_strength,
    double liquidity_score,
    double expected_value,
    const std::string& exit_reason
) {
    DynamicSlippageThresholds thresholds;
    thresholds.max_slippage_pct = std::clamp(
        cfg.max_slippage_pct,
        std::min(
            cfg.execution_guard_slippage_base_clamp_min,
            cfg.execution_guard_slippage_base_clamp_max
        ),
        std::max(
            cfg.execution_guard_slippage_base_clamp_min,
            cfg.execution_guard_slippage_base_clamp_max
        )
    );
    const double tighten_pressure = computeHostilityTightenPressure(cfg, hostility_ewma);
    const double guard_multiplier = std::max(0.0, cfg.execution_guard_slippage_guard_multiplier);

    if (is_buy) {
        double tighten = tighten_pressure;
        if (regime == analytics::MarketRegime::HIGH_VOLATILITY ||
            regime == analytics::MarketRegime::TRENDING_DOWN) {
            tighten = std::min(
                1.0,
                tighten + std::clamp(cfg.execution_guard_slippage_buy_hostile_tighten_add, -1.0, 1.0)
            );
        }
        const double buy_low_liquidity_threshold =
            std::max(0.0, cfg.execution_guard_slippage_buy_low_liquidity_threshold);
        if (liquidity_score > 0.0 && liquidity_score < buy_low_liquidity_threshold) {
            tighten = std::min(
                1.0,
                tighten +
                    std::clamp(
                        (buy_low_liquidity_threshold - liquidity_score) /
                            std::max(1e-6, cfg.execution_guard_slippage_buy_low_liquidity_scale),
                        0.0,
                        std::max(0.0, cfg.execution_guard_slippage_buy_low_liquidity_tighten_cap)
                    )
            );
        }

        double quality_relief = 0.0;
        if (regime == analytics::MarketRegime::TRENDING_UP ||
            regime == analytics::MarketRegime::RANGING) {
            quality_relief +=
                std::clamp(cfg.execution_guard_slippage_buy_regime_quality_relief_add, -1.0, 1.0);
        }
        quality_relief +=
            std::clamp(
                (signal_strength - std::clamp(cfg.execution_guard_slippage_buy_quality_strength_center, 0.0, 1.0)) /
                    std::max(1e-6, cfg.execution_guard_slippage_buy_quality_strength_scale),
                0.0,
                1.0
            ) *
            std::max(0.0, cfg.execution_guard_slippage_buy_quality_strength_weight);
        quality_relief +=
            std::clamp(
                (liquidity_score -
                 std::max(0.0, cfg.execution_guard_slippage_buy_quality_liquidity_center)) /
                    std::max(1e-6, cfg.execution_guard_slippage_buy_quality_liquidity_scale),
                0.0,
                1.0
            ) *
            std::max(0.0, cfg.execution_guard_slippage_buy_quality_liquidity_weight);
        quality_relief +=
            std::clamp(
                (expected_value - cfg.execution_guard_slippage_buy_quality_ev_center) /
                    std::max(1e-9, cfg.execution_guard_slippage_buy_quality_ev_scale),
                0.0,
                1.0
            ) *
            std::max(0.0, cfg.execution_guard_slippage_buy_quality_ev_weight);
        quality_relief *= (
            1.0 -
            (std::max(0.0, cfg.execution_guard_slippage_buy_quality_tighten_dampen) * tighten)
        );

        thresholds.max_slippage_pct *= (
            1.0 -
            (std::max(0.0, cfg.execution_guard_slippage_buy_tighten_scale) * tighten) +
            (std::max(0.0, cfg.execution_guard_slippage_buy_relief_scale) * quality_relief)
        );
        thresholds.max_slippage_pct = std::clamp(
            thresholds.max_slippage_pct,
            std::min(
                cfg.execution_guard_slippage_buy_clamp_min,
                cfg.execution_guard_slippage_buy_clamp_max
            ),
            std::max(
                cfg.execution_guard_slippage_buy_clamp_min,
                cfg.execution_guard_slippage_buy_clamp_max
            )
        );
    } else {
        const std::string reason_lower = toLowerCopy(exit_reason);
        const bool urgent_exit =
            reason_lower.find("stop") != std::string::npos ||
            reason_lower.find("drawdown") != std::string::npos ||
            reason_lower.find("risk") != std::string::npos ||
            reason_lower.find("liquid") != std::string::npos ||
            reason_lower.find("emergency") != std::string::npos;

        double relax = cfg.execution_guard_slippage_sell_relax_tighten_scale * tighten_pressure;
        if (urgent_exit) {
            relax += cfg.execution_guard_slippage_sell_relax_urgent_add;
        }
        if (liquidity_score > 0.0 &&
            liquidity_score < std::max(0.0, cfg.execution_guard_slippage_sell_relax_low_liquidity_threshold)) {
            relax += cfg.execution_guard_slippage_sell_relax_low_liquidity_add;
        }
        if (!urgent_exit &&
            regime == analytics::MarketRegime::TRENDING_UP &&
            liquidity_score >=
                std::max(0.0, cfg.execution_guard_slippage_sell_relax_uptrend_liquidity_threshold)) {
            relax -= cfg.execution_guard_slippage_sell_relax_uptrend_relief;
        }
        relax = std::clamp(
            relax,
            std::min(
                cfg.execution_guard_slippage_sell_relax_clamp_min,
                cfg.execution_guard_slippage_sell_relax_clamp_max
            ),
            std::max(
                cfg.execution_guard_slippage_sell_relax_clamp_min,
                cfg.execution_guard_slippage_sell_relax_clamp_max
            )
        );

        thresholds.max_slippage_pct *= (1.0 + relax);
        const double sell_clamp_min = std::max(0.0, cfg.execution_guard_slippage_sell_clamp_min);
        const double sell_clamp_cap = urgent_exit
            ? std::max(0.0, cfg.execution_guard_slippage_sell_urgent_cap)
            : std::max(0.0, cfg.execution_guard_slippage_sell_nonurgent_cap);
        thresholds.max_slippage_pct = std::clamp(
            thresholds.max_slippage_pct,
            sell_clamp_min,
            std::max(sell_clamp_min, sell_clamp_cap)
        );
    }

    const double guard_clamp_min = std::max(0.0, cfg.execution_guard_slippage_guard_clamp_min);
    const double guard_clamp_max = is_buy
        ? std::max(0.0, cfg.execution_guard_slippage_guard_buy_clamp_max)
        : std::max(0.0, cfg.execution_guard_slippage_guard_sell_clamp_max);
    thresholds.guard_slippage_pct = std::clamp(
        thresholds.max_slippage_pct * guard_multiplier,
        guard_clamp_min,
        std::max(guard_clamp_min, guard_clamp_max)
    );
    return thresholds;
}

} // namespace execution_guard
} // namespace common
} // namespace autolife

