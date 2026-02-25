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
    const auto& engine_cfg = autolife::Config::getInstance().getEngineConfig();

    const auto closes_5m = extractTfCloses(metrics, "5m");
    const auto closes_15m = extractTfCloses(metrics, "15m");
    const auto closes_1h = extractTfCloses(metrics, "1h");
    const std::size_t min_bars_5m =
        static_cast<std::size_t>(std::max(1, engine_cfg.foundation_mtf_min_bars_5m));
    const std::size_t min_bars_15m =
        static_cast<std::size_t>(std::max(1, engine_cfg.foundation_mtf_min_bars_15m));
    const std::size_t min_bars_1h =
        static_cast<std::size_t>(std::max(1, engine_cfg.foundation_mtf_min_bars_1h));
    if (closes_5m.size() < min_bars_5m || closes_15m.size() < min_bars_15m || closes_1h.size() < min_bars_1h) {
        out.reject_reason = "foundation_no_signal_mtf_window";
        return out;
    }

    const std::size_t momentum_5m_lookback =
        static_cast<std::size_t>(std::max(1, engine_cfg.foundation_mtf_momentum_5m_lookback));
    const std::size_t momentum_15m_lookback =
        static_cast<std::size_t>(std::max(1, engine_cfg.foundation_mtf_momentum_15m_lookback));
    const std::size_t momentum_1h_lookback =
        static_cast<std::size_t>(std::max(1, engine_cfg.foundation_mtf_momentum_1h_lookback));
    const int ema_fast_15m = std::max(1, engine_cfg.foundation_mtf_ema_fast_15m);
    const int ema_slow_15m = std::max(ema_fast_15m + 1, engine_cfg.foundation_mtf_ema_slow_15m);
    const int ema_fast_1h = std::max(1, engine_cfg.foundation_mtf_ema_fast_1h);
    const int ema_slow_1h = std::max(ema_fast_1h + 1, engine_cfg.foundation_mtf_ema_slow_1h);
    const int rsi_period = std::max(2, engine_cfg.foundation_mtf_rsi_period);

    out.momentum_5m = rollingReturn(closes_5m, momentum_5m_lookback);
    out.momentum_15m = rollingReturn(closes_15m, momentum_15m_lookback);
    out.momentum_1h = rollingReturn(closes_1h, momentum_1h_lookback);
    out.ema_gap_15m = safeEmaGap(closes_15m, ema_fast_15m, ema_slow_15m);
    out.ema_gap_1h = safeEmaGap(closes_1h, ema_fast_1h, ema_slow_1h);
    out.rsi_5m = analytics::TechnicalIndicators::calculateRSI(closes_5m, rsi_period);
    out.rsi_15m = analytics::TechnicalIndicators::calculateRSI(closes_15m, rsi_period);

    const double score_momentum_5m_clip = std::max(0.0, engine_cfg.foundation_mtf_score_momentum_5m_clip);
    const double score_momentum_15m_clip = std::max(0.0, engine_cfg.foundation_mtf_score_momentum_15m_clip);
    const double score_momentum_1h_clip = std::max(0.0, engine_cfg.foundation_mtf_score_momentum_1h_clip);
    const double score_ema_gap_15m_clip = std::max(0.0, engine_cfg.foundation_mtf_score_ema_gap_15m_clip);
    const double score_ema_gap_1h_clip = std::max(0.0, engine_cfg.foundation_mtf_score_ema_gap_1h_clip);
    const double score_rsi_15m_clip = std::max(0.0, engine_cfg.foundation_mtf_score_rsi_15m_clip);
    const double score_rsi_5m_clip = std::max(0.0, engine_cfg.foundation_mtf_score_rsi_5m_clip);
    const double score_rsi_15m_divisor =
        std::max(1e-9, std::abs(engine_cfg.foundation_mtf_score_rsi_15m_divisor));
    const double score_rsi_5m_divisor =
        std::max(1e-9, std::abs(engine_cfg.foundation_mtf_score_rsi_5m_divisor));

    double score = engine_cfg.foundation_mtf_score_base;
    score += std::clamp(
        out.momentum_5m * engine_cfg.foundation_mtf_score_momentum_5m_weight,
        -score_momentum_5m_clip,
        score_momentum_5m_clip
    );
    score += std::clamp(
        out.momentum_15m * engine_cfg.foundation_mtf_score_momentum_15m_weight,
        -score_momentum_15m_clip,
        score_momentum_15m_clip
    );
    score += std::clamp(
        out.momentum_1h * engine_cfg.foundation_mtf_score_momentum_1h_weight,
        -score_momentum_1h_clip,
        score_momentum_1h_clip
    );
    score += std::clamp(
        out.ema_gap_15m * engine_cfg.foundation_mtf_score_ema_gap_15m_weight,
        -score_ema_gap_15m_clip,
        score_ema_gap_15m_clip
    );
    score += std::clamp(
        out.ema_gap_1h * engine_cfg.foundation_mtf_score_ema_gap_1h_weight,
        -score_ema_gap_1h_clip,
        score_ema_gap_1h_clip
    );
    score += std::clamp(
        (engine_cfg.foundation_mtf_score_rsi_15m_center - out.rsi_15m) / score_rsi_15m_divisor,
        -score_rsi_15m_clip,
        score_rsi_15m_clip
    );
    score += std::clamp(
        (engine_cfg.foundation_mtf_score_rsi_5m_center - out.rsi_5m) / score_rsi_5m_divisor,
        -score_rsi_5m_clip,
        score_rsi_5m_clip
    );
    out.score = std::clamp(score, 0.0, 1.0);

    switch (regime) {
        case analytics::MarketRegime::TRENDING_UP:
            out.pass =
                out.momentum_15m >= engine_cfg.foundation_mtf_uptrend_min_momentum_15m &&
                out.ema_gap_15m >= engine_cfg.foundation_mtf_uptrend_min_ema_gap_15m &&
                out.momentum_1h >= engine_cfg.foundation_mtf_uptrend_min_momentum_1h &&
                out.rsi_15m <= engine_cfg.foundation_mtf_uptrend_max_rsi_15m &&
                out.score >= engine_cfg.foundation_mtf_uptrend_min_score;
            out.reject_reason = "foundation_no_signal_mtf_uptrend_mismatch";
            break;
        case analytics::MarketRegime::RANGING:
            out.pass =
                std::abs(out.momentum_15m) <=
                    std::max(0.0, engine_cfg.foundation_mtf_ranging_max_abs_momentum_15m) &&
                out.rsi_15m <= engine_cfg.foundation_mtf_ranging_max_rsi_15m &&
                out.score >= engine_cfg.foundation_mtf_ranging_min_score;
            out.reject_reason = "foundation_no_signal_mtf_ranging_mismatch";
            break;
        case analytics::MarketRegime::TRENDING_DOWN:
        case analytics::MarketRegime::HIGH_VOLATILITY:
            out.pass =
                out.momentum_1h >= engine_cfg.foundation_mtf_hostile_min_momentum_1h &&
                out.momentum_15m >= engine_cfg.foundation_mtf_hostile_min_momentum_15m &&
                out.rsi_5m <= engine_cfg.foundation_mtf_hostile_max_rsi_5m &&
                out.score >= engine_cfg.foundation_mtf_hostile_min_score;
            out.reject_reason = "foundation_no_signal_mtf_hostile_mismatch";
            break;
        case analytics::MarketRegime::UNKNOWN:
        default:
            out.pass = out.score >= engine_cfg.foundation_mtf_unknown_min_score;
            out.reject_reason = "foundation_no_signal_mtf_unknown_mismatch";
            break;
    }
    return out;
}

bool isLowLiquidityUptrendOpportunity(
    const engine::EngineConfig& cfg,
    const analytics::CoinMetrics& metrics,
    const std::vector<double>& closes,
    double current_price,
    double ema_fast,
    double ema_slow,
    double rsi
) {
    if (!cfg.foundation_entry_low_liq_uptrend_enabled) {
        return false;
    }
    if (metrics.liquidity_score < cfg.foundation_entry_low_liq_uptrend_liquidity_min ||
        metrics.liquidity_score >= cfg.foundation_entry_low_liq_uptrend_liquidity_max) {
        return false;
    }
    if (metrics.volume_surge_ratio < cfg.foundation_entry_low_liq_uptrend_volume_surge_min ||
        metrics.volume_surge_ratio > cfg.foundation_entry_low_liq_uptrend_volume_surge_max) {
        return false;
    }
    if (metrics.order_book_imbalance <= cfg.foundation_entry_low_liq_uptrend_imbalance_min) {
        return false;
    }

    const double ret5 = rollingReturn(closes, 5);
    const double ret20 = rollingReturn(closes, 20);
    if (ret5 < cfg.foundation_entry_low_liq_uptrend_ret5_min ||
        ret20 < cfg.foundation_entry_low_liq_uptrend_ret20_min) {
        return false;
    }

    return (
        current_price > ema_fast * cfg.foundation_entry_low_liq_uptrend_price_to_ema_fast_min &&
        ema_fast >= ema_slow * cfg.foundation_entry_low_liq_uptrend_ema_fast_to_ema_slow_min &&
        rsi >= cfg.foundation_entry_low_liq_uptrend_rsi_min &&
        rsi <= cfg.foundation_entry_low_liq_uptrend_rsi_max
    );
}

bool isThinLiquidityAdaptiveOpportunity(
    const engine::EngineConfig& cfg,
    const analytics::CoinMetrics& metrics,
    const std::vector<double>& closes,
    double current_price,
    double ema_fast,
    double ema_slow,
    double rsi,
    double bb_middle,
    analytics::MarketRegime regime
) {
    if (!cfg.foundation_entry_thin_liq_adaptive_enabled) {
        return false;
    }
    if (metrics.liquidity_score < cfg.foundation_entry_thin_liq_adaptive_liquidity_min ||
        metrics.liquidity_score >= cfg.foundation_entry_thin_liq_adaptive_liquidity_max) {
        return false;
    }
    if (metrics.volume_surge_ratio < cfg.foundation_entry_thin_liq_adaptive_volume_surge_min ||
        metrics.volume_surge_ratio > cfg.foundation_entry_thin_liq_adaptive_volume_surge_max) {
        return false;
    }
    if (metrics.orderbook_snapshot.valid &&
        metrics.orderbook_snapshot.spread_pct > cfg.foundation_entry_thin_liq_adaptive_spread_max) {
        return false;
    }

    const double ret3 = rollingReturn(closes, 3);
    const double ret8 = rollingReturn(closes, 8);
    const double ret20 = rollingReturn(closes, 20);

    switch (regime) {
        case analytics::MarketRegime::TRENDING_UP:
            return (
                current_price >=
                    ema_fast * cfg.foundation_entry_thin_liq_adaptive_uptrend_price_to_ema_fast_min &&
                ema_fast >=
                    ema_slow * cfg.foundation_entry_thin_liq_adaptive_uptrend_ema_fast_to_ema_slow_min &&
                rsi >= cfg.foundation_entry_thin_liq_adaptive_uptrend_rsi_min &&
                rsi <= cfg.foundation_entry_thin_liq_adaptive_uptrend_rsi_max &&
                ret8 >= cfg.foundation_entry_thin_liq_adaptive_uptrend_ret8_min &&
                ret20 >= cfg.foundation_entry_thin_liq_adaptive_uptrend_ret20_min &&
                metrics.order_book_imbalance > cfg.foundation_entry_thin_liq_adaptive_uptrend_imbalance_min
            );
        case analytics::MarketRegime::RANGING:
            return (
                current_price <=
                    bb_middle * cfg.foundation_entry_thin_liq_adaptive_ranging_price_to_bb_middle_max &&
                rsi >= cfg.foundation_entry_thin_liq_adaptive_ranging_rsi_min &&
                rsi <= cfg.foundation_entry_thin_liq_adaptive_ranging_rsi_max &&
                ret3 <= cfg.foundation_entry_thin_liq_adaptive_ranging_ret3_max &&
                ret20 >= cfg.foundation_entry_thin_liq_adaptive_ranging_ret20_min &&
                metrics.order_book_imbalance > cfg.foundation_entry_thin_liq_adaptive_ranging_imbalance_min
            );
        case analytics::MarketRegime::TRENDING_DOWN:
        case analytics::MarketRegime::HIGH_VOLATILITY:
            return (
                rsi <= cfg.foundation_entry_thin_liq_adaptive_hostile_rsi_max &&
                current_price >=
                    ema_fast * cfg.foundation_entry_thin_liq_adaptive_hostile_price_to_ema_fast_min &&
                metrics.buy_pressure >=
                    (metrics.sell_pressure *
                     cfg.foundation_entry_thin_liq_adaptive_hostile_buy_pressure_ratio_min) &&
                ret3 >= cfg.foundation_entry_thin_liq_adaptive_hostile_ret3_min &&
                ret8 >= cfg.foundation_entry_thin_liq_adaptive_hostile_ret8_min
            );
        case analytics::MarketRegime::UNKNOWN:
        default:
            return (
                current_price >=
                    ema_fast * cfg.foundation_entry_thin_liq_adaptive_unknown_price_to_ema_fast_min &&
                rsi <= cfg.foundation_entry_thin_liq_adaptive_unknown_rsi_max &&
                metrics.order_book_imbalance > cfg.foundation_entry_thin_liq_adaptive_unknown_imbalance_min &&
                ret8 >= cfg.foundation_entry_thin_liq_adaptive_unknown_ret8_min
            );
    }
}

bool isRangingLowFlowOpportunity(
    const engine::EngineConfig& cfg,
    const analytics::CoinMetrics& metrics,
    const std::vector<double>& closes,
    double current_price,
    double ema_fast,
    double rsi,
    double bb_middle
) {
    if (!cfg.foundation_entry_ranging_low_flow_enabled) {
        return false;
    }
    if (metrics.liquidity_score < cfg.foundation_entry_ranging_low_flow_liquidity_min ||
        metrics.liquidity_score >= cfg.foundation_entry_ranging_low_flow_liquidity_max) {
        return false;
    }
    if (metrics.volume_surge_ratio < cfg.foundation_entry_ranging_low_flow_volume_surge_min ||
        metrics.volume_surge_ratio > cfg.foundation_entry_ranging_low_flow_volume_surge_max) {
        return false;
    }
    if (metrics.orderbook_snapshot.valid &&
        metrics.orderbook_snapshot.spread_pct > cfg.foundation_entry_ranging_low_flow_spread_max) {
        return false;
    }

    const double ret3 = rollingReturn(closes, 3);
    const double ret8 = rollingReturn(closes, 8);
    return (
        current_price <= bb_middle * cfg.foundation_entry_ranging_low_flow_price_to_bb_middle_max &&
        current_price >= ema_fast * cfg.foundation_entry_ranging_low_flow_price_to_ema_fast_min &&
        rsi >= cfg.foundation_entry_ranging_low_flow_rsi_min &&
        rsi <= cfg.foundation_entry_ranging_low_flow_rsi_max &&
        ret3 <= cfg.foundation_entry_ranging_low_flow_ret3_max &&
        ret8 >= cfg.foundation_entry_ranging_low_flow_ret8_min &&
        metrics.order_book_imbalance > cfg.foundation_entry_ranging_low_flow_imbalance_min &&
        metrics.buy_pressure >=
            (metrics.sell_pressure * cfg.foundation_entry_ranging_low_flow_buy_pressure_ratio_min)
    );
}

bool isDowntrendLowFlowReboundOpportunity(
    const engine::EngineConfig& cfg,
    const analytics::CoinMetrics& metrics,
    const std::vector<double>& closes,
    double current_price,
    double ema_fast,
    double ema_slow,
    double rsi
) {
    if (!cfg.foundation_entry_downtrend_rebound_enabled) {
        return false;
    }
    if (metrics.liquidity_score < cfg.foundation_entry_downtrend_rebound_liquidity_min ||
        metrics.liquidity_score >= cfg.foundation_entry_downtrend_rebound_liquidity_max) {
        return false;
    }
    if (metrics.volume_surge_ratio < cfg.foundation_entry_downtrend_rebound_volume_surge_min ||
        metrics.volume_surge_ratio > cfg.foundation_entry_downtrend_rebound_volume_surge_max) {
        return false;
    }
    if (metrics.orderbook_snapshot.valid &&
        metrics.orderbook_snapshot.spread_pct > cfg.foundation_entry_downtrend_rebound_spread_max) {
        return false;
    }
    if (metrics.order_book_imbalance < cfg.foundation_entry_downtrend_rebound_imbalance_min) {
        return false;
    }
    if (metrics.buy_pressure <
        (metrics.sell_pressure * cfg.foundation_entry_downtrend_rebound_buy_pressure_ratio_min)) {
        return false;
    }

    const double ret3 = rollingReturn(closes, 3);
    const double ret8 = rollingReturn(closes, 8);
    const double ret20 = rollingReturn(closes, 20);
    if (ret3 < cfg.foundation_entry_downtrend_rebound_ret3_min ||
        ret8 < cfg.foundation_entry_downtrend_rebound_ret8_min ||
        ret20 < cfg.foundation_entry_downtrend_rebound_ret20_min) {
        return false;
    }

    return (
        current_price >= ema_fast * cfg.foundation_entry_downtrend_rebound_price_to_ema_fast_min &&
        ema_fast >= ema_slow * cfg.foundation_entry_downtrend_rebound_ema_fast_to_ema_slow_min &&
        rsi >= cfg.foundation_entry_downtrend_rebound_rsi_min &&
        rsi <= cfg.foundation_entry_downtrend_rebound_rsi_max
    );
}

struct EntryGateDecision {
    bool pass = false;
    const char* reject_reason = "foundation_no_signal_entry_gate_unknown";
    bool low_liquidity_relaxed_path = false;
    bool thin_liquidity_adaptive_path = false;
    bool ranging_low_flow_path = false;
    bool downtrend_low_flow_rebound_path = false;
};

EntryGateDecision evaluateEntryGate(
    const analytics::CoinMetrics& metrics,
    const std::vector<double>& closes,
    double current_price,
    double ema_fast,
    double ema_slow,
    double rsi,
    double bb_middle,
    analytics::MarketRegime regime
) {
    EntryGateDecision decision;
    const auto& engine_cfg = autolife::Config::getInstance().getEngineConfig();
    const bool base_liquidity_pass = (
        metrics.liquidity_score >= std::max(0.0, engine_cfg.foundation_entry_base_liquidity_min) &&
        metrics.volume_surge_ratio >=
            std::max(0.0, engine_cfg.foundation_entry_base_volume_surge_min)
    );
    const bool non_hostile_regime =
        regime != analytics::MarketRegime::HIGH_VOLATILITY &&
        regime != analytics::MarketRegime::TRENDING_DOWN;
    const bool spread_guard_ok =
        !metrics.orderbook_snapshot.valid ||
        metrics.orderbook_snapshot.spread_pct <=
            std::max(0.0, engine_cfg.foundation_entry_spread_guard_max);
    double adaptive_liquidity_floor = std::max(
        0.0,
        engine_cfg.foundation_entry_adaptive_liquidity_floor_default
    );
    double adaptive_volume_floor = std::max(
        0.0,
        engine_cfg.foundation_entry_adaptive_volume_floor_default
    );
    switch (regime) {
        case analytics::MarketRegime::RANGING:
            adaptive_liquidity_floor = std::max(
                0.0,
                engine_cfg.foundation_entry_adaptive_liquidity_floor_ranging
            );
            adaptive_volume_floor = std::max(
                0.0,
                engine_cfg.foundation_entry_adaptive_volume_floor_ranging
            );
            break;
        case analytics::MarketRegime::TRENDING_UP:
            adaptive_liquidity_floor = std::max(
                0.0,
                engine_cfg.foundation_entry_adaptive_liquidity_floor_uptrend
            );
            adaptive_volume_floor = std::max(
                0.0,
                engine_cfg.foundation_entry_adaptive_volume_floor_uptrend
            );
            break;
        case analytics::MarketRegime::UNKNOWN:
            adaptive_liquidity_floor = std::max(
                0.0,
                engine_cfg.foundation_entry_adaptive_liquidity_floor_unknown
            );
            adaptive_volume_floor = std::max(
                0.0,
                engine_cfg.foundation_entry_adaptive_volume_floor_unknown
            );
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
        metrics.volatility <=
            std::max(0.0, engine_cfg.foundation_entry_adaptive_thin_volatility_max) &&
        metrics.liquidity_score >= adaptive_liquidity_floor &&
        metrics.volume_surge_ratio >= adaptive_volume_floor &&
        metrics.order_book_imbalance >
            engine_cfg.foundation_entry_adaptive_thin_imbalance_min &&
        metrics.buy_pressure >=
            (metrics.sell_pressure *
             std::max(0.0, engine_cfg.foundation_entry_adaptive_thin_buy_pressure_ratio_min));
    const bool tight_spread_context =
        metrics.orderbook_snapshot.valid &&
        metrics.orderbook_snapshot.spread_pct > 0.0 &&
        metrics.orderbook_snapshot.spread_pct <=
            std::max(0.0, engine_cfg.foundation_entry_narrow_relief_spread_max);
    const bool low_vol_context =
        metrics.volatility > 0.0 &&
        metrics.volatility <=
            std::max(0.0, engine_cfg.foundation_entry_narrow_relief_volatility_max);
    const bool narrow_liquidity_relief_context =
        tight_spread_context &&
        low_vol_context &&
        regime != analytics::MarketRegime::HIGH_VOLATILITY &&
        regime != analytics::MarketRegime::TRENDING_DOWN;
    const bool narrow_liquidity_relief_pass =
        narrow_liquidity_relief_context &&
        metrics.liquidity_score >=
            std::max(0.0, engine_cfg.foundation_entry_narrow_relief_liquidity_min) &&
        metrics.volume_surge_ratio >=
            std::max(0.0, engine_cfg.foundation_entry_narrow_relief_volume_surge_min) &&
        metrics.order_book_imbalance >
            engine_cfg.foundation_entry_narrow_relief_imbalance_min &&
        metrics.buy_pressure >=
            (metrics.sell_pressure *
             std::max(0.0, engine_cfg.foundation_entry_narrow_relief_buy_pressure_ratio_min));

    const bool core_liquidity_pass =
        base_liquidity_pass ||
        narrow_liquidity_relief_pass ||
        adaptive_thin_flow_pass;
    decision.low_liquidity_relaxed_path = (
        engine_cfg.foundation_entry_enable_low_liq_relaxed_path &&
        regime == analytics::MarketRegime::TRENDING_UP &&
        metrics.liquidity_score >=
            std::max(0.0, engine_cfg.foundation_entry_low_liq_relaxed_liquidity_min) &&
        metrics.liquidity_score <
            std::max(0.0, engine_cfg.foundation_entry_low_liq_relaxed_liquidity_max) &&
        metrics.volume_surge_ratio >=
            std::max(0.0, engine_cfg.foundation_entry_low_liq_relaxed_volume_surge_min) &&
        metrics.order_book_imbalance >
            engine_cfg.foundation_entry_low_liq_relaxed_imbalance_min &&
        metrics.buy_pressure >=
            (metrics.sell_pressure *
             std::max(0.0, engine_cfg.foundation_entry_low_liq_relaxed_buy_pressure_ratio_min))
    );
    decision.thin_liquidity_adaptive_path =
        isThinLiquidityAdaptiveOpportunity(
            engine_cfg,
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
            engine_cfg,
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
            engine_cfg,
            metrics,
            closes,
            current_price,
            ema_fast,
            ema_slow,
            rsi
        );

    if (!core_liquidity_pass) {
        if (regime == analytics::MarketRegime::TRENDING_UP &&
            isLowLiquidityUptrendOpportunity(
                engine_cfg,
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
        if (decision.ranging_low_flow_path) {
            decision.pass = true;
            return decision;
        }
        if (decision.downtrend_low_flow_rebound_path) {
            decision.pass = true;
            return decision;
        }
        if (decision.thin_liquidity_adaptive_path) {
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
            const double ret20_floor =
                (metrics.liquidity_score >= engine_cfg.foundation_entry_uptrend_ret20_floor_liquidity_pivot)
                    ? engine_cfg.foundation_entry_uptrend_ret20_floor_high_liq
                    : engine_cfg.foundation_entry_uptrend_ret20_floor_default;
            const bool overextended_uptrend =
                (rsi >= engine_cfg.foundation_entry_uptrend_overextended_rsi_min &&
                 ret5 >= engine_cfg.foundation_entry_uptrend_overextended_ret5_min);
            const bool base_structure_ok =
                current_price >=
                    ema_fast * engine_cfg.foundation_entry_uptrend_base_price_to_ema_fast_min &&
                ema_fast >=
                    ema_slow * engine_cfg.foundation_entry_uptrend_base_ema_fast_to_ema_slow_min &&
                rsi >= engine_cfg.foundation_entry_uptrend_base_rsi_min &&
                rsi <= engine_cfg.foundation_entry_uptrend_base_rsi_max &&
                metrics.order_book_imbalance >
                    engine_cfg.foundation_entry_uptrend_base_imbalance_min &&
                metrics.buy_pressure >=
                    (metrics.sell_pressure *
                     engine_cfg.foundation_entry_uptrend_base_buy_pressure_ratio_min) &&
                ret5 >= engine_cfg.foundation_entry_uptrend_base_ret5_min &&
                ret20 >= (ret20_floor + engine_cfg.foundation_entry_uptrend_base_ret20_offset);
            const bool thin_uptrend_structure_relief_context =
                metrics.liquidity_score <
                    engine_cfg.foundation_entry_uptrend_relief_context_liquidity_max &&
                metrics.volatility > 0.0 &&
                metrics.volatility <=
                    engine_cfg.foundation_entry_uptrend_relief_context_volatility_max;
            const bool relaxed_structure_ok =
                thin_uptrend_structure_relief_context &&
                current_price >=
                    ema_fast * engine_cfg.foundation_entry_uptrend_relief_price_to_ema_fast_min &&
                ema_fast >=
                    ema_slow * engine_cfg.foundation_entry_uptrend_relief_ema_fast_to_ema_slow_min &&
                rsi >= engine_cfg.foundation_entry_uptrend_relief_rsi_min &&
                rsi <= engine_cfg.foundation_entry_uptrend_relief_rsi_max &&
                metrics.order_book_imbalance >
                    engine_cfg.foundation_entry_uptrend_relief_imbalance_min &&
                metrics.buy_pressure >=
                    (metrics.sell_pressure *
                     engine_cfg.foundation_entry_uptrend_relief_buy_pressure_ratio_min) &&
                ret5 >= engine_cfg.foundation_entry_uptrend_relief_ret5_min &&
                ret20 >= (ret20_floor + engine_cfg.foundation_entry_uptrend_relief_ret20_offset);
            if ((!base_structure_ok && !relaxed_structure_ok) || overextended_uptrend) {
                decision.reject_reason = "foundation_no_signal_uptrend_structure";
                return decision;
            }
            const bool low_vol_high_liq_uptrend =
                metrics.liquidity_score >=
                    engine_cfg.foundation_entry_uptrend_exhaustion_context_liquidity_min &&
                metrics.volatility > 0.0 &&
                metrics.volatility <=
                    engine_cfg.foundation_entry_uptrend_exhaustion_context_volatility_max;
            const double ema_premium = (ema_fast > 1e-9)
                ? ((current_price - ema_fast) / ema_fast)
                : 0.0;
            const bool exhaustion_risk =
                rsi >= engine_cfg.foundation_entry_uptrend_exhaustion_rsi_min &&
                (ret3 >= engine_cfg.foundation_entry_uptrend_exhaustion_ret3_min ||
                 ret5 >= engine_cfg.foundation_entry_uptrend_exhaustion_ret5_min ||
                 ema_premium >= engine_cfg.foundation_entry_uptrend_exhaustion_ema_premium_min);
            if (low_vol_high_liq_uptrend && exhaustion_risk) {
                decision.reject_reason = "foundation_no_signal_uptrend_exhaustion_guard";
                return decision;
            }
            if (thin_uptrend_structure_relief_context) {
                if (!(metrics.volume_surge_ratio >=
                          engine_cfg.foundation_entry_uptrend_thin_context_volume_surge_min &&
                      metrics.order_book_imbalance >=
                          engine_cfg.foundation_entry_uptrend_thin_context_imbalance_min &&
                      rsi <= engine_cfg.foundation_entry_uptrend_thin_context_rsi_max &&
                      ret5 >= engine_cfg.foundation_entry_uptrend_thin_context_ret5_min &&
                      ret20 >= engine_cfg.foundation_entry_uptrend_thin_context_ret20_min)) {
                    decision.reject_reason = "foundation_no_signal_uptrend_thin_context";
                    return decision;
                }
            }
            decision.pass = true;
            return decision;
        }
        case analytics::MarketRegime::RANGING:
            if (!(current_price <=
                      bb_middle * engine_cfg.foundation_entry_ranging_structure_price_to_bb_middle_max &&
                  rsi <= engine_cfg.foundation_entry_ranging_structure_rsi_max &&
                  metrics.order_book_imbalance >
                      engine_cfg.foundation_entry_ranging_structure_imbalance_min &&
                  metrics.buy_pressure >=
                      (metrics.sell_pressure *
                       engine_cfg.foundation_entry_ranging_structure_buy_pressure_ratio_min))) {
                const bool narrow_ranging_relief =
                    narrow_liquidity_relief_pass &&
                    current_price <=
                        bb_middle * engine_cfg.foundation_entry_ranging_relief_price_to_bb_middle_max &&
                    rsi <= engine_cfg.foundation_entry_ranging_relief_rsi_max &&
                    metrics.order_book_imbalance >
                        engine_cfg.foundation_entry_ranging_relief_imbalance_min &&
                    metrics.buy_pressure >=
                        (metrics.sell_pressure *
                         engine_cfg.foundation_entry_ranging_relief_buy_pressure_ratio_min);
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
            if (!(rsi <= engine_cfg.foundation_entry_hostile_bear_rebound_rsi_max &&
                  current_price >
                      ema_fast * engine_cfg.foundation_entry_hostile_bear_rebound_price_to_ema_fast_min &&
                  metrics.buy_pressure >=
                      (metrics.sell_pressure *
                       engine_cfg.foundation_entry_hostile_bear_rebound_buy_pressure_ratio_min) &&
                  metrics.liquidity_score >=
                      engine_cfg.foundation_entry_hostile_bear_rebound_liquidity_min &&
                  metrics.volume_surge_ratio >=
                      engine_cfg.foundation_entry_hostile_bear_rebound_volume_surge_min)) {
                decision.reject_reason = "foundation_no_signal_bear_rebound_guard";
                return decision;
            }
            decision.pass = true;
            return decision;
        case analytics::MarketRegime::UNKNOWN:
        default:
            if (!(current_price >
                      ema_fast * engine_cfg.foundation_entry_unknown_structure_price_to_ema_fast_min &&
                  rsi < engine_cfg.foundation_entry_unknown_structure_rsi_max &&
                  metrics.order_book_imbalance >
                      engine_cfg.foundation_entry_unknown_structure_imbalance_min)) {
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
    analytics::MarketRegime regime
) {
    EntryEvaluationSnapshot snapshot;
    const auto& engine_cfg = autolife::Config::getInstance().getEngineConfig();
    const std::vector<double> closes = analytics::TechnicalIndicators::extractClosePrices(candles);
    if (closes.size() <
        static_cast<std::size_t>(std::max(1, engine_cfg.foundation_entry_snapshot_min_bars))) {
        snapshot.reject_reason = "foundation_no_signal_data_window";
        return snapshot;
    }

    const int snapshot_ema_fast_period =
        std::max(1, engine_cfg.foundation_entry_snapshot_ema_fast_period);
    const int snapshot_ema_slow_period =
        std::max(snapshot_ema_fast_period + 1, engine_cfg.foundation_entry_snapshot_ema_slow_period);
    const int snapshot_rsi_period =
        std::max(2, engine_cfg.foundation_entry_snapshot_rsi_period);
    const int snapshot_bb_period =
        std::max(2, engine_cfg.foundation_entry_snapshot_bb_period);
    const double snapshot_bb_stddev =
        std::max(1e-9, engine_cfg.foundation_entry_snapshot_bb_stddev);

    snapshot.ema_fast = analytics::TechnicalIndicators::calculateEMA(closes, snapshot_ema_fast_period);
    snapshot.ema_slow = analytics::TechnicalIndicators::calculateEMA(closes, snapshot_ema_slow_period);
    snapshot.rsi = analytics::TechnicalIndicators::calculateRSI(closes, snapshot_rsi_period);
    const auto bb = analytics::TechnicalIndicators::calculateBollingerBands(
        closes,
        current_price,
        snapshot_bb_period,
        snapshot_bb_stddev
    );
    snapshot.bb_middle = bb.middle;

    snapshot.entry_gate = evaluateEntryGate(
        metrics,
        closes,
        current_price,
        snapshot.ema_fast,
        snapshot.ema_slow,
        snapshot.rsi,
        snapshot.bb_middle,
        regime
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
    const auto& engine_cfg = autolife::Config::getInstance().getEngineConfig();

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

    const EntryEvaluationSnapshot entry_snapshot = evaluateEntrySnapshot(
        metrics,
        candles,
        current_price,
        regime.regime
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
    const bool thin_liquidity_adaptive_path = entry_gate.thin_liquidity_adaptive_path;
    const bool ranging_low_flow_path = entry_gate.ranging_low_flow_path;
    const bool downtrend_low_flow_rebound_path = entry_gate.downtrend_low_flow_rebound_path;
    const bool thin_low_vol_context =
        (metrics.liquidity_score <
             engine_cfg.foundation_signal_context_thin_low_vol_liquidity_max &&
         metrics.volatility > 0.0 &&
         metrics.volatility <=
             engine_cfg.foundation_signal_context_thin_low_vol_volatility_max);
    const bool thin_low_vol_uptrend_context =
        (regime.regime == analytics::MarketRegime::TRENDING_UP && thin_low_vol_context);
    const bool hostile_uptrend_context =
        (regime.regime == analytics::MarketRegime::TRENDING_UP &&
         metrics.liquidity_score <
             engine_cfg.foundation_signal_context_hostile_uptrend_liquidity_max &&
         metrics.volatility > 0.0 &&
         metrics.volatility <=
             engine_cfg.foundation_signal_context_hostile_uptrend_volatility_max);

    double risk_pct = clampRiskPctByRegime(regime.regime, atr_pct);
    double reward_risk = targetRewardRiskByRegime(regime.regime);
    if (thin_liquidity_adaptive_path) {
        risk_pct *= std::max(0.0, engine_cfg.foundation_signal_path_risk_mult_thin_liq_adaptive);
        reward_risk = std::max(
            reward_risk,
            std::max(0.0, engine_cfg.foundation_signal_path_reward_risk_min_thin_liq_adaptive)
        );
    } else if (downtrend_low_flow_rebound_path) {
        risk_pct *= std::max(0.0, engine_cfg.foundation_signal_path_risk_mult_downtrend_rebound);
        reward_risk = std::max(
            reward_risk,
            std::max(0.0, engine_cfg.foundation_signal_path_reward_risk_min_downtrend_rebound)
        );
    } else if (low_liquidity_relaxed_path) {
        risk_pct *= std::max(0.0, engine_cfg.foundation_signal_path_risk_mult_low_liq_relaxed);
        reward_risk = std::max(
            reward_risk,
            std::max(0.0, engine_cfg.foundation_signal_path_reward_risk_min_low_liq_relaxed)
        );
    }
    if (ranging_low_flow_path) {
        risk_pct *= std::max(0.0, engine_cfg.foundation_signal_path_risk_mult_ranging_low_flow);
        reward_risk = std::max(
            reward_risk,
            std::max(0.0, engine_cfg.foundation_signal_path_reward_risk_min_ranging_low_flow)
        );
    }
    if (thin_low_vol_context) {
        risk_pct *= std::max(0.0, engine_cfg.foundation_signal_context_thin_low_vol_risk_mult);
        if (regime.regime == analytics::MarketRegime::TRENDING_UP) {
            reward_risk = std::clamp(
                reward_risk,
                std::max(0.0, engine_cfg.foundation_signal_context_thin_low_vol_reward_min_uptrend),
                std::max(0.0, engine_cfg.foundation_signal_context_thin_low_vol_reward_max_uptrend)
            );
        } else {
            reward_risk = std::clamp(
                reward_risk,
                std::max(0.0, engine_cfg.foundation_signal_context_thin_low_vol_reward_min_other),
                std::max(0.0, engine_cfg.foundation_signal_context_thin_low_vol_reward_max_other)
            );
        }
    }
    if (hostile_uptrend_context) {
        risk_pct *= std::max(0.0, engine_cfg.foundation_signal_context_hostile_uptrend_risk_mult);
        reward_risk = std::clamp(
            reward_risk,
            std::max(0.0, engine_cfg.foundation_signal_context_hostile_uptrend_reward_min),
            std::max(0.0, engine_cfg.foundation_signal_context_hostile_uptrend_reward_max)
        );
    }
    const double mtf_centered = mtf_confirmation.score - 0.50;
    risk_pct *= std::clamp(
        1.0 - (mtf_centered * engine_cfg.foundation_signal_mtf_risk_scale),
        std::max(0.0, engine_cfg.foundation_signal_mtf_risk_scale_min),
        std::max(0.0, engine_cfg.foundation_signal_mtf_risk_scale_max)
    );
    reward_risk += std::clamp(
        mtf_centered * engine_cfg.foundation_signal_mtf_reward_add_scale,
        engine_cfg.foundation_signal_mtf_reward_add_min,
        engine_cfg.foundation_signal_mtf_reward_add_max
    );
    risk_pct = std::clamp(
        risk_pct,
        std::max(0.0, engine_cfg.foundation_signal_risk_pct_min),
        std::max(0.0, engine_cfg.foundation_signal_risk_pct_max)
    );
    reward_risk = std::clamp(
        reward_risk,
        std::max(0.0, engine_cfg.foundation_signal_reward_risk_min),
        std::max(0.0, engine_cfg.foundation_signal_reward_risk_max)
    );

    const double ema_gap = (ema_slow > 1e-9) ? ((ema_fast - ema_slow) / ema_slow) : 0.0;
    const double strength_ema_gap_add_min = std::min(
        engine_cfg.foundation_signal_strength_ema_gap_add_min,
        engine_cfg.foundation_signal_strength_ema_gap_add_max
    );
    const double strength_ema_gap_add_max = std::max(
        engine_cfg.foundation_signal_strength_ema_gap_add_min,
        engine_cfg.foundation_signal_strength_ema_gap_add_max
    );
    const double strength_rsi_add_min = std::min(
        engine_cfg.foundation_signal_strength_rsi_add_min,
        engine_cfg.foundation_signal_strength_rsi_add_max
    );
    const double strength_rsi_add_max = std::max(
        engine_cfg.foundation_signal_strength_rsi_add_min,
        engine_cfg.foundation_signal_strength_rsi_add_max
    );
    const double strength_liquidity_add_min = std::min(
        engine_cfg.foundation_signal_strength_liquidity_add_min,
        engine_cfg.foundation_signal_strength_liquidity_add_max
    );
    const double strength_liquidity_add_max = std::max(
        engine_cfg.foundation_signal_strength_liquidity_add_min,
        engine_cfg.foundation_signal_strength_liquidity_add_max
    );
    const double strength_mtf_add_min = std::min(
        engine_cfg.foundation_signal_strength_mtf_add_min,
        engine_cfg.foundation_signal_strength_mtf_add_max
    );
    const double strength_mtf_add_max = std::max(
        engine_cfg.foundation_signal_strength_mtf_add_min,
        engine_cfg.foundation_signal_strength_mtf_add_max
    );
    const double strength_final_min = std::min(
        std::clamp(engine_cfg.foundation_signal_strength_final_min, 0.0, 1.0),
        std::clamp(engine_cfg.foundation_signal_strength_final_max, 0.0, 1.0)
    );
    const double strength_final_max = std::max(
        std::clamp(engine_cfg.foundation_signal_strength_final_min, 0.0, 1.0),
        std::clamp(engine_cfg.foundation_signal_strength_final_max, 0.0, 1.0)
    );
    double strength = std::clamp(engine_cfg.foundation_signal_strength_base, 0.0, 1.0);
    strength += std::clamp(
        ema_gap * std::clamp(engine_cfg.foundation_signal_strength_ema_gap_weight, -1000.0, 1000.0),
        strength_ema_gap_add_min,
        strength_ema_gap_add_max
    );
    strength += std::clamp(
        (std::clamp(engine_cfg.foundation_signal_strength_rsi_center, 0.0, 100.0) - rsi) /
            std::max(1e-9, std::abs(engine_cfg.foundation_signal_strength_rsi_divisor)),
        strength_rsi_add_min,
        strength_rsi_add_max
    );
    strength += std::clamp(
        (metrics.liquidity_score - std::clamp(engine_cfg.foundation_signal_strength_liquidity_center, 0.0, 10000.0)) /
            std::max(1e-9, std::abs(engine_cfg.foundation_signal_strength_liquidity_divisor)),
        strength_liquidity_add_min,
        strength_liquidity_add_max
    );
    if (regime.regime == analytics::MarketRegime::TRENDING_UP) {
        strength += std::clamp(engine_cfg.foundation_signal_strength_uptrend_add, -1.0, 1.0);
    }
    if (regime.regime == analytics::MarketRegime::TRENDING_DOWN ||
        regime.regime == analytics::MarketRegime::HIGH_VOLATILITY) {
        strength += std::clamp(engine_cfg.foundation_signal_strength_hostile_regime_add, -1.0, 1.0);
    }
    if (thin_liquidity_adaptive_path) {
        strength += std::clamp(engine_cfg.foundation_signal_strength_path_thin_liq_add, -1.0, 1.0);
    } else if (ranging_low_flow_path) {
        strength += std::clamp(engine_cfg.foundation_signal_strength_path_ranging_add, -1.0, 1.0);
    } else if (low_liquidity_relaxed_path) {
        strength += std::clamp(engine_cfg.foundation_signal_strength_path_low_liq_add, -1.0, 1.0);
    }
    if (hostile_uptrend_context) {
        strength += std::clamp(engine_cfg.foundation_signal_strength_hostile_uptrend_add, -1.0, 1.0);
    }
    strength += std::clamp(
        mtf_centered * std::clamp(engine_cfg.foundation_signal_strength_mtf_scale, -1000.0, 1000.0),
        strength_mtf_add_min,
        strength_mtf_add_max
    );
    strength = std::clamp(strength, strength_final_min, strength_final_max);

    signal.type = (
        regime.regime == analytics::MarketRegime::TRENDING_UP &&
        strength >= std::clamp(engine_cfg.foundation_strong_buy_strength_threshold, 0.0, 1.0)
    )
        ? SignalType::STRONG_BUY
        : SignalType::BUY;
    signal.strength = strength;
    signal.entry_price = current_price;
    signal.stop_loss = current_price * (1.0 - risk_pct);
    const double tp1_rr_multiplier = std::max(0.0, engine_cfg.foundation_take_profit_1_rr_multiplier);
    const double breakeven_rr_multiplier = std::max(0.0, engine_cfg.foundation_breakeven_rr_multiplier);
    const double trailing_rr_multiplier = std::max(0.0, engine_cfg.foundation_trailing_rr_multiplier);
    signal.take_profit_1 = current_price * (1.0 + risk_pct * std::max(1.0, reward_risk * tp1_rr_multiplier));
    signal.take_profit_2 = current_price * (1.0 + risk_pct * reward_risk);
    signal.breakeven_trigger = current_price * (1.0 + risk_pct * breakeven_rr_multiplier);
    signal.trailing_start = current_price * (1.0 + risk_pct * trailing_rr_multiplier);
    signal.position_size = calculatePositionSize(
        available_capital,
        signal.entry_price,
        signal.stop_loss,
        metrics
    );
    const double size_mult_thin_liquidity_adaptive = std::max(
        0.0,
        engine_cfg.foundation_position_mult_thin_liquidity_adaptive
    );
    const double size_mult_downtrend_low_flow_rebound = std::max(
        0.0,
        engine_cfg.foundation_position_mult_downtrend_low_flow_rebound
    );
    const double size_mult_ranging_low_flow = std::max(
        0.0,
        engine_cfg.foundation_position_mult_ranging_low_flow
    );
    const double size_mult_thin_low_vol_uptrend = std::max(
        0.0,
        engine_cfg.foundation_position_mult_thin_low_vol_uptrend
    );
    const double size_mult_thin_low_vol = std::max(
        0.0,
        engine_cfg.foundation_position_mult_thin_low_vol
    );
    if (thin_liquidity_adaptive_path) {
        signal.position_size *= size_mult_thin_liquidity_adaptive;
    } else if (downtrend_low_flow_rebound_path) {
        signal.position_size *= size_mult_downtrend_low_flow_rebound;
    } else if (ranging_low_flow_path) {
        signal.position_size *= size_mult_ranging_low_flow;
    } else if (thin_low_vol_uptrend_context) {
        signal.position_size *= size_mult_thin_low_vol_uptrend;
    } else if (thin_low_vol_context) {
        signal.position_size *= size_mult_thin_low_vol;
    }

    if (signal.position_size <= 0.0) {
        signal.type = SignalType::NONE;
        signal.reason = "foundation_no_signal_position_sizing";
        return signal;
    }

    signal.expected_return_pct = (signal.take_profit_2 - signal.entry_price) / signal.entry_price;
    signal.expected_risk_pct = (signal.entry_price - signal.stop_loss) / signal.entry_price;
    double implied_win_prob = std::clamp(
        std::clamp(engine_cfg.foundation_implied_win_base, 0.0, 1.0) +
            (strength * std::clamp(engine_cfg.foundation_implied_win_strength_scale, -10.0, 10.0)),
        std::clamp(engine_cfg.foundation_implied_win_preclamp_min, 0.0, 1.0),
        std::clamp(engine_cfg.foundation_implied_win_preclamp_max, 0.0, 1.0)
    );
    if (hostile_uptrend_context) {
        implied_win_prob = std::clamp(
            implied_win_prob -
                std::clamp(engine_cfg.foundation_implied_win_hostile_uptrend_penalty, -1.0, 1.0),
            std::clamp(engine_cfg.foundation_implied_win_hostile_min, 0.0, 1.0),
            std::clamp(engine_cfg.foundation_implied_win_hostile_max, 0.0, 1.0)
        );
    }
    implied_win_prob += std::clamp(
        mtf_centered * std::clamp(engine_cfg.foundation_implied_win_mtf_scale, -10.0, 10.0),
        std::clamp(engine_cfg.foundation_implied_win_mtf_add_min, -1.0, 1.0),
        std::clamp(engine_cfg.foundation_implied_win_mtf_add_max, -1.0, 1.0)
    );
    implied_win_prob = std::clamp(
        implied_win_prob,
        std::clamp(engine_cfg.foundation_implied_win_final_min, 0.0, 1.0),
        std::clamp(engine_cfg.foundation_implied_win_final_max, 0.0, 1.0)
    );
    signal.expected_value =
        (implied_win_prob * signal.expected_return_pct) -
        ((1.0 - implied_win_prob) * signal.expected_risk_pct);

    signal.signal_filter = std::clamp(
        std::clamp(engine_cfg.foundation_signal_filter_base, 0.0, 1.0) +
            (strength * std::clamp(engine_cfg.foundation_signal_filter_strength_scale, -10.0, 10.0)),
        std::clamp(engine_cfg.foundation_signal_filter_min, 0.0, 1.0),
        std::clamp(engine_cfg.foundation_signal_filter_max, 0.0, 1.0)
    );
    if (hostile_uptrend_context) {
        signal.signal_filter = std::clamp(
            signal.signal_filter +
                std::clamp(engine_cfg.foundation_signal_filter_hostile_uptrend_add, -1.0, 1.0),
            std::clamp(engine_cfg.foundation_signal_filter_min, 0.0, 1.0),
            std::clamp(engine_cfg.foundation_signal_filter_hostile_uptrend_max, 0.0, 1.0)
        );
    }
    signal.buy_order_type = OrderTypePolicy::LIMIT_WITH_FALLBACK;
    signal.sell_order_type = OrderTypePolicy::LIMIT_WITH_FALLBACK;
    signal.max_retries = 2;
    signal.retry_wait_ms = 700;
    if (thin_liquidity_adaptive_path) {
        signal.reason = "foundation_adaptive_regime_entry_thin_liq_adaptive";
    } else if (downtrend_low_flow_rebound_path) {
        signal.reason = "foundation_adaptive_regime_entry_downtrend_low_flow_rebound";
    } else if (ranging_low_flow_path) {
        signal.reason = "foundation_adaptive_regime_entry_ranging_low_flow";
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
    const EntryEvaluationSnapshot entry_snapshot = evaluateEntrySnapshot(
        metrics,
        candles,
        current_price,
        regime.regime
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
    const auto& cfg = autolife::Config::getInstance().getEngineConfig();
    const double stop_loss_pct = std::clamp(cfg.foundation_exit_stop_loss_pct, 0.0, 1.0);
    const double take_profit_pct = std::clamp(cfg.foundation_exit_take_profit_pct, 0.0, 5.0);
    const double time_limit_hours = std::clamp(cfg.foundation_exit_time_limit_hours, 0.0, 168.0);
    const double time_limit_min_profit_pct = std::clamp(
        cfg.foundation_exit_time_limit_min_profit_pct,
        -1.0,
        1.0
    );
    if (current_price <= entry_price * (1.0 - stop_loss_pct)) {
        return true;
    }
    if (current_price >= entry_price * (1.0 + take_profit_pct)) {
        return true;
    }
    if (holding_time_seconds >= (time_limit_hours * 3600.0) &&
        current_price <= entry_price * (1.0 + time_limit_min_profit_pct)) {
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
    const auto& cfg = autolife::Config::getInstance().getEngineConfig();

    const double stop_distance = std::max(entry_price - stop_loss, entry_price * 0.0025);
    const double risk_budget_pct = std::clamp(cfg.foundation_position_risk_budget_pct, 0.0, 1.0);
    const double risk_budget_krw = capital * risk_budget_pct;
    const double risk_limited_notional = (risk_budget_krw * entry_price) / std::max(1e-9, stop_distance);

    double max_notional = capital * std::clamp(cfg.foundation_position_notional_cap_pct, 0.0, 1.0);
    if (metrics.liquidity_score < 50.0) {
        max_notional *= std::max(0.0, cfg.foundation_position_liq_mult_lt_50);
    } else if (metrics.liquidity_score < 55.0) {
        max_notional *= std::max(0.0, cfg.foundation_position_liq_mult_lt_55);
    }
    if (metrics.volatility > 4.0) {
        max_notional *= std::max(0.0, cfg.foundation_position_vol_mult_gt_4);
    }
    if (metrics.orderbook_snapshot.valid &&
        metrics.orderbook_snapshot.spread_pct >
            std::clamp(cfg.foundation_position_spread_threshold_pct, 0.0, 1.0)) {
        max_notional *= std::max(0.0, cfg.foundation_position_spread_mult);
    }
    const double min_notional_low_liq =
        std::clamp(cfg.foundation_position_min_notional_pct_low_liq, 0.0, 1.0);
    const double min_notional_default =
        std::clamp(cfg.foundation_position_min_notional_pct_default, 0.0, 1.0);
    const double min_notional = capital * ((metrics.liquidity_score < 55.0)
                                               ? min_notional_low_liq
                                               : min_notional_default);
    const double notional = std::clamp(risk_limited_notional, min_notional, max_notional);
    double output_min = std::clamp(cfg.foundation_position_output_min_pct, 0.0, 1.0);
    double output_max = std::clamp(cfg.foundation_position_output_max_pct, 0.0, 1.0);
    if (output_max < output_min) {
        std::swap(output_min, output_max);
    }
    return std::clamp(notional / capital, output_min, output_max);
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
    const auto& cfg = autolife::Config::getInstance().getEngineConfig();
    switch (regime) {
        case analytics::MarketRegime::TRENDING_UP:
            return std::clamp(
                std::max(
                    atr_pct * std::max(0.0, cfg.foundation_risk_mult_uptrend),
                    std::clamp(cfg.foundation_risk_floor_uptrend, 0.0, 1.0)
                ),
                std::clamp(cfg.foundation_risk_floor_uptrend, 0.0, 1.0),
                std::clamp(cfg.foundation_risk_cap_uptrend, 0.0, 1.0)
            );
        case analytics::MarketRegime::RANGING:
            return std::clamp(
                std::max(
                    atr_pct * std::max(0.0, cfg.foundation_risk_mult_ranging),
                    std::clamp(cfg.foundation_risk_floor_ranging, 0.0, 1.0)
                ),
                std::clamp(cfg.foundation_risk_floor_ranging, 0.0, 1.0),
                std::clamp(cfg.foundation_risk_cap_ranging, 0.0, 1.0)
            );
        case analytics::MarketRegime::TRENDING_DOWN:
        case analytics::MarketRegime::HIGH_VOLATILITY:
            return std::clamp(
                std::max(
                    atr_pct * std::max(0.0, cfg.foundation_risk_mult_hostile),
                    std::clamp(cfg.foundation_risk_floor_hostile, 0.0, 1.0)
                ),
                std::clamp(cfg.foundation_risk_floor_hostile, 0.0, 1.0),
                std::clamp(cfg.foundation_risk_cap_hostile, 0.0, 1.0)
            );
        case analytics::MarketRegime::UNKNOWN:
        default:
            return std::clamp(
                std::max(
                    atr_pct * std::max(0.0, cfg.foundation_risk_mult_unknown),
                    std::clamp(cfg.foundation_risk_floor_unknown, 0.0, 1.0)
                ),
                std::clamp(cfg.foundation_risk_floor_unknown, 0.0, 1.0),
                std::clamp(cfg.foundation_risk_cap_unknown, 0.0, 1.0)
            );
    }
}

double FoundationAdaptiveStrategy::targetRewardRiskByRegime(analytics::MarketRegime regime) {
    const auto& cfg = autolife::Config::getInstance().getEngineConfig();
    switch (regime) {
        case analytics::MarketRegime::TRENDING_UP:
            return std::max(0.0, cfg.foundation_reward_risk_uptrend);
        case analytics::MarketRegime::RANGING:
            return std::max(0.0, cfg.foundation_reward_risk_ranging);
        case analytics::MarketRegime::TRENDING_DOWN:
        case analytics::MarketRegime::HIGH_VOLATILITY:
            return std::max(0.0, cfg.foundation_reward_risk_hostile);
        case analytics::MarketRegime::UNKNOWN:
        default:
            return std::max(0.0, cfg.foundation_reward_risk_unknown);
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

