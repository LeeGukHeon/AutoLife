#include "strategy/FoundationAdaptiveStrategy.h"

#include "analytics/TechnicalIndicators.h"
#include "common/Config.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

namespace autolife {
namespace strategy {

namespace {

double safeAtr(const std::vector<Candle>& candles, double entry_price) {
    double atr = analytics::TechnicalIndicators::calculateATR(candles, 14);
    if (!std::isfinite(atr) || atr <= 0.0) {
        atr = entry_price * 0.0045;
    }
    return atr;
}

double rollingReturn(const std::vector<double>& closes, std::size_t lookback) {
    if (closes.empty() || lookback == 0 || closes.size() <= lookback) {
        return 0.0;
    }
    const double now = closes.back();
    const double prev = closes[closes.size() - 1 - lookback];
    if (prev <= 1e-9) {
        return 0.0;
    }
    return (now - prev) / prev;
}

bool isSignalSupplyFallbackEnabled() {
    const auto& cfg = Config::getInstance().getEngineConfig();
    if (!cfg.foundation_signal_supply_fallback_enabled) {
        return false;
    }
    // Keep fallback tied to probabilistic-aware runtime path.
    return cfg.enable_probabilistic_runtime_model && cfg.probabilistic_runtime_primary_mode;
}

struct MtfConfirmation {
    bool pass = false;
    const char* reject_reason = "foundation_no_signal_mtf_window";
    double score = 0.0;
    double momentum_5m = 0.0;
    double momentum_15m = 0.0;
    double momentum_1h = 0.0;
    double ema_gap_15m = 0.0;
    double ema_gap_1h = 0.0;
    double rsi_5m = 50.0;
    double rsi_15m = 50.0;
};

std::vector<double> extractTfCloses(
    const analytics::CoinMetrics& metrics,
    const std::string& tf_key
) {
    const auto it = metrics.candles_by_tf.find(tf_key);
    if (it == metrics.candles_by_tf.end() || it->second.empty()) {
        return {};
    }
    return analytics::TechnicalIndicators::extractClosePrices(it->second);
}

double safeEmaGap(const std::vector<double>& closes, int fast, int slow) {
    if (closes.size() < static_cast<size_t>(std::max(fast, slow)) + 2) {
        return 0.0;
    }
    const double ema_fast = analytics::TechnicalIndicators::calculateEMA(closes, fast);
    const double ema_slow = analytics::TechnicalIndicators::calculateEMA(closes, slow);
    if (!std::isfinite(ema_fast) || !std::isfinite(ema_slow) || ema_slow <= 1e-9) {
        return 0.0;
    }
    return (ema_fast - ema_slow) / ema_slow;
}

MtfConfirmation evaluateMtfConfirmation(
    const analytics::CoinMetrics& metrics,
    analytics::MarketRegime regime
) {
    MtfConfirmation out;

    const auto closes_5m = extractTfCloses(metrics, "5m");
    const auto closes_15m = extractTfCloses(metrics, "15m");
    const auto closes_1h = extractTfCloses(metrics, "1h");
    if (closes_5m.size() < 24 || closes_15m.size() < 20 || closes_1h.size() < 16) {
        out.reject_reason = "foundation_no_signal_mtf_window";
        return out;
    }

    out.momentum_5m = rollingReturn(closes_5m, 6);
    out.momentum_15m = rollingReturn(closes_15m, 4);
    out.momentum_1h = rollingReturn(closes_1h, 3);
    out.ema_gap_15m = safeEmaGap(closes_15m, 8, 21);
    out.ema_gap_1h = safeEmaGap(closes_1h, 5, 13);
    out.rsi_5m = analytics::TechnicalIndicators::calculateRSI(closes_5m, 14);
    out.rsi_15m = analytics::TechnicalIndicators::calculateRSI(closes_15m, 14);

    double score = 0.50;
    score += std::clamp(out.momentum_5m * 14.0, -0.08, 0.08);
    score += std::clamp(out.momentum_15m * 18.0, -0.10, 0.10);
    score += std::clamp(out.momentum_1h * 12.0, -0.08, 0.08);
    score += std::clamp(out.ema_gap_15m * 16.0, -0.08, 0.08);
    score += std::clamp(out.ema_gap_1h * 10.0, -0.06, 0.06);
    score += std::clamp((56.0 - out.rsi_15m) / 180.0, -0.05, 0.05);
    score += std::clamp((54.0 - out.rsi_5m) / 220.0, -0.04, 0.04);
    out.score = std::clamp(score, 0.0, 1.0);

    switch (regime) {
        case analytics::MarketRegime::TRENDING_UP:
            out.pass =
                out.momentum_15m >= -0.0020 &&
                out.ema_gap_15m >= -0.0014 &&
                out.momentum_1h >= -0.0050 &&
                out.rsi_15m <= 71.0 &&
                out.score >= 0.43;
            out.reject_reason = "foundation_no_signal_mtf_uptrend_mismatch";
            break;
        case analytics::MarketRegime::RANGING:
            out.pass =
                std::abs(out.momentum_15m) <= 0.0240 &&
                out.rsi_15m <= 66.0 &&
                out.score >= 0.37;
            out.reject_reason = "foundation_no_signal_mtf_ranging_mismatch";
            break;
        case analytics::MarketRegime::TRENDING_DOWN:
        case analytics::MarketRegime::HIGH_VOLATILITY:
            out.pass =
                out.momentum_1h >= -0.0100 &&
                out.momentum_15m >= -0.0070 &&
                out.rsi_5m <= 50.0 &&
                out.score >= 0.50;
            out.reject_reason = "foundation_no_signal_mtf_hostile_mismatch";
            break;
        case analytics::MarketRegime::UNKNOWN:
        default:
            out.pass = out.score >= 0.47;
            out.reject_reason = "foundation_no_signal_mtf_unknown_mismatch";
            break;
    }
    return out;
}

bool isLowLiquidityUptrendOpportunity(
    const analytics::CoinMetrics& metrics,
    const std::vector<double>& closes,
    double current_price,
    double ema_fast,
    double ema_slow,
    double rsi
) {
    if (metrics.liquidity_score < 34.0 || metrics.liquidity_score >= 48.0) {
        return false;
    }
    if (metrics.volume_surge_ratio < 0.62 || metrics.volume_surge_ratio > 1.70) {
        return false;
    }
    if (metrics.order_book_imbalance <= -0.06) {
        return false;
    }

    const double ret5 = rollingReturn(closes, 5);
    const double ret20 = rollingReturn(closes, 20);
    if (ret5 < 0.0004 || ret20 < 0.0010) {
        return false;
    }

    return (
        current_price > ema_fast * 0.9992 &&
        ema_fast >= ema_slow * 0.9990 &&
        rsi >= 48.0 && rsi <= 68.0
    );
}

bool isUptrendLowFlowProbeOpportunity(
    const analytics::CoinMetrics& metrics,
    const std::vector<double>& closes,
    double current_price,
    double ema_fast,
    double ema_slow,
    double rsi
) {
    if (metrics.liquidity_score < 22.0 || metrics.liquidity_score >= 40.0) {
        return false;
    }
    if (metrics.volume_surge_ratio < 0.48 || metrics.volume_surge_ratio > 1.85) {
        return false;
    }
    if (metrics.orderbook_snapshot.valid && metrics.orderbook_snapshot.spread_pct > 0.0024) {
        return false;
    }
    if (metrics.order_book_imbalance < -0.08) {
        return false;
    }
    if (metrics.buy_pressure < (metrics.sell_pressure * 0.88)) {
        return false;
    }

    const double ret3 = rollingReturn(closes, 3);
    const double ret8 = rollingReturn(closes, 8);
    const double ret20 = rollingReturn(closes, 20);
    if (ret3 < -0.0012 || ret8 < -0.0018 || ret20 < 0.0001) {
        return false;
    }

    return (
        current_price >= ema_fast * 0.9988 &&
        ema_fast >= ema_slow * 0.9988 &&
        rsi >= 41.0 && rsi <= 65.0
    );
}

bool isThinLiquidityAdaptiveOpportunity(
    const analytics::CoinMetrics& metrics,
    const std::vector<double>& closes,
    double current_price,
    double ema_fast,
    double ema_slow,
    double rsi,
    double bb_middle,
    analytics::MarketRegime regime
) {
    if (metrics.liquidity_score < 30.0 || metrics.liquidity_score >= 56.0) {
        return false;
    }
    if (metrics.volume_surge_ratio < 0.56 || metrics.volume_surge_ratio > 1.85) {
        return false;
    }
    if (metrics.orderbook_snapshot.valid && metrics.orderbook_snapshot.spread_pct > 0.0038) {
        return false;
    }

    const double ret3 = rollingReturn(closes, 3);
    const double ret8 = rollingReturn(closes, 8);
    const double ret20 = rollingReturn(closes, 20);

    switch (regime) {
        case analytics::MarketRegime::TRENDING_UP:
            return (
                current_price >= ema_fast * 0.9995 &&
                ema_fast >= ema_slow * 0.9990 &&
                rsi >= 44.0 && rsi <= 68.0 &&
                ret8 >= -0.0004 &&
                ret20 >= 0.0006 &&
                metrics.order_book_imbalance > -0.08
            );
        case analytics::MarketRegime::RANGING:
            return (
                current_price <= bb_middle * 1.0030 &&
                rsi >= 34.0 && rsi <= 50.0 &&
                ret3 <= 0.0018 &&
                ret20 >= -0.0015 &&
                metrics.order_book_imbalance > -0.12
            );
        case analytics::MarketRegime::TRENDING_DOWN:
        case analytics::MarketRegime::HIGH_VOLATILITY:
            return (
                rsi <= 35.0 &&
                current_price >= ema_fast * 0.9985 &&
                metrics.buy_pressure >= (metrics.sell_pressure * 0.95) &&
                ret3 >= -0.0010 &&
                ret8 >= -0.0020
            );
        case analytics::MarketRegime::UNKNOWN:
        default:
            return (
                current_price >= ema_fast * 0.9990 &&
                rsi <= 55.0 &&
                metrics.order_book_imbalance > -0.08 &&
                ret8 >= -0.0006
            );
    }
}

bool isUltraThinLiquidityProbeOpportunity(
    const analytics::CoinMetrics& metrics,
    const std::vector<double>& closes,
    double current_price,
    double ema_fast,
    double ema_slow,
    double rsi,
    double bb_middle,
    analytics::MarketRegime regime
) {
    if (metrics.liquidity_score < 18.0 || metrics.liquidity_score >= 36.0) {
        return false;
    }
    if (metrics.volume_surge_ratio < 0.70 || metrics.volume_surge_ratio > 2.00) {
        return false;
    }
    if (metrics.orderbook_snapshot.valid && metrics.orderbook_snapshot.spread_pct > 0.0032) {
        return false;
    }

    const double ret3 = rollingReturn(closes, 3);
    const double ret5 = rollingReturn(closes, 5);
    const double ret20 = rollingReturn(closes, 20);

    switch (regime) {
        case analytics::MarketRegime::TRENDING_UP:
            return (
                current_price >= ema_fast * 0.9998 &&
                ema_fast >= ema_slow * 0.9995 &&
                rsi >= 46.0 && rsi <= 63.0 &&
                ret5 >= 0.0001 &&
                ret20 >= 0.0012 &&
                metrics.order_book_imbalance >= -0.02 &&
                metrics.buy_pressure >= (metrics.sell_pressure * 0.95)
            );
        case analytics::MarketRegime::RANGING:
            return (
                current_price <= bb_middle * 1.0018 &&
                rsi >= 34.0 && rsi <= 45.0 &&
                ret3 <= 0.0012 &&
                ret20 >= -0.0010 &&
                metrics.order_book_imbalance >= 0.0 &&
                metrics.buy_pressure >= metrics.sell_pressure
            );
        case analytics::MarketRegime::TRENDING_DOWN:
        case analytics::MarketRegime::HIGH_VOLATILITY:
        case analytics::MarketRegime::UNKNOWN:
        default:
            return false;
    }
}

bool isRangingLowFlowOpportunity(
    const analytics::CoinMetrics& metrics,
    const std::vector<double>& closes,
    double current_price,
    double ema_fast,
    double rsi,
    double bb_middle
) {
    if (metrics.liquidity_score < 22.0 || metrics.liquidity_score >= 55.0) {
        return false;
    }
    if (metrics.volume_surge_ratio < 0.34 || metrics.volume_surge_ratio > 1.30) {
        return false;
    }
    if (metrics.orderbook_snapshot.valid && metrics.orderbook_snapshot.spread_pct > 0.0042) {
        return false;
    }

    const double ret3 = rollingReturn(closes, 3);
    const double ret8 = rollingReturn(closes, 8);
    return (
        current_price <= bb_middle * 1.0030 &&
        current_price >= ema_fast * 0.9950 &&
        rsi >= 28.0 && rsi <= 52.0 &&
        ret3 <= 0.0015 &&
        ret8 >= -0.0034 &&
        metrics.order_book_imbalance > -0.14 &&
        metrics.buy_pressure >= (metrics.sell_pressure * 0.82)
    );
}

bool isRangingSignalSupplyFallbackOpportunity(
    const analytics::CoinMetrics& metrics,
    const std::vector<double>& closes,
    double current_price,
    double ema_fast,
    double rsi,
    double bb_middle
) {
    if (metrics.liquidity_score < 20.0 || metrics.liquidity_score >= 56.0) {
        return false;
    }
    if (metrics.volume_surge_ratio < 0.28 || metrics.volume_surge_ratio > 1.70) {
        return false;
    }
    if (metrics.orderbook_snapshot.valid &&
        metrics.orderbook_snapshot.spread_pct > 0.0028) {
        return false;
    }
    if (metrics.order_book_imbalance < -0.24) {
        return false;
    }
    if (metrics.buy_pressure < (metrics.sell_pressure * 0.80)) {
        return false;
    }
    const double ret3 = rollingReturn(closes, 3);
    const double ret8 = rollingReturn(closes, 8);
    const bool very_thin_liquidity = metrics.liquidity_score < 32.0;
    if (very_thin_liquidity) {
        if (metrics.orderbook_snapshot.valid &&
            metrics.orderbook_snapshot.spread_pct > 0.0022) {
            return false;
        }
        if (metrics.order_book_imbalance < -0.08) {
            return false;
        }
        if (metrics.buy_pressure < (metrics.sell_pressure * 0.95)) {
            return false;
        }
        return (
            current_price <= bb_middle * 1.0018 &&
            current_price >= ema_fast * 0.9960 &&
            rsi >= 32.0 && rsi <= 47.0 &&
            ret3 <= 0.0012 &&
            ret8 >= -0.0018
        );
    }
    return (
        current_price <= bb_middle * 1.0030 &&
        current_price >= ema_fast * 0.9945 &&
        rsi >= 30.0 && rsi <= 50.0 &&
        ret3 <= 0.0018 &&
        ret8 >= -0.0032
    );
}

bool isRangingMinimalProbeOpportunity(
    const analytics::CoinMetrics& metrics,
    const std::vector<double>& closes,
    double current_price,
    double ema_fast,
    double rsi,
    double bb_middle
) {
    if (metrics.liquidity_score < 10.0 || metrics.liquidity_score >= 55.0) {
        return false;
    }
    if (metrics.volume_surge_ratio < 0.14 || metrics.volume_surge_ratio > 2.20) {
        return false;
    }
    if (metrics.orderbook_snapshot.valid &&
        metrics.orderbook_snapshot.spread_pct > 0.0060) {
        return false;
    }
    if (metrics.order_book_imbalance < -0.40) {
        return false;
    }
    if (metrics.buy_pressure < (metrics.sell_pressure * 0.60)) {
        return false;
    }

    const double ret3 = rollingReturn(closes, 3);
    const double ret8 = rollingReturn(closes, 8);
    return (
        current_price <= bb_middle * 1.0080 &&
        current_price >= ema_fast * 0.9860 &&
        rsi >= 22.0 && rsi <= 60.0 &&
        ret3 <= 0.0052 &&
        ret8 >= -0.0105
    );
}

bool isDowntrendLowFlowReboundOpportunity(
    const analytics::CoinMetrics& metrics,
    const std::vector<double>& closes,
    double current_price,
    double ema_fast,
    double ema_slow,
    double rsi
) {
    if (metrics.liquidity_score < 18.0 || metrics.liquidity_score >= 52.0) {
        return false;
    }
    if (metrics.volume_surge_ratio < 0.22 || metrics.volume_surge_ratio > 1.40) {
        return false;
    }
    if (metrics.orderbook_snapshot.valid && metrics.orderbook_snapshot.spread_pct > 0.0032) {
        return false;
    }
    if (metrics.order_book_imbalance < -0.10) {
        return false;
    }
    if (metrics.buy_pressure < (metrics.sell_pressure * 0.90)) {
        return false;
    }

    const double ret3 = rollingReturn(closes, 3);
    const double ret8 = rollingReturn(closes, 8);
    const double ret20 = rollingReturn(closes, 20);
    if (ret3 < -0.0012 || ret8 < -0.0050 || ret20 < -0.0280) {
        return false;
    }

    return (
        current_price >= ema_fast * 0.9970 &&
        ema_fast >= ema_slow * 0.9920 &&
        rsi >= 23.0 && rsi <= 43.0
    );
}

struct EntryGateDecision {
    bool pass = false;
    const char* reject_reason = "foundation_no_signal_entry_gate_unknown";
    bool core_liquidity_pass = false;
    bool low_liquidity_relaxed_path = false;
    bool uptrend_low_flow_probe_path = false;
    bool thin_liquidity_adaptive_path = false;
    bool ultra_thin_liquidity_probe_path = false;
    bool ranging_low_flow_path = false;
    bool downtrend_low_flow_rebound_path = false;
    bool signal_supply_fallback_path = false;
};

EntryGateDecision evaluateEntryGate(
    const analytics::CoinMetrics& metrics,
    const std::vector<double>& closes,
    double current_price,
    double ema_fast,
    double ema_slow,
    double rsi,
    double bb_middle,
    analytics::MarketRegime regime,
    bool enable_signal_supply_fallback
) {
    EntryGateDecision decision;
    const bool base_liquidity_pass = (
        metrics.liquidity_score >= 42.0 &&
        metrics.volume_surge_ratio >= 0.68
    );
    const bool non_hostile_regime =
        regime != analytics::MarketRegime::HIGH_VOLATILITY &&
        regime != analytics::MarketRegime::TRENDING_DOWN;
    const bool spread_guard_ok =
        !metrics.orderbook_snapshot.valid ||
        metrics.orderbook_snapshot.spread_pct <= 0.0042;
    double adaptive_liquidity_floor = 32.0;
    double adaptive_volume_floor = 0.45;
    switch (regime) {
        case analytics::MarketRegime::RANGING:
            adaptive_liquidity_floor = 20.0;
            adaptive_volume_floor = 0.20;
            break;
        case analytics::MarketRegime::TRENDING_UP:
            adaptive_liquidity_floor = 30.0;
            adaptive_volume_floor = 0.34;
            break;
        case analytics::MarketRegime::UNKNOWN:
            adaptive_liquidity_floor = 24.0;
            adaptive_volume_floor = 0.28;
            break;
        case analytics::MarketRegime::TRENDING_DOWN:
        case analytics::MarketRegime::HIGH_VOLATILITY:
        default:
            break;
    }
    const bool adaptive_thin_flow_pass =
        non_hostile_regime &&
        spread_guard_ok &&
        metrics.volatility > 0.0 &&
        metrics.volatility <= 3.6 &&
        metrics.liquidity_score >= adaptive_liquidity_floor &&
        metrics.volume_surge_ratio >= adaptive_volume_floor &&
        metrics.order_book_imbalance > -0.26 &&
        metrics.buy_pressure >= (metrics.sell_pressure * 0.74);
    const bool tight_spread_context =
        metrics.orderbook_snapshot.valid &&
        metrics.orderbook_snapshot.spread_pct > 0.0 &&
        metrics.orderbook_snapshot.spread_pct <= 0.0018;
    const bool low_vol_context =
        metrics.volatility > 0.0 &&
        metrics.volatility <= 2.2;
    const bool narrow_liquidity_relief_context =
        tight_spread_context &&
        low_vol_context &&
        regime != analytics::MarketRegime::HIGH_VOLATILITY &&
        regime != analytics::MarketRegime::TRENDING_DOWN;
    const bool narrow_liquidity_relief_pass =
        narrow_liquidity_relief_context &&
        metrics.liquidity_score >= 34.0 &&
        metrics.volume_surge_ratio >= 0.58 &&
        metrics.order_book_imbalance > -0.14 &&
        metrics.buy_pressure >= (metrics.sell_pressure * 0.85);

    decision.core_liquidity_pass =
        base_liquidity_pass ||
        narrow_liquidity_relief_pass ||
        adaptive_thin_flow_pass;
    decision.low_liquidity_relaxed_path = (
        regime == analytics::MarketRegime::TRENDING_UP &&
        metrics.liquidity_score >= 28.0 &&
        metrics.liquidity_score < 45.0 &&
        metrics.volume_surge_ratio >= 0.46 &&
        metrics.order_book_imbalance > -0.18 &&
        metrics.buy_pressure >= (metrics.sell_pressure * 0.80)
    );
    decision.uptrend_low_flow_probe_path =
        regime == analytics::MarketRegime::TRENDING_UP &&
        isUptrendLowFlowProbeOpportunity(
            metrics,
            closes,
            current_price,
            ema_fast,
            ema_slow,
            rsi
        );
    decision.thin_liquidity_adaptive_path =
        isThinLiquidityAdaptiveOpportunity(
            metrics,
            closes,
            current_price,
            ema_fast,
            ema_slow,
            rsi,
            bb_middle,
            regime
        );
    decision.ultra_thin_liquidity_probe_path =
        isUltraThinLiquidityProbeOpportunity(
            metrics,
            closes,
            current_price,
            ema_fast,
            ema_slow,
            rsi,
            bb_middle,
            regime
        );
    decision.ranging_low_flow_path =
        regime == analytics::MarketRegime::RANGING &&
        isRangingLowFlowOpportunity(
            metrics,
            closes,
            current_price,
            ema_fast,
            rsi,
            bb_middle
        );
    decision.downtrend_low_flow_rebound_path =
        regime == analytics::MarketRegime::TRENDING_DOWN &&
        isDowntrendLowFlowReboundOpportunity(
            metrics,
            closes,
            current_price,
            ema_fast,
            ema_slow,
            rsi
        );
    decision.signal_supply_fallback_path =
        enable_signal_supply_fallback &&
        regime == analytics::MarketRegime::RANGING &&
        (
            isRangingSignalSupplyFallbackOpportunity(
                metrics,
                closes,
                current_price,
                ema_fast,
                rsi,
                bb_middle
            ) ||
            isRangingMinimalProbeOpportunity(
                metrics,
                closes,
                current_price,
                ema_fast,
                rsi,
                bb_middle
            )
        );

    if (!decision.core_liquidity_pass) {
        if (regime == analytics::MarketRegime::TRENDING_UP &&
            isLowLiquidityUptrendOpportunity(
                metrics,
                closes,
                current_price,
                ema_fast,
                ema_slow,
                rsi
            )) {
            decision.pass = true;
            return decision;
        }
        if (decision.uptrend_low_flow_probe_path) {
            decision.pass = true;
            return decision;
        }
        if (decision.ranging_low_flow_path) {
            decision.pass = true;
            return decision;
        }
        if (decision.downtrend_low_flow_rebound_path) {
            decision.pass = true;
            return decision;
        }
        if (decision.signal_supply_fallback_path) {
            decision.pass = true;
            return decision;
        }
        if (decision.thin_liquidity_adaptive_path) {
            decision.pass = true;
            return decision;
        }
        if (decision.ultra_thin_liquidity_probe_path) {
            decision.pass = true;
            return decision;
        }
        decision.reject_reason = "foundation_no_signal_liquidity_volume_gate";
        return decision;
    }

    switch (regime) {
        case analytics::MarketRegime::TRENDING_UP: {
            const double ret3 = rollingReturn(closes, 3);
            const double ret5 = rollingReturn(closes, 5);
            const double ret20 = rollingReturn(closes, 20);
            const double ret20_floor = (metrics.liquidity_score >= 60.0) ? 0.0008 : 0.0004;
            const bool overextended_uptrend = (rsi >= 68.0 && ret5 >= 0.0045);
            if (!(current_price >= ema_fast * 0.9985 &&
                  ema_fast >= ema_slow * 0.9980 &&
                  rsi >= 42.0 && rsi <= 74.0 &&
                  metrics.order_book_imbalance > -0.16 &&
                  metrics.buy_pressure >= (metrics.sell_pressure * 0.86) &&
                  ret5 >= -0.0016 &&
                  ret20 >= (ret20_floor - 0.0008)) ||
                overextended_uptrend) {
                decision.reject_reason = "foundation_no_signal_uptrend_structure";
                return decision;
            }
            const bool low_vol_high_liq_uptrend =
                metrics.liquidity_score >= 62.0 &&
                metrics.volatility > 0.0 &&
                metrics.volatility <= 1.7;
            const double ema_premium = (ema_fast > 1e-9)
                ? ((current_price - ema_fast) / ema_fast)
                : 0.0;
            const bool exhaustion_risk =
                rsi >= 63.0 &&
                (ret3 >= 0.0038 || ret5 >= 0.0055 || ema_premium >= 0.0045);
            if (low_vol_high_liq_uptrend && exhaustion_risk) {
                decision.reject_reason = "foundation_no_signal_uptrend_exhaustion_guard";
                return decision;
            }
            if (metrics.liquidity_score < 55.0 &&
                metrics.volatility > 0.0 &&
                metrics.volatility <= 1.8) {
                if (!(metrics.volume_surge_ratio >= 0.74 &&
                      metrics.order_book_imbalance >= -0.10 &&
                      rsi <= 72.0 &&
                      ret5 >= -0.0006 &&
                      ret20 >= 0.0004)) {
                    decision.reject_reason = "foundation_no_signal_uptrend_thin_context";
                    return decision;
                }
            }
            decision.pass = true;
            return decision;
        }
        case analytics::MarketRegime::RANGING:
            if (!(current_price <= bb_middle * 1.0025 &&
                  rsi <= 46.0 &&
                  metrics.order_book_imbalance > -0.24 &&
                  metrics.buy_pressure >= (metrics.sell_pressure * 0.80))) {
                const bool narrow_ranging_relief =
                    narrow_liquidity_relief_pass &&
                    current_price <= bb_middle * 1.0035 &&
                    rsi <= 47.0 &&
                    metrics.order_book_imbalance > -0.20 &&
                    metrics.buy_pressure >= (metrics.sell_pressure * 0.82);
                if (narrow_ranging_relief) {
                    decision.pass = true;
                    return decision;
                }
                decision.reject_reason = "foundation_no_signal_ranging_structure";
                return decision;
            }
            decision.pass = true;
            return decision;
        case analytics::MarketRegime::TRENDING_DOWN:
        case analytics::MarketRegime::HIGH_VOLATILITY:
            if (!(rsi <= 32.0 &&
                  current_price > ema_fast &&
                  metrics.buy_pressure >= metrics.sell_pressure &&
                  metrics.liquidity_score >= 62.0 &&
                  metrics.volume_surge_ratio >= 1.10)) {
                decision.reject_reason = "foundation_no_signal_bear_rebound_guard";
                return decision;
            }
            decision.pass = true;
            return decision;
        case analytics::MarketRegime::UNKNOWN:
        default:
            if (!(current_price > ema_fast &&
                  rsi < 52.0 &&
                  metrics.order_book_imbalance > -0.10)) {
                decision.reject_reason = "foundation_no_signal_unknown_regime_guard";
                return decision;
            }
            decision.pass = true;
            return decision;
    }
}

struct EntryEvaluationSnapshot {
    bool pass = false;
    const char* reject_reason = "foundation_no_signal_entry_gate_unknown";
    double ema_fast = 0.0;
    double ema_slow = 0.0;
    double rsi = 50.0;
    double bb_middle = 0.0;
    EntryGateDecision entry_gate;
    MtfConfirmation mtf_confirmation;
};

EntryEvaluationSnapshot evaluateEntrySnapshot(
    const analytics::CoinMetrics& metrics,
    const std::vector<Candle>& candles,
    double current_price,
    analytics::MarketRegime regime,
    bool signal_supply_experiment_enabled
) {
    EntryEvaluationSnapshot snapshot;
    const std::vector<double> closes = analytics::TechnicalIndicators::extractClosePrices(candles);
    if (closes.size() < 60) {
        snapshot.reject_reason = "foundation_no_signal_data_window";
        return snapshot;
    }

    snapshot.ema_fast = analytics::TechnicalIndicators::calculateEMA(closes, 12);
    snapshot.ema_slow = analytics::TechnicalIndicators::calculateEMA(closes, 48);
    snapshot.rsi = analytics::TechnicalIndicators::calculateRSI(closes, 14);
    const auto bb = analytics::TechnicalIndicators::calculateBollingerBands(closes, current_price, 20, 2.0);
    snapshot.bb_middle = bb.middle;

    snapshot.entry_gate = evaluateEntryGate(
        metrics,
        closes,
        current_price,
        snapshot.ema_fast,
        snapshot.ema_slow,
        snapshot.rsi,
        snapshot.bb_middle,
        regime,
        signal_supply_experiment_enabled
    );
    if (!snapshot.entry_gate.pass) {
        snapshot.reject_reason = snapshot.entry_gate.reject_reason;
        return snapshot;
    }

    snapshot.mtf_confirmation = evaluateMtfConfirmation(metrics, regime);
    if (!snapshot.mtf_confirmation.pass) {
        snapshot.reject_reason = snapshot.mtf_confirmation.reject_reason;
        return snapshot;
    }

    snapshot.pass = true;
    return snapshot;
}

} // namespace

FoundationAdaptiveStrategy::FoundationAdaptiveStrategy(std::shared_ptr<network::UpbitHttpClient> client)
    : client_(std::move(client)) {
    (void)client_;
}

StrategyInfo FoundationAdaptiveStrategy::getInfo() const {
    StrategyInfo info;
    info.name = "Foundation Adaptive Strategy";
    info.description = "Regime-adaptive baseline strategy with dynamic risk sizing.";
    info.timeframe = "1m+MTF";
    info.min_capital = 30000.0;
    info.expected_winrate = 0.52;
    info.risk_level = 4.5;
    return info;
}

Signal FoundationAdaptiveStrategy::generateSignal(
    const std::string& market,
    const analytics::CoinMetrics& metrics,
    const std::vector<Candle>& candles,
    double current_price,
    double available_capital,
    const analytics::RegimeAnalysis& regime
) {
    std::lock_guard<std::mutex> lock(mutex_);

    Signal signal;
    signal.type = SignalType::NONE;
    signal.market = market;
    signal.strategy_name = getInfo().name;
    signal.timestamp = nowMs();
    signal.market_regime = regime.regime;

    if (!enabled_ || available_capital <= 0.0 || current_price <= 0.0) {
        signal.reason = "foundation_no_signal_runtime_guard";
        return signal;
    }
    if (candles.size() < 80) {
        signal.reason = "foundation_no_signal_data_window";
        return signal;
    }

    const bool signal_supply_experiment_enabled = isSignalSupplyFallbackEnabled();
    const EntryEvaluationSnapshot entry_snapshot = evaluateEntrySnapshot(
        metrics,
        candles,
        current_price,
        regime.regime,
        signal_supply_experiment_enabled
    );
    if (!entry_snapshot.pass) {
        signal.reason = entry_snapshot.reject_reason;
        return signal;
    }
    const EntryGateDecision& entry_gate = entry_snapshot.entry_gate;
    const MtfConfirmation& mtf_confirmation = entry_snapshot.mtf_confirmation;
    const double ema_fast = entry_snapshot.ema_fast;
    const double ema_slow = entry_snapshot.ema_slow;
    const double rsi = entry_snapshot.rsi;

    const double atr = safeAtr(candles, current_price);
    const double atr_pct = atr / std::max(1e-9, current_price);

    const bool low_liquidity_relaxed_path = entry_gate.low_liquidity_relaxed_path;
    const bool uptrend_low_flow_probe_path = entry_gate.uptrend_low_flow_probe_path;
    const bool thin_liquidity_adaptive_path = entry_gate.thin_liquidity_adaptive_path;
    const bool ultra_thin_liquidity_probe_path = entry_gate.ultra_thin_liquidity_probe_path;
    const bool ranging_low_flow_path = entry_gate.ranging_low_flow_path;
    const bool downtrend_low_flow_rebound_path = entry_gate.downtrend_low_flow_rebound_path;
    const bool signal_supply_fallback_path = entry_gate.signal_supply_fallback_path;
    const bool thin_low_vol_context =
        (metrics.liquidity_score < 55.0 &&
         metrics.volatility > 0.0 &&
         metrics.volatility <= 1.8);
    const bool thin_low_vol_uptrend_context =
        (regime.regime == analytics::MarketRegime::TRENDING_UP && thin_low_vol_context);
    const bool hostile_uptrend_context =
        (regime.regime == analytics::MarketRegime::TRENDING_UP &&
         metrics.liquidity_score < 50.0 &&
         metrics.volatility > 0.0 &&
         metrics.volatility <= 1.9);

    double risk_pct = clampRiskPctByRegime(regime.regime, atr_pct);
    double reward_risk = targetRewardRiskByRegime(regime.regime);
    if (thin_liquidity_adaptive_path) {
        risk_pct = std::clamp(risk_pct * 0.74, 0.0024, 0.0088);
        reward_risk = std::max(reward_risk, 2.10);
    } else if (uptrend_low_flow_probe_path) {
        risk_pct = std::clamp(risk_pct * 0.68, 0.0022, 0.0078);
        reward_risk = std::max(reward_risk, 2.05);
    } else if (ultra_thin_liquidity_probe_path) {
        risk_pct = std::clamp(risk_pct * 0.66, 0.0022, 0.0075);
        reward_risk = std::max(reward_risk, 2.35);
    } else if (downtrend_low_flow_rebound_path) {
        risk_pct = std::clamp(risk_pct * 0.58, 0.0020, 0.0068);
        reward_risk = std::max(reward_risk, 2.00);
    } else if (low_liquidity_relaxed_path) {
        risk_pct = std::clamp(risk_pct * 0.84, 0.0028, 0.0105);
        reward_risk = std::max(reward_risk, 2.25);
    }
    if (ranging_low_flow_path) {
        risk_pct = std::clamp(risk_pct * 0.78, 0.0024, 0.0088);
        reward_risk = std::max(reward_risk, 1.80);
    }
    if (signal_supply_fallback_path) {
        risk_pct = std::clamp(risk_pct * 0.72, 0.0022, 0.0085);
        reward_risk = std::max(reward_risk, 1.95);
    }
    if (thin_low_vol_context) {
        risk_pct = std::clamp(risk_pct * 0.88, 0.0025, 0.0098);
        if (regime.regime == analytics::MarketRegime::TRENDING_UP) {
            reward_risk = std::clamp(reward_risk, 1.35, 1.75);
        } else {
            reward_risk = std::clamp(reward_risk, 1.45, 1.90);
        }
    }
    if (hostile_uptrend_context) {
        risk_pct = std::clamp(risk_pct * 0.92, 0.0023, 0.0085);
        reward_risk = std::clamp(reward_risk, 1.30, 1.65);
    }
    const double mtf_centered = mtf_confirmation.score - 0.50;
    risk_pct *= std::clamp(1.0 - (mtf_centered * 0.40), 0.84, 1.12);
    reward_risk += std::clamp(mtf_centered * 0.90, -0.12, 0.18);
    risk_pct = std::clamp(risk_pct, 0.0022, 0.0185);
    reward_risk = std::clamp(reward_risk, 1.10, 2.50);

    const double ema_gap = (ema_slow > 1e-9) ? ((ema_fast - ema_slow) / ema_slow) : 0.0;
    double strength = 0.50;
    strength += std::clamp(ema_gap * 12.0, -0.10, 0.20);
    strength += std::clamp((55.0 - rsi) / 120.0, -0.08, 0.10);
    strength += std::clamp((metrics.liquidity_score - 50.0) / 350.0, -0.07, 0.12);
    if (regime.regime == analytics::MarketRegime::TRENDING_UP) {
        strength += 0.08;
    }
    if (regime.regime == analytics::MarketRegime::TRENDING_DOWN ||
        regime.regime == analytics::MarketRegime::HIGH_VOLATILITY) {
        strength -= 0.06;
    }
    if (thin_liquidity_adaptive_path) {
        strength += 0.03;
    } else if (uptrend_low_flow_probe_path) {
        strength += 0.01;
    } else if (ultra_thin_liquidity_probe_path) {
        strength -= 0.02;
    } else if (downtrend_low_flow_rebound_path) {
        strength += 0.00;
    } else if (ranging_low_flow_path) {
        strength += 0.02;
    } else if (low_liquidity_relaxed_path) {
        strength += 0.04;
    }
    if (hostile_uptrend_context) {
        strength -= 0.03;
    }
    strength += std::clamp(mtf_centered * 0.30, -0.10, 0.12);
    strength = std::clamp(strength, 0.35, 0.92);

    signal.type = (regime.regime == analytics::MarketRegime::TRENDING_UP && strength >= 0.74)
        ? SignalType::STRONG_BUY
        : SignalType::BUY;
    signal.strength = strength;
    signal.entry_price = current_price;
    signal.stop_loss = current_price * (1.0 - risk_pct);
    signal.take_profit_1 = current_price * (1.0 + risk_pct * std::max(1.0, reward_risk * 0.55));
    signal.take_profit_2 = current_price * (1.0 + risk_pct * reward_risk);
    signal.breakeven_trigger = current_price * (1.0 + risk_pct * 0.80);
    signal.trailing_start = current_price * (1.0 + risk_pct * 1.20);
    signal.position_size = calculatePositionSize(
        available_capital,
        signal.entry_price,
        signal.stop_loss,
        metrics
    );
    if (ultra_thin_liquidity_probe_path) {
        signal.position_size *= 0.40;
    } else if (uptrend_low_flow_probe_path) {
        signal.position_size *= 0.42;
    } else if (thin_liquidity_adaptive_path) {
        signal.position_size *= 0.48;
    } else if (downtrend_low_flow_rebound_path) {
        signal.position_size *= 0.32;
    } else if (ranging_low_flow_path) {
        signal.position_size *= 0.50;
    } else if (signal_supply_fallback_path) {
        signal.position_size *= 0.46;
    } else if (thin_low_vol_uptrend_context) {
        signal.position_size *= 0.55;
    } else if (thin_low_vol_context) {
        signal.position_size *= 0.62;
    }

    if (signal.position_size <= 0.0) {
        signal.type = SignalType::NONE;
        signal.reason = "foundation_no_signal_position_sizing";
        return signal;
    }

    signal.expected_return_pct = (signal.take_profit_2 - signal.entry_price) / signal.entry_price;
    signal.expected_risk_pct = (signal.entry_price - signal.stop_loss) / signal.entry_price;
    double implied_win_prob = std::clamp(0.47 + (strength * 0.18), 0.44, 0.66);
    if (hostile_uptrend_context) {
        implied_win_prob = std::clamp(implied_win_prob - 0.08, 0.36, 0.58);
    }
    implied_win_prob += std::clamp(mtf_centered * 0.22, -0.05, 0.06);
    implied_win_prob = std::clamp(implied_win_prob, 0.34, 0.72);
    signal.expected_value =
        (implied_win_prob * signal.expected_return_pct) -
        ((1.0 - implied_win_prob) * signal.expected_risk_pct);

    signal.signal_filter = std::clamp(0.52 + (strength * 0.30), 0.55, 0.85);
    if (hostile_uptrend_context) {
        signal.signal_filter = std::clamp(signal.signal_filter + 0.03, 0.55, 0.88);
    }
    signal.buy_order_type = OrderTypePolicy::LIMIT_WITH_FALLBACK;
    signal.sell_order_type = OrderTypePolicy::LIMIT_WITH_FALLBACK;
    signal.max_retries = 2;
    signal.retry_wait_ms = 700;
    if (ultra_thin_liquidity_probe_path) {
        signal.reason = "foundation_adaptive_regime_entry_ultra_thin_probe";
    } else if (uptrend_low_flow_probe_path) {
        signal.reason = "foundation_adaptive_regime_entry_uptrend_low_flow_probe";
    } else if (thin_liquidity_adaptive_path) {
        signal.reason = "foundation_adaptive_regime_entry_thin_liq_adaptive";
    } else if (downtrend_low_flow_rebound_path) {
        signal.reason = "foundation_adaptive_regime_entry_downtrend_low_flow_rebound";
    } else if (ranging_low_flow_path) {
        signal.reason = "foundation_adaptive_regime_entry_ranging_low_flow";
    } else if (signal_supply_fallback_path) {
        signal.reason = "foundation_adaptive_regime_entry_signal_supply_fallback";
    } else if (thin_low_vol_uptrend_context) {
        signal.reason = "foundation_adaptive_regime_entry_thin_lowvol_guarded";
    } else if (low_liquidity_relaxed_path) {
        signal.reason = "foundation_adaptive_regime_entry_low_liq_uptrend";
    } else {
        signal.reason = "foundation_adaptive_regime_entry";
    }
    signal.entry_archetype = archetypeByRegime(regime.regime);

    return signal;
}

bool FoundationAdaptiveStrategy::shouldEnter(
    const std::string& market,
    const analytics::CoinMetrics& metrics,
    const std::vector<Candle>& candles,
    double current_price,
    const analytics::RegimeAnalysis& regime
) {
    (void)market;
    if (!enabled_ || current_price <= 0.0 || candles.size() < 80) {
        return false;
    }
    const bool signal_supply_experiment_enabled = isSignalSupplyFallbackEnabled();
    const EntryEvaluationSnapshot entry_snapshot = evaluateEntrySnapshot(
        metrics,
        candles,
        current_price,
        regime.regime,
        signal_supply_experiment_enabled
    );
    return entry_snapshot.pass;
}

bool FoundationAdaptiveStrategy::shouldExit(
    const std::string& market,
    double entry_price,
    double current_price,
    double holding_time_seconds
) {
    (void)market;
    if (entry_price <= 0.0 || current_price <= 0.0) {
        return false;
    }
    if (current_price <= entry_price * 0.994) {
        return true;
    }
    if (current_price >= entry_price * 1.018) {
        return true;
    }
    if (holding_time_seconds >= 6.0 * 3600.0 && current_price <= entry_price * 1.001) {
        return true;
    }
    return false;
}

double FoundationAdaptiveStrategy::calculateStopLoss(
    double entry_price,
    const std::vector<Candle>& candles
) {
    if (entry_price <= 0.0) {
        return 0.0;
    }
    const double atr = safeAtr(candles, entry_price);
    const double atr_pct = atr / std::max(1e-9, entry_price);
    const double risk_pct = std::clamp(std::max(atr_pct * 1.20, 0.004), 0.0035, 0.015);
    return entry_price * (1.0 - risk_pct);
}

double FoundationAdaptiveStrategy::calculateTakeProfit(
    double entry_price,
    const std::vector<Candle>& candles
) {
    if (entry_price <= 0.0) {
        return 0.0;
    }
    const double stop_loss = calculateStopLoss(entry_price, candles);
    const double risk = std::max(0.0, entry_price - stop_loss);
    return entry_price + (risk * 1.55);
}

double FoundationAdaptiveStrategy::calculatePositionSize(
    double capital,
    double entry_price,
    double stop_loss,
    const analytics::CoinMetrics& metrics
) {
    if (capital <= 0.0 || entry_price <= 0.0 || stop_loss <= 0.0) {
        return 0.0;
    }

    const double stop_distance = std::max(entry_price - stop_loss, entry_price * 0.0025);
    const double risk_budget_krw = capital * 0.0065;
    const double risk_limited_notional = (risk_budget_krw * entry_price) / std::max(1e-9, stop_distance);

    double max_notional = capital * 0.22;
    if (metrics.liquidity_score < 50.0) {
        max_notional *= 0.60;
    } else if (metrics.liquidity_score < 55.0) {
        max_notional *= 0.75;
    }
    if (metrics.volatility > 4.0) {
        max_notional *= 0.85;
    }
    if (metrics.orderbook_snapshot.valid && metrics.orderbook_snapshot.spread_pct > 0.0025) {
        max_notional *= 0.80;
    }
    const double min_notional = capital * ((metrics.liquidity_score < 55.0) ? 0.02 : 0.03);
    const double notional = std::clamp(risk_limited_notional, min_notional, max_notional);
    return std::clamp(notional / capital, 0.02, 0.22);
}

void FoundationAdaptiveStrategy::setEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    enabled_ = enabled;
}

bool FoundationAdaptiveStrategy::isEnabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return enabled_;
}

IStrategy::Statistics FoundationAdaptiveStrategy::getStatistics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

void FoundationAdaptiveStrategy::updateStatistics(const std::string& market, bool is_win, double profit_loss) {
    (void)market;
    std::lock_guard<std::mutex> lock(mutex_);

    stats_.total_signals++;
    if (is_win) {
        stats_.winning_trades++;
        stats_.total_profit += profit_loss;
    } else {
        stats_.losing_trades++;
        stats_.total_loss += std::abs(profit_loss);
    }

    const int closed = stats_.winning_trades + stats_.losing_trades;
    if (closed > 0) {
        stats_.win_rate = static_cast<double>(stats_.winning_trades) / static_cast<double>(closed);
        stats_.avg_profit = (stats_.winning_trades > 0)
            ? (stats_.total_profit / static_cast<double>(stats_.winning_trades))
            : 0.0;
        stats_.avg_loss = (stats_.losing_trades > 0)
            ? (stats_.total_loss / static_cast<double>(stats_.losing_trades))
            : 0.0;
    } else {
        stats_.win_rate = 0.0;
        stats_.avg_profit = 0.0;
        stats_.avg_loss = 0.0;
    }

    if (stats_.total_loss > 1e-9) {
        stats_.profit_factor = stats_.total_profit / stats_.total_loss;
    } else {
        stats_.profit_factor = (stats_.total_profit > 0.0) ? 999.0 : 0.0;
    }

    stats_.sharpe_ratio = (stats_.avg_loss > 1e-9) ? (stats_.avg_profit / stats_.avg_loss) : 0.0;
}

void FoundationAdaptiveStrategy::setStatistics(const Statistics& stats) {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_ = stats;
}

long long FoundationAdaptiveStrategy::nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

double FoundationAdaptiveStrategy::clampRiskPctByRegime(
    analytics::MarketRegime regime,
    double atr_pct
) {
    switch (regime) {
        case analytics::MarketRegime::TRENDING_UP:
            return std::clamp(std::max(atr_pct * 1.35, 0.0040), 0.0040, 0.0180);
        case analytics::MarketRegime::RANGING:
            return std::clamp(std::max(atr_pct * 1.15, 0.0035), 0.0035, 0.0140);
        case analytics::MarketRegime::TRENDING_DOWN:
        case analytics::MarketRegime::HIGH_VOLATILITY:
            return std::clamp(std::max(atr_pct * 0.90, 0.0030), 0.0030, 0.0100);
        case analytics::MarketRegime::UNKNOWN:
        default:
            return std::clamp(std::max(atr_pct * 1.00, 0.0038), 0.0038, 0.0140);
    }
}

double FoundationAdaptiveStrategy::targetRewardRiskByRegime(analytics::MarketRegime regime) {
    switch (regime) {
        case analytics::MarketRegime::TRENDING_UP:
            return 1.90;
        case analytics::MarketRegime::RANGING:
            return 1.45;
        case analytics::MarketRegime::TRENDING_DOWN:
        case analytics::MarketRegime::HIGH_VOLATILITY:
            return 1.15;
        case analytics::MarketRegime::UNKNOWN:
        default:
            return 1.35;
    }
}

std::string FoundationAdaptiveStrategy::archetypeByRegime(analytics::MarketRegime regime) {
    switch (regime) {
        case analytics::MarketRegime::TRENDING_UP:
            return "FOUNDATION_UPTREND_CONTINUATION";
        case analytics::MarketRegime::RANGING:
            return "FOUNDATION_RANGE_PULLBACK";
        case analytics::MarketRegime::TRENDING_DOWN:
            return "FOUNDATION_DOWNTREND_BOUNCE";
        case analytics::MarketRegime::HIGH_VOLATILITY:
            return "FOUNDATION_HIGH_VOL_GUARDED";
        case analytics::MarketRegime::UNKNOWN:
        default:
            return "FOUNDATION_UNKNOWN_GUARDED";
    }
}

} // namespace strategy
} // namespace autolife

