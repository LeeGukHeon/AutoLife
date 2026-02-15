#include "strategy/BreakoutStrategy.h"
#include "analytics/TechnicalIndicators.h"
#include "common/Config.h"
#include "common/PathUtils.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <cstdlib>
#include <spdlog/spdlog.h>

namespace autolife {
namespace strategy {
namespace {
double clamp01(double v) {
    return std::clamp(v, 0.0, 1.0);
}

std::filesystem::path getBreakoutAdaptiveStatsPath() {
    return utils::PathUtils::resolveRelativePath("state/breakout_entry_stats.json");
}

bool disableAdaptiveEntryStateIo() {
    std::string value;
#ifdef _WIN32
    char* raw = nullptr;
    size_t len = 0;
    if (_dupenv_s(&raw, &len, "AUTOLIFE_DISABLE_ADAPTIVE_STATE_IO") != 0 || raw == nullptr) {
        return false;
    }
    value.assign(raw);
    free(raw);
#else
    const char* raw = std::getenv("AUTOLIFE_DISABLE_ADAPTIVE_STATE_IO");
    if (raw == nullptr) {
        return false;
    }
    value.assign(raw);
#endif
    return !(value.empty() || value == "0" || value == "false" || value == "FALSE");
}

long long currentWallClockMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

const char* breakoutTypeToLabel(BreakoutType t) {
    switch (t) {
        case BreakoutType::DONCHIAN_BREAK: return "DONCHIAN_BREAK";
        case BreakoutType::RESISTANCE_BREAK: return "RESISTANCE_BREAK";
        case BreakoutType::CONSOLIDATION_BREAK: return "CONSOLIDATION_BREAK";
        case BreakoutType::VOLUME_BREAKOUT: return "VOLUME_BREAKOUT";
        default: return "UNSPECIFIED";
    }
}

int makeBreakoutEntryKey(BreakoutType t, analytics::MarketRegime regime) {
    return (static_cast<int>(t) * 100) + static_cast<int>(regime);
}

long long normalizeTimestampMs(long long ts) {
    if (ts > 0 && ts < 1000000000000LL) {
        return ts * 1000LL;
    }
    return ts;
}

bool isLikelyDescendingByTimestamp(const std::vector<Candle>& candles) {
    if (candles.size() < 3) {
        return false;
    }
    size_t asc = 0;
    size_t desc = 0;
    for (size_t i = 1; i < candles.size(); ++i) {
        if (candles[i].timestamp > candles[i - 1].timestamp) {
            asc++;
        } else if (candles[i].timestamp < candles[i - 1].timestamp) {
            desc++;
        }
    }
    return desc > asc;
}

std::vector<Candle> ensureChronologicalCandles(const std::vector<Candle>& candles) {
    if (!isLikelyDescendingByTimestamp(candles)) {
        return candles;
    }
    std::vector<Candle> normalized(candles.rbegin(), candles.rend());
    return normalized;
}

double computeDirectionalBias(const std::vector<Candle>& candles, size_t lookback) {
    const auto ordered = ensureChronologicalCandles(candles);
    if (ordered.size() < lookback + 1 || lookback < 3) {
        return 0.0;
    }

    const size_t start = ordered.size() - lookback - 1;
    const double start_close = ordered[start].close;
    const double end_close = ordered.back().close;
    if (start_close <= 0.0 || end_close <= 0.0) {
        return 0.0;
    }

    int up_bars = 0;
    int down_bars = 0;
    for (size_t i = start + 1; i < ordered.size(); ++i) {
        if (ordered[i].close > ordered[i - 1].close) {
            up_bars++;
        } else if (ordered[i].close < ordered[i - 1].close) {
            down_bars++;
        }
    }

    const double step_bias = static_cast<double>(up_bars - down_bars) / static_cast<double>(lookback);
    const double price_move = (end_close - start_close) / start_close;
    const double raw = (price_move * 14.0) + (step_bias * 0.45);
    return std::clamp(raw, -1.0, 1.0);
}

double computeBreakoutHigherTfTrendBias(
    const analytics::CoinMetrics& metrics,
    const std::vector<Candle>& candles_5m
) {
    double bias_5m = 0.0;
    const auto ordered_5m = ensureChronologicalCandles(candles_5m);
    if (ordered_5m.size() >= 25) {
        bias_5m = computeDirectionalBias(ordered_5m, 24);
    }

    double bias_1h = 0.0;
    auto tf_1h_it = metrics.candles_by_tf.find("1h");
    if (tf_1h_it != metrics.candles_by_tf.end() && tf_1h_it->second.size() >= 12) {
        const auto candles_1h = ensureChronologicalCandles(tf_1h_it->second);
        const size_t lookback = std::min<size_t>(12, candles_1h.size() - 1);
        bias_1h = computeDirectionalBias(candles_1h, lookback);
    } else if (ordered_5m.size() >= 49) {
        // Fallback: wider 5m horizon as pseudo higher timeframe trend.
        bias_1h = computeDirectionalBias(ordered_5m, 48);
    }

    double combined = (bias_5m * 0.55) + (bias_1h * 0.45);
    if (metrics.order_book_imbalance < -0.10) {
        combined -= 0.08;
    } else if (metrics.order_book_imbalance > 0.12) {
        combined += 0.05;
    }
    return std::clamp(combined, -1.0, 1.0);
}

double computeBreakoutAdaptiveStrengthFloor(
    const analytics::CoinMetrics& metrics,
    const analytics::RegimeAnalysis& regime,
    double base_floor
) {
    const double vol_t = clamp01(metrics.volatility / 6.0);
    const double liq_stress_t = clamp01((70.0 - metrics.liquidity_score) / 70.0);
    const double pressure_total = std::max(1e-6, std::abs(metrics.buy_pressure) + std::abs(metrics.sell_pressure));
    const double sell_bias_t = clamp01((metrics.sell_pressure - metrics.buy_pressure) / pressure_total);
    const double book_imbalance_t = clamp01((-metrics.order_book_imbalance + 0.05) / 0.60);
    const double flow_t = std::max(sell_bias_t, book_imbalance_t);

    double floor = base_floor + (vol_t * 0.07) + (liq_stress_t * 0.09) + (flow_t * 0.07);
    if (regime.regime == analytics::MarketRegime::HIGH_VOLATILITY) {
        floor += 0.04;
    } else if (regime.regime == analytics::MarketRegime::RANGING) {
        floor += 0.015;
    } else if (regime.regime == analytics::MarketRegime::TRENDING_UP) {
        floor -= 0.01;
    } else if (regime.regime == analytics::MarketRegime::TRENDING_DOWN) {
        floor += 0.08;
    }

    if (metrics.liquidity_score >= 78.0 && metrics.volume_surge_ratio >= 1.9) {
        floor -= 0.04;
    }
    return std::clamp(floor, 0.35, 0.74);
}

double computeBreakoutAdaptiveLiquidityFloor(
    const analytics::CoinMetrics& metrics,
    const analytics::RegimeAnalysis& regime,
    double base_floor
) {
    const double vol_t = clamp01(metrics.volatility / 6.0);
    const double volume_relief_t = clamp01((metrics.volume_surge_ratio - 1.0) / 2.0);
    double floor = base_floor + (vol_t * 8.0) - (volume_relief_t * 7.0);

    if (regime.regime == analytics::MarketRegime::HIGH_VOLATILITY) {
        floor += 3.0;
    } else if (regime.regime == analytics::MarketRegime::TRENDING_DOWN) {
        floor += 8.0;
    } else if (regime.regime == analytics::MarketRegime::TRENDING_UP) {
        floor -= 2.0;
    }
    return std::clamp(floor, 35.0, 80.0);
}

bool isBreakoutRegimeTradable(
    const analytics::CoinMetrics& metrics,
    const analytics::RegimeAnalysis& regime
) {
    switch (regime.regime) {
        case analytics::MarketRegime::TRENDING_UP:
            return true;
        case analytics::MarketRegime::HIGH_VOLATILITY:
            return (
                metrics.liquidity_score >= 72.0 &&
                metrics.volume_surge_ratio >= 1.9 &&
                metrics.order_book_imbalance >= -0.02
            );
        case analytics::MarketRegime::RANGING:
            // Allow only selective range-breakouts with clear flow and liquidity.
            return (
                metrics.liquidity_score >= 66.0 &&
                metrics.volume_surge_ratio >= 1.50 &&
                metrics.order_book_imbalance >= -0.03 &&
                std::abs(metrics.price_change_rate) >= 0.12
            );
        default:
            return false;
    }
}

double computeBreakoutAtrMultipleFloor(const analytics::CoinMetrics& metrics) {
    const double vol_t = clamp01(metrics.volatility / 6.0);
    const double liq_stress_t = clamp01((65.0 - metrics.liquidity_score) / 65.0);
    const double volume_relief_t = clamp01((metrics.volume_surge_ratio - 1.0) / 2.0);
    const double floor = 1.45 + (vol_t * 0.35) + (liq_stress_t * 0.40) - (volume_relief_t * 0.35);
    return std::clamp(floor, 1.00, 2.10);
}

double computeBreakoutVolumeConfirmFloor(const analytics::CoinMetrics& metrics) {
    const double vol_t = clamp01(metrics.volatility / 6.0);
    const double liq_stress_t = clamp01((65.0 - metrics.liquidity_score) / 65.0);
    const double volume_relief_t = clamp01((metrics.volume_surge_ratio - 1.0) / 2.0);
    const double floor = 0.52 + (vol_t * 0.11) + (liq_stress_t * 0.15) - (volume_relief_t * 0.16);
    return std::clamp(floor, 0.36, 0.80);
}

double computeBreakoutMicroValidityFloor(const analytics::CoinMetrics& metrics) {
    const double liq_boost_t = clamp01((metrics.liquidity_score - 55.0) / 45.0);
    const double volume_boost_t = clamp01((metrics.volume_surge_ratio - 1.0) / 2.0);
    const double floor = 0.44 - (liq_boost_t * 0.07) - (volume_boost_t * 0.04);
    return std::clamp(floor, 0.33, 0.47);
}

double computeBreakoutSignalQualityFloor(
    const analytics::CoinMetrics& metrics,
    const analytics::RegimeAnalysis& regime,
    double higher_tf_trend_bias
) {
    double floor = 0.24;
    if (regime.regime == analytics::MarketRegime::TRENDING_UP) {
        floor = 0.29;
    } else if (regime.regime == analytics::MarketRegime::HIGH_VOLATILITY) {
        floor = 0.31;
    } else if (regime.regime == analytics::MarketRegime::RANGING) {
        floor = 0.27;
    } else {
        floor = 0.34;
    }

    if (higher_tf_trend_bias < 0.25) {
        floor += 0.05;
    }
    if (higher_tf_trend_bias < 0.15) {
        floor += 0.05;
    }
    if (metrics.volume_surge_ratio < 1.8) {
        floor += 0.04;
    }
    if (metrics.liquidity_score < 68.0) {
        floor += 0.03;
    }
    if (metrics.order_book_imbalance < -0.08) {
        floor += 0.04;
    }
    if (metrics.buy_pressure < metrics.sell_pressure) {
        floor += 0.03;
    }

    if (higher_tf_trend_bias >= 0.28 &&
        metrics.volume_surge_ratio >= 2.4 &&
        metrics.liquidity_score >= 72.0) {
        floor -= 0.06;
    }
    if (higher_tf_trend_bias >= 0.20 &&
        metrics.volume_surge_ratio >= 1.7 &&
        metrics.liquidity_score >= 66.0) {
        floor -= 0.07;
    }
    return std::clamp(floor, 0.24, 0.55);
}
}

// ===== Constructor =====

BreakoutStrategy::BreakoutStrategy(std::shared_ptr<network::UpbitHttpClient> client)
    : client_(client)
    , enabled_(true)
    , last_signal_time_(0)
    , last_orderbook_fetch_time_(0)
    , daily_trades_count_(0)
    , hourly_trades_count_(0)
    , current_day_start_(0)
    , current_hour_start_(0)
    , consecutive_losses_(0)
    , circuit_breaker_active_(false)
    , circuit_breaker_until_(0)
{
    stats_.total_signals = 0;
    stats_.winning_trades = 0;
    stats_.losing_trades = 0;
    stats_.total_profit = 0.0;
    stats_.total_loss = 0.0;
    stats_.win_rate = 0.0;
    stats_.avg_profit = 0.0;
    stats_.avg_loss = 0.0;
    stats_.profit_factor = 0.0;
    stats_.sharpe_ratio = 0.0;
    
    rolling_stats_.rolling_win_rate = 0.0;
    rolling_stats_.avg_holding_time_minutes = 0.0;
    rolling_stats_.rolling_profit_factor = 0.0;
    rolling_stats_.total_breakouts_detected = 0;
    rolling_stats_.successful_breakouts = 0;
    
    spdlog::info("[BreakoutStrategy] Initialized - Turtle Trading + Volume Profile");
    loadAdaptiveEntryStats();
}

// ===== Strategy Info =====

StrategyInfo BreakoutStrategy::getInfo() const
{
    StrategyInfo info;
    info.name = "Breakout Strategy";
    info.description = "Donchian Channel breakout with volume confirmation and false breakout filtering";
    info.risk_level = 7.0;
    info.timeframe = "5m";
    info.min_capital = 100000.0;
    info.expected_winrate = 0.62;
    return info;
}

// ===== Main Signal Generation =====

Signal BreakoutStrategy::generateSignal(
    const std::string& market,
    const analytics::CoinMetrics& metrics,
    const std::vector<Candle>& candles,
    double current_price,
    double available_capital,
    const analytics::RegimeAnalysis& regime)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto strategy_cfg = Config::getInstance().getBreakoutConfig();
    
    Signal signal;
    signal.type = SignalType::NONE;
    signal.market = market;
    signal.strategy_name = "Breakout Strategy";
    signal.timestamp = getCurrentTimestamp();
    pending_entry_keys_.erase(market);
    if (!candles.empty()) {
        latest_market_timestamp_ms_ = normalizeTimestampMs(candles.back().timestamp);
        signal.timestamp = latest_market_timestamp_ms_;
    }
    
    // ===== Hard Gates =====
    if (active_positions_.find(market) != active_positions_.end()) return signal;
    if (available_capital <= 0) return signal;
    if (!isBreakoutRegimeTradable(metrics, regime)) return signal;
    const double pressure_total = std::max(1e-6, std::abs(metrics.buy_pressure) + std::abs(metrics.sell_pressure));
    const double sell_bias = (metrics.sell_pressure - metrics.buy_pressure) / pressure_total;
    if (metrics.order_book_imbalance < -0.20 &&
        sell_bias > 0.10 &&
        metrics.volume_surge_ratio < 1.8) {
        return signal;
    }
    if (metrics.orderbook_snapshot.valid) {
        double spread_hard_limit = 0.0032;
        if (regime.regime == analytics::MarketRegime::HIGH_VOLATILITY) {
            spread_hard_limit = 0.0042;
        } else if (regime.regime == analytics::MarketRegime::RANGING) {
            spread_hard_limit = 0.0034;
        }
        if (metrics.orderbook_snapshot.spread_pct > spread_hard_limit &&
            metrics.liquidity_score < 72.0) {
            return signal;
        }
    }
    
    std::vector<Candle> candles_5m;
    auto tf_5m_it = metrics.candles_by_tf.find("5m");
    const bool used_preloaded_5m = (tf_5m_it != metrics.candles_by_tf.end() && tf_5m_it->second.size() >= 30);
    signal.used_preloaded_tf_5m = used_preloaded_5m;
    signal.used_preloaded_tf_1h =
        metrics.candles_by_tf.find("1h") != metrics.candles_by_tf.end() &&
        metrics.candles_by_tf.at("1h").size() >= 26;
    signal.used_resampled_tf_fallback = !used_preloaded_5m;
    if (used_preloaded_5m) {
        candles_5m = tf_5m_it->second;
    } else {
        candles_5m = resampleTo5m(candles);
    }
    candles_5m = ensureChronologicalCandles(candles_5m);
    if (candles_5m.size() < 30) return signal;
    const double higher_tf_trend_bias = computeBreakoutHigherTfTrendBias(metrics, candles_5m);
    if (higher_tf_trend_bias < -0.18) {
        return signal;
    }
    if (higher_tf_trend_bias < 0.02 && metrics.volume_surge_ratio < 2.0) {
        return signal;
    }
    if (regime.regime == analytics::MarketRegime::TRENDING_UP) {
        if (higher_tf_trend_bias < 0.10) {
            return signal;
        }
        if (higher_tf_trend_bias < 0.18 &&
            (metrics.volume_surge_ratio < 1.8 || metrics.liquidity_score < 64.0)) {
            return signal;
        }
    }
    if (regime.regime == analytics::MarketRegime::RANGING &&
        higher_tf_trend_bias < 0.05 &&
        metrics.volume_surge_ratio < 2.0) {
        return signal;
    }
    if (!canTradeNow()) return signal;
    if (daily_trades_count_ >= 6 &&
        (regime.regime != analytics::MarketRegime::TRENDING_UP ||
         metrics.liquidity_score < 72.0 ||
         metrics.volume_surge_ratio < 1.8)) {
        return signal;
    }
    
    checkCircuitBreaker();
    if (isCircuitBreakerActive()) return signal;
    
    // ===== Score-Based Evaluation =====
    double total_score = 0.0;
    
    // --- (1) 돌파 분석 (최대 0.40) ---
    BreakoutSignalMetrics breakout = analyzeBreakout(market, metrics, candles_5m, current_price);
    if (!shouldGenerateBreakoutSignal(breakout)) {
        return signal;
    }
    const int entry_key = makeBreakoutEntryKey(breakout.type, regime.regime);
    const double adaptive_bias = getAdaptiveEntryBias(entry_key);
    double breakout_quality_floor =
        computeBreakoutSignalQualityFloor(metrics, regime, higher_tf_trend_bias);
    if (adaptive_bias <= -0.18 && higher_tf_trend_bias < 0.30) {
        return signal;
    }
    if (adaptive_bias < 0.0) {
        breakout_quality_floor = std::min(0.92, breakout_quality_floor + std::min(0.10, -adaptive_bias * 0.35));
    } else if (adaptive_bias > 0.0) {
        breakout_quality_floor = std::max(0.28, breakout_quality_floor - std::min(0.05, adaptive_bias * 0.25));
    }
    if (breakout.strength < breakout_quality_floor) {
        return signal;
    }
    total_score += breakout.strength * 0.40;
    
    // --- (2) 유동성 (최대 0.10) ---
    const double adaptive_liq_floor = computeBreakoutAdaptiveLiquidityFloor(metrics, regime, strategy_cfg.min_liquidity_score);
    if (metrics.liquidity_score >= adaptive_liq_floor) total_score += 0.10;
    else if (metrics.liquidity_score >= adaptive_liq_floor * 0.85) total_score += 0.06;
    else if (metrics.liquidity_score >= adaptive_liq_floor * 0.70) total_score += 0.03;
    
    // --- (3) 레짐 (최대 0.15) ---
    double regime_score = 0.0;
    switch (regime.regime) {
        case analytics::MarketRegime::TRENDING_UP: regime_score = 0.15; break;
        case analytics::MarketRegime::HIGH_VOLATILITY: regime_score = 0.11; break;
        case analytics::MarketRegime::RANGING: regime_score = 0.08; break;
        default: regime_score = 0.00; break;
    }
    total_score += regime_score;
    
    // --- (4) 거래량 (최대 0.15) ---
    if (metrics.volume_surge_ratio >= 3.0) total_score += 0.15;
    else if (metrics.volume_surge_ratio >= 1.5) total_score += 0.10;
    else if (metrics.volume_surge_ratio >= 1.0) total_score += 0.05;

    if (higher_tf_trend_bias >= 0.30) total_score += 0.10;
    else if (higher_tf_trend_bias >= 0.15) total_score += 0.06;
    else if (higher_tf_trend_bias >= 0.05) total_score += 0.03;
    else if (higher_tf_trend_bias <= -0.20) total_score -= 0.10;
    else if (higher_tf_trend_bias <= -0.08) total_score -= 0.06;
    
    // --- (5) 기술 지표 (최대 0.20) ---
    auto prices = analytics::TechnicalIndicators::extractClosePrices(candles_5m);
    double rsi = analytics::TechnicalIndicators::calculateRSI(prices, 14);
    auto macd_data = analytics::TechnicalIndicators::calculateMACD(prices, 12, 26, 9);
    
    if (rsi >= 50 && rsi <= 70) total_score += 0.08;
    else if (rsi >= 40) total_score += 0.04;
    
    if (macd_data.histogram > 0) total_score += 0.08;
    else if (macd_data.macd > 0) total_score += 0.04;

    // 가격 변동률
    if (metrics.price_change_rate > 1.0) total_score += 0.04;
    else if (metrics.price_change_rate > 0.3) total_score += 0.02;
    
    // ===== 최종 판정 =====
    signal.strength = std::clamp(total_score, 0.0, 1.0);
    if (regime.regime == analytics::MarketRegime::TRENDING_UP &&
        signal.strength >= 0.55 &&
        signal.strength < 0.70 &&
        higher_tf_trend_bias < 0.30) {
        return signal;
    }

    double adaptive_strength_floor = computeBreakoutAdaptiveStrengthFloor(
        metrics, regime, strategy_cfg.min_signal_strength);
    if (higher_tf_trend_bias >= 0.25) {
        adaptive_strength_floor = std::max(0.33, adaptive_strength_floor - 0.03);
    } else if (higher_tf_trend_bias < 0.0) {
        adaptive_strength_floor = std::min(0.78, adaptive_strength_floor + 0.04);
    }
    if (breakout.strength >= 0.72 &&
        higher_tf_trend_bias >= 0.24 &&
        metrics.volume_surge_ratio >= 1.8 &&
        metrics.liquidity_score >= 70.0) {
        adaptive_strength_floor = std::max(0.31, adaptive_strength_floor - 0.04);
    }
    if (adaptive_bias < 0.0) {
        adaptive_strength_floor = std::min(0.90, adaptive_strength_floor + std::min(0.10, -adaptive_bias * 0.35));
    } else if (adaptive_bias > 0.0) {
        adaptive_strength_floor = std::max(0.26, adaptive_strength_floor - std::min(0.05, adaptive_bias * 0.25));
    }
    if (signal.strength < adaptive_strength_floor) return signal;
    
    // ===== 신호 생성 =====
    signal.type = SignalType::BUY;
    signal.entry_price = current_price;
    signal.stop_loss = calculateStopLoss(current_price, candles_5m);
    double risk = current_price - signal.stop_loss;
    signal.take_profit_1 = current_price + (risk * 2.0 * 0.5);
    signal.take_profit_2 = calculateTakeProfit(current_price, candles_5m);
    signal.buy_order_type = strategy::OrderTypePolicy::LIMIT_WITH_FALLBACK;
    signal.sell_order_type = strategy::OrderTypePolicy::LIMIT_WITH_FALLBACK;
    signal.max_retries = 3;
    signal.retry_wait_ms = 500;
    
    // 포지션 사이징
    signal.position_size = calculatePositionSize(available_capital, current_price, signal.stop_loss, metrics);
    
    double order_amount = available_capital * signal.position_size;
    if (order_amount < MIN_ORDER_AMOUNT_KRW) {
        signal.type = SignalType::NONE;
        return signal;
    }
    
    // EV gate (hard): skip weak breakouts that cannot cover trading costs.
    if (risk > 0) {
        const auto& engine_cfg = Config::getInstance().getEngineConfig();
        const double expected_return = (signal.take_profit_2 - current_price) / current_price;
        const double expected_rr = (signal.take_profit_2 - current_price) / risk;
        const double fee_rate = Config::getInstance().getFeeRate();
        double spread_penalty = 0.0;
        if (metrics.orderbook_snapshot.valid) {
            spread_penalty = std::clamp(metrics.orderbook_snapshot.spread_pct * 0.60, 0.0, 0.0020);
        }
        const double round_trip_cost = (fee_rate * 2.0) + (engine_cfg.max_slippage_pct * 2.0) + spread_penalty;
        const double net_edge = expected_return - round_trip_cost;
        double rr_floor = strategy_cfg.min_risk_reward_ratio;
        if (regime.regime == analytics::MarketRegime::HIGH_VOLATILITY) {
            rr_floor = std::max(rr_floor, 1.70);
        } else if (regime.regime == analytics::MarketRegime::RANGING) {
            rr_floor = std::max(rr_floor, 1.60);
        }
        if (higher_tf_trend_bias < 0.08) {
            rr_floor = std::max(rr_floor, 1.75);
        } else if (higher_tf_trend_bias > 0.30) {
            rr_floor = std::max(1.35, rr_floor - 0.15);
        }
        double edge_multiplier = 1.03;
        if (regime.regime == analytics::MarketRegime::HIGH_VOLATILITY) {
            edge_multiplier = 1.10;
        } else if (regime.regime == analytics::MarketRegime::RANGING) {
            edge_multiplier = 1.06;
        }
        if (metrics.liquidity_score < 62.0) {
            edge_multiplier = std::max(edge_multiplier, 1.08);
        }
        if (signal.strength >= 0.72 &&
            metrics.liquidity_score >= 72.0 &&
            metrics.volume_surge_ratio >= 1.8 &&
            sell_bias <= 0.0) {
            edge_multiplier = std::max(0.98, edge_multiplier - 0.05);
        }
        if (higher_tf_trend_bias < 0.0) {
            edge_multiplier = std::max(edge_multiplier, 1.12);
        } else if (higher_tf_trend_bias > 0.30) {
            edge_multiplier = std::max(0.95, edge_multiplier - 0.05);
        }
        if (expected_rr < rr_floor || net_edge <= engine_cfg.min_expected_edge_pct * edge_multiplier) {
            signal.type = SignalType::NONE;
            return signal;
        }
    } else {
        signal.type = SignalType::NONE;
        return signal;
    }
    
    if (signal.strength >= 0.70) {
        signal.type = SignalType::STRONG_BUY;
    }
    signal.entry_archetype = breakoutTypeToLabel(breakout.type);
    pending_entry_keys_[market] = entry_key;
    
    spdlog::info("[Breakout] 🎯 BUY Signal - {} | Strength: {:.3f} | Size: {:.2f}%",
                 market, signal.strength, signal.position_size * 100);
    
    last_signal_time_ = getCurrentTimestamp();
    rolling_stats_.total_breakouts_detected++;
    
    return signal;
}

// ===== Entry Decision =====

bool BreakoutStrategy::shouldEnter(
    const std::string& market,
    const analytics::CoinMetrics& metrics,
    const std::vector<Candle>& candles,
    double current_price,
    const analytics::RegimeAnalysis& regime)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto strategy_cfg = Config::getInstance().getBreakoutConfig();
    if (!candles.empty()) {
        latest_market_timestamp_ms_ = normalizeTimestampMs(candles.back().timestamp);
    }

    if (active_positions_.find(market) != active_positions_.end()) {
        return false;
    }

    if (!isBreakoutRegimeTradable(metrics, regime)) {
        return false;
    }
    const double pressure_total = std::max(1e-6, std::abs(metrics.buy_pressure) + std::abs(metrics.sell_pressure));
    const double sell_bias = (metrics.sell_pressure - metrics.buy_pressure) / pressure_total;
    if (metrics.order_book_imbalance < -0.20 &&
        sell_bias > 0.10 &&
        metrics.volume_surge_ratio < 1.8) {
        return false;
    }
    if (metrics.orderbook_snapshot.valid) {
        double spread_hard_limit = 0.0032;
        if (regime.regime == analytics::MarketRegime::HIGH_VOLATILITY) {
            spread_hard_limit = 0.0042;
        } else if (regime.regime == analytics::MarketRegime::RANGING) {
            spread_hard_limit = 0.0034;
        }
        if (metrics.orderbook_snapshot.spread_pct > spread_hard_limit &&
            metrics.liquidity_score < 72.0) {
            return false;
        }
    }

    std::vector<Candle> candles_5m;
    auto tf_5m_it = metrics.candles_by_tf.find("5m");
    if (tf_5m_it != metrics.candles_by_tf.end() && tf_5m_it->second.size() >= 30) {
        candles_5m = tf_5m_it->second;
    } else {
        candles_5m = resampleTo5m(candles);
    }
    candles_5m = ensureChronologicalCandles(candles_5m);

    if (candles_5m.size() < 30) {
        return false;
    }
    const double higher_tf_trend_bias = computeBreakoutHigherTfTrendBias(metrics, candles_5m);
    if (higher_tf_trend_bias < -0.18) {
        return false;
    }
    if (higher_tf_trend_bias < 0.02 && metrics.volume_surge_ratio < 2.0) {
        return false;
    }
    if (regime.regime == analytics::MarketRegime::TRENDING_UP) {
        if (higher_tf_trend_bias < 0.10) {
            return false;
        }
        if (higher_tf_trend_bias < 0.18 &&
            (metrics.volume_surge_ratio < 1.8 || metrics.liquidity_score < 64.0)) {
            return false;
        }
    }
    if (regime.regime == analytics::MarketRegime::RANGING &&
        higher_tf_trend_bias < 0.05 &&
        metrics.volume_surge_ratio < 2.0) {
        return false;
    }
    if (daily_trades_count_ >= 6 &&
        (regime.regime != analytics::MarketRegime::TRENDING_UP ||
         metrics.liquidity_score < 72.0 ||
         metrics.volume_surge_ratio < 1.8)) {
        return false;
    }

    const double adaptive_liq_floor = computeBreakoutAdaptiveLiquidityFloor(
        metrics, regime, strategy_cfg.min_liquidity_score);
    if (metrics.liquidity_score < adaptive_liq_floor) {
        return false;
    }

    if (!canTradeNow()) {
        return false;
    }

    checkCircuitBreaker();
    if (isCircuitBreakerActive()) {
        return false;
    }

    long long now = getCurrentTimestamp();
    if ((now - last_signal_time_) < static_cast<long long>(strategy_cfg.min_signal_interval_sec) * 1000) {
        return false;
    }

    BreakoutSignalMetrics breakout = analyzeBreakout(market, metrics, candles_5m, current_price);
    if (!shouldGenerateBreakoutSignal(breakout)) {
        return false;
    }
    const double breakout_quality_floor =
        computeBreakoutSignalQualityFloor(metrics, regime, higher_tf_trend_bias);
    return breakout.strength >= breakout_quality_floor;
}

bool BreakoutStrategy::onSignalAccepted(const Signal& signal, double allocated_capital)
{
    (void)allocated_capital;

    std::lock_guard<std::mutex> lock(mutex_);

    if (signal.market.empty()) {
        return false;
    }

    if (active_positions_.find(signal.market) != active_positions_.end()) {
        return false;
    }

    BreakoutPositionData pos_data;
    pos_data.market = signal.market;
    pos_data.entry_price = signal.entry_price;
    pos_data.highest_price = signal.entry_price;
    pos_data.trailing_stop = signal.stop_loss > 0.0
        ? signal.stop_loss
        : signal.entry_price * (1.0 - BASE_STOP_LOSS);
    pos_data.entry_timestamp = getCurrentTimestamp();
    pos_data.tp1_hit = false;
    pos_data.tp2_hit = false;

    active_positions_.insert(signal.market);
    position_data_[signal.market] = pos_data;
    {
        int entry_key = 0;
        auto pending_it = pending_entry_keys_.find(signal.market);
        if (pending_it != pending_entry_keys_.end()) {
            entry_key = pending_it->second;
            pending_entry_keys_.erase(pending_it);
        }
        active_entry_keys_[signal.market] = entry_key;
    }
    recordTrade();

    spdlog::info("[Breakout] Position Registered - {} | Entry: {:.2f}", signal.market, signal.entry_price);
    return true;
}

// ===== Exit Decision =====

bool BreakoutStrategy::shouldExit(
    const std::string& market,
    double entry_price,
    double current_price,
    double holding_time_seconds)
{
    std::lock_guard<std::mutex> lock(mutex_);
    
    // [수정] position_data_ 맵 사용
    if (position_data_.find(market) == position_data_.end()) {
        return false;
    }
    
    auto& pos_data = position_data_[market];
    
    double profit_pct = (current_price - entry_price) / entry_price;
    double holding_minutes = holding_time_seconds / 60.0;
    
    // 고가 갱신
    if (current_price > pos_data.highest_price) {
        pos_data.highest_price = current_price;
    }
    
    // ===== 1. 손절 =====
    // 진입 시 설정한 trailing_stop(초기엔 stop_loss)을 건드리면 청산
    if (current_price <= pos_data.trailing_stop) {
        spdlog::info("[Breakout] Stop Loss / Trailing Stop - {} | Price: {:.2f} <= {:.2f}", 
                     market, current_price, pos_data.trailing_stop);
        // [중요] 여기서 erase 하지 않음! true만 리턴
        return true;
    }
    
    // ===== 2. 트레일링 스탑 업데이트 =====
    if (profit_pct >= TRAILING_ACTIVATION) {
        double new_trailing = pos_data.highest_price * (1.0 - TRAILING_DISTANCE);
        
        if (new_trailing > pos_data.trailing_stop) {
            pos_data.trailing_stop = new_trailing;
            // 로그가 너무 많으면 주석 처리
            // spdlog::debug("[Breakout] TS Updated: {}", new_trailing);
        }
    }
    
    // ===== 3. 익절 2 (6%) - 전량 청산 =====
    if (!pos_data.tp2_hit && profit_pct >= BASE_TAKE_PROFIT_2) {
        spdlog::info("[Breakout] Take Profit 2 (Full Exit) - {} | Profit: {:.2f}%", market, profit_pct * 100);
        return true;
    }
    
    // ===== 4. 익절 1 (3.5%) - 부분 청산 기록 =====
    if (!pos_data.tp1_hit && profit_pct >= BASE_TAKE_PROFIT_1) {
        pos_data.tp1_hit = true;
        spdlog::info("[Breakout] Take Profit 1 Hit - {} | Profit: {:.2f}%", market, profit_pct * 100);
        // 부분 청산은 엔진(TradingEngine)의 monitorPositions에서 처리하므로 여기선 false 유지
        // 만약 여기서 true를 리턴하면 전량 청산되버림
    }
    
    // ===== 5. 시간 제한 =====
    if (holding_minutes >= MAX_HOLDING_TIME_MINUTES) {
        spdlog::info("[Breakout] Max Holding Time - {} | Duration: {:.1f} min", market, holding_minutes);
        return true;
    }
    
    // ===== 6. Breakeven 이동 =====
    if (shouldMoveToBreakeven(entry_price, current_price)) {
        // 수수료 포함 본전 위로 스탑 이동
        double break_even_price = entry_price * 1.002; 
        if (pos_data.trailing_stop < break_even_price) {
            pos_data.trailing_stop = break_even_price;
            spdlog::info("[Breakout] Moved to Breakeven - {}", market);
        }
    }
    
    return false;
}

// ===== Calculate Stop Loss =====

double BreakoutStrategy::calculateStopLoss(
    double entry_price,
    const std::vector<Candle>& candles)
{
    //std::lock_guard<std::mutex> lock(mutex_);
    
    double atr = calculateATR(candles, 14);
    double atr_pct = atr / entry_price;
    
    double stop_pct = std::max(BASE_STOP_LOSS, atr_pct * 1.5);
    stop_pct = std::min(stop_pct, 0.03);
    
    return entry_price * (1.0 - stop_pct);
}

// ===== Calculate Take Profit =====

double BreakoutStrategy::calculateTakeProfit(
    double entry_price,
    const std::vector<Candle>& candles)
{
   // std::lock_guard<std::mutex> lock(mutex_);
   (void)candles;  // candles 파라미터 미사용
    return entry_price * (1.0 + BASE_TAKE_PROFIT_1);
}

// ===== Calculate Position Size =====

double BreakoutStrategy::calculatePositionSize(
    double capital,
    double entry_price,
    double stop_loss,
    const analytics::CoinMetrics& metrics)
{
    //std::lock_guard<std::mutex> lock(mutex_);
    (void)capital;    // capital 파라미터 미사용
    (void)metrics;    // metrics 파라미터 미사용
    
    double risk_pct = (entry_price - stop_loss) / entry_price;
    
    if (risk_pct <= 0) {
        return 0.05;
    }
    
    double win_rate = stats_.win_rate > 0 ? stats_.win_rate : 0.60;
    double avg_win = stats_.avg_profit > 0 ? stats_.avg_profit : BASE_TAKE_PROFIT_1;
    double avg_loss = stats_.avg_loss > 0 ? stats_.avg_loss : BASE_STOP_LOSS;
    
    double kelly = calculateKellyFraction(win_rate, avg_win, avg_loss);
    double half_kelly = kelly * 0.5;
    
    double volatility = calculateVolatility(metrics.candles);
    double vol_adj = adjustForVolatility(half_kelly, volatility);
    
    double position_size = std::clamp(vol_adj, 0.05, MAX_POSITION_SIZE);
    
    return position_size;
}

// ===== Enabled =====

void BreakoutStrategy::setEnabled(bool enabled)
{
    std::lock_guard<std::mutex> lock(mutex_);
    enabled_ = enabled;
    spdlog::info("[BreakoutStrategy] Enabled: {}", enabled);
}

bool BreakoutStrategy::isEnabled() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return enabled_;
}

// ===== Statistics =====

IStrategy::Statistics BreakoutStrategy::getStatistics() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

void BreakoutStrategy::updateStatistics(const std::string& market, bool is_win, double profit_loss)
{
    std::lock_guard<std::mutex> lock(mutex_);
    int closed_entry_key = 0;
    auto key_it = active_entry_keys_.find(market);
    if (key_it != active_entry_keys_.end()) {
        closed_entry_key = key_it->second;
        active_entry_keys_.erase(key_it);
    }
    pending_entry_keys_.erase(market);
    
    // [핵심] 거래가 완전히 끝났으므로 목록에서 제거 (재진입 허용)
    if (active_positions_.erase(market)) {
        // active_positions_에 있었다면 position_data_에도 있을 것임
        position_data_.erase(market);
        spdlog::info("[Breakout] Position Cleared - {} (Ready for next trade)", market);
    } else {
        // 혹시 모르니 map에서도 확인 사살
        position_data_.erase(market);
    }
    
    // --- 통계 업데이트 (기존 로직) ---
    stats_.total_signals++;
    
    if (is_win) {
        stats_.winning_trades++;
        stats_.total_profit += profit_loss;
        consecutive_losses_ = 0;
    } else {
        stats_.losing_trades++;
        stats_.total_loss += std::abs(profit_loss);
        consecutive_losses_++;
    }
    
    if (stats_.total_signals > 0) {
        stats_.win_rate = static_cast<double>(stats_.winning_trades) / stats_.total_signals;
    }
    
    if (stats_.winning_trades > 0) {
        stats_.avg_profit = stats_.total_profit / stats_.winning_trades;
    }
    
    if (stats_.losing_trades > 0) {
        stats_.avg_loss = stats_.total_loss / stats_.losing_trades;
    }
    
    if (stats_.total_loss > 0) {
        stats_.profit_factor = stats_.total_profit / stats_.total_loss;
    }
    
    recent_returns_.push_back(profit_loss);
    if (recent_returns_.size() > 1000) {
        recent_returns_.pop_front();
    }
    
    // 거래 시간 기록 등 추가 로직이 있다면 여기서 처리
    trade_timestamps_.push_back(getCurrentTimestamp());
    if (trade_timestamps_.size() > 1000) {
        trade_timestamps_.pop_front();
    }
    
    updateRollingStatistics();
    checkCircuitBreaker();
    if (closed_entry_key != 0) {
        recordAdaptiveEntryOutcome(closed_entry_key, is_win, profit_loss);
    }
}

void BreakoutStrategy::setStatistics(const Statistics& stats)
{
    std::lock_guard<std::mutex> lock(mutex_);
    stats_ = stats;
}

// ===== Trailing Stop =====

double BreakoutStrategy::updateTrailingStop(
    double entry_price,
    double highest_price,
    double current_price)
{
    //std::lock_guard<std::mutex> lock(mutex_);
    (void)current_price;  // current_price 파라미터 미사용
    
    double profit_pct = (highest_price - entry_price) / entry_price;
    
    if (profit_pct >= TRAILING_ACTIVATION) {
        return highest_price * (1.0 - TRAILING_DISTANCE);
    }
    
    return 0.0;
}

// ===== Breakeven =====

bool BreakoutStrategy::shouldMoveToBreakeven(
    double entry_price,
    double current_price)
{
    double profit_pct = (current_price - entry_price) / entry_price;
    return profit_pct >= BREAKEVEN_TRIGGER;
}

// ===== Rolling Statistics =====

BreakoutRollingStatistics BreakoutStrategy::getRollingStatistics() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return rolling_stats_;
}

double BreakoutStrategy::getAdaptiveEntryBias(int entry_key) const
{
    auto it = adaptive_entry_stats_.find(entry_key);
    if (it == adaptive_entry_stats_.end()) {
        return 0.0;
    }
    const auto& st = it->second;
    if (st.trades < ADAPTIVE_ENTRY_MIN_TRADES) {
        return 0.0;
    }
    const double win_rate = static_cast<double>(st.wins) / std::max(1, st.trades);
    const double expectancy = st.pnl_sum / std::max(1, st.trades);
    const double win_component = std::clamp((win_rate - 0.50) * 0.60, -0.18, 0.18);
    const double expectancy_component = std::clamp(expectancy / 30.0, -0.12, 0.12);
    const double ema_component = std::clamp(st.pnl_ema / 25.0, -0.10, 0.10);
    return std::clamp(win_component + expectancy_component + ema_component, -0.22, 0.16);
}

void BreakoutStrategy::recordAdaptiveEntryOutcome(int entry_key, bool is_win, double profit_loss)
{
    auto& st = adaptive_entry_stats_[entry_key];
    st.trades += 1;
    if (is_win) {
        st.wins += 1;
    }
    st.pnl_sum += profit_loss;
    if (st.trades == 1) {
        st.pnl_ema = profit_loss;
    } else {
        st.pnl_ema = (st.pnl_ema * 0.82) + (profit_loss * 0.18);
    }
    saveAdaptiveEntryStats();
}

void BreakoutStrategy::loadAdaptiveEntryStats()
{
    if (disableAdaptiveEntryStateIo()) {
        spdlog::info("[Breakout] Adaptive entry stats I/O disabled by environment.");
        return;
    }
    try {
        const auto path = getBreakoutAdaptiveStatsPath();
        if (!std::filesystem::exists(path)) {
            return;
        }
        std::ifstream in(path.string(), std::ios::binary);
        if (!in.is_open()) {
            spdlog::warn("[Breakout] Failed to open adaptive stats file: {}", path.string());
            return;
        }
        nlohmann::json payload;
        in >> payload;
        if (!payload.contains("stats") || !payload["stats"].is_array()) {
            return;
        }
        std::map<int, AdaptiveEntryStats> loaded;
        for (const auto& row : payload["stats"]) {
            if (!row.is_object()) {
                continue;
            }
            const int key = row.value("key", 0);
            if (key == 0) {
                continue;
            }
            AdaptiveEntryStats s;
            s.trades = std::max(0, row.value("trades", 0));
            s.wins = std::max(0, row.value("wins", 0));
            s.pnl_sum = row.value("pnl_sum", 0.0);
            s.pnl_ema = row.value("pnl_ema", 0.0);
            loaded[key] = s;
        }
        adaptive_entry_stats_ = std::move(loaded);
        spdlog::info(
            "[Breakout] Adaptive entry stats loaded: path={} entries={}",
            path.string(),
            adaptive_entry_stats_.size()
        );
    } catch (const std::exception& e) {
        spdlog::warn("[Breakout] Failed to load adaptive entry stats: {}", e.what());
    }
}

void BreakoutStrategy::saveAdaptiveEntryStats() const
{
    if (disableAdaptiveEntryStateIo()) {
        return;
    }
    try {
        const auto path = getBreakoutAdaptiveStatsPath();
        std::filesystem::create_directories(path.parent_path());
        nlohmann::json payload;
        payload["schema_version"] = 1;
        payload["saved_at_ms"] = currentWallClockMs();
        payload["stats"] = nlohmann::json::array();
        for (const auto& kv : adaptive_entry_stats_) {
            const auto& s = kv.second;
            nlohmann::json row;
            row["key"] = kv.first;
            row["trades"] = s.trades;
            row["wins"] = s.wins;
            row["pnl_sum"] = s.pnl_sum;
            row["pnl_ema"] = s.pnl_ema;
            payload["stats"].push_back(std::move(row));
        }
        std::ofstream out(path.string(), std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            spdlog::warn("[Breakout] Failed to open adaptive stats for write: {}", path.string());
            return;
        }
        out << payload.dump(2);
    } catch (const std::exception& e) {
        spdlog::warn("[Breakout] Failed to save adaptive entry stats: {}", e.what());
    }
}

void BreakoutStrategy::updateRollingStatistics()
{
    rolling_stats_.rolling_win_rate = stats_.win_rate;
    
    if (!recent_holding_times_.empty()) {
        double sum = std::accumulate(recent_holding_times_.begin(), recent_holding_times_.end(), 0.0);
        rolling_stats_.avg_holding_time_minutes = (sum / recent_holding_times_.size()) / 60.0;
    }
    
    rolling_stats_.rolling_profit_factor = stats_.profit_factor;
    
    if (recent_returns_.size() >= 30) {
        std::vector<double> returns_vec(recent_returns_.begin(), recent_returns_.end());
        double mean = calculateMean(returns_vec);
        double std_dev = calculateStdDev(returns_vec, mean);
        
        if (std_dev > 0) {
            double annual_mean = mean * 525600;
            double annual_std = std_dev * std::sqrt(525600);
            stats_.sharpe_ratio = annual_mean / annual_std;
        }
    }
}

// ===== API Call Management =====

bool BreakoutStrategy::canMakeOrderBookAPICall() const
{
    long long now = getCurrentTimestamp();
    
    int recent_calls = 0;
    for (auto it = api_call_timestamps_.rbegin(); it != api_call_timestamps_.rend(); ++it) {
        if (now - *it < 1000) {
            recent_calls++;
        } else {
            break;
        }
    }
    
    return recent_calls < MAX_ORDERBOOK_CALLS_PER_SEC;
}

bool BreakoutStrategy::canMakeCandleAPICall() const
{
    long long now = getCurrentTimestamp();
    
    int recent_calls = 0;
    for (auto it = api_call_timestamps_.rbegin(); it != api_call_timestamps_.rend(); ++it) {
        if (now - *it < 1000) {
            recent_calls++;
        } else {
            break;
        }
    }
    
    return recent_calls < MAX_CANDLE_CALLS_PER_SEC;
}

void BreakoutStrategy::recordAPICall() const
{
    long long now = getCurrentTimestamp();
    api_call_timestamps_.push_back(now);
    
    while (!api_call_timestamps_.empty() && (now - api_call_timestamps_.front()) > 5000) {
        api_call_timestamps_.pop_front();
    }
}

nlohmann::json BreakoutStrategy::getCachedOrderBook(const std::string& market)
{
    long long now = getCurrentTimestamp();
    
    if (!cached_orderbook_.empty() && (now - last_orderbook_fetch_time_) < ORDERBOOK_CACHE_MS) {
        return cached_orderbook_;
    }
    
    if (!canMakeOrderBookAPICall()) {
        return cached_orderbook_;
    }
    
    try {
        cached_orderbook_ = client_->getOrderBook(market);
        last_orderbook_fetch_time_ = now;
        recordAPICall();
    } catch (const std::exception& e) {
        spdlog::warn("[Breakout] Failed to fetch orderbook: {}", e.what());
    }
    
    return cached_orderbook_;
}

std::vector<Candle> BreakoutStrategy::getCachedCandles(
    const std::string& market,
    int count)
{
    long long now = getCurrentTimestamp();
    
    if (candle_cache_.count(market) && 
        (now - candle_cache_time_[market]) < CANDLE_CACHE_MS) {
        return candle_cache_[market];
    }
    
    if (!canMakeCandleAPICall()) {
        if (candle_cache_.count(market)) {
            return candle_cache_[market];
        }
        return std::vector<Candle>();
    }
    
    try {
        nlohmann::json json_data = client_->getCandles(market, "5", count);
        auto candles = parseCandlesFromJson(json_data);
        
        candle_cache_[market] = candles;
        candle_cache_time_[market] = now;
        recordAPICall();
        return candles;
    } catch (const std::exception& e) {
        spdlog::warn("[Breakout] Failed to fetch candles: {}", e.what());
        if (candle_cache_.count(market)) {
            return candle_cache_[market];
        }
        return std::vector<Candle>();
    }
}

// ===== Trade Frequency Management =====

bool BreakoutStrategy::canTradeNow()
{
    const auto strategy_cfg = Config::getInstance().getBreakoutConfig();
    resetDailyCounters();
    resetHourlyCounters();
    
    if (daily_trades_count_ >= strategy_cfg.max_daily_trades) {
        return false;
    }
    
    if (hourly_trades_count_ >= strategy_cfg.max_hourly_trades) {
        return false;
    }
    
    return true;
}

void BreakoutStrategy::recordTrade()
{
    daily_trades_count_++;
    hourly_trades_count_++;
    
    long long now = getCurrentTimestamp();
    trade_timestamps_.push_back(now);
    
    if (trade_timestamps_.size() > 100) {
        trade_timestamps_.pop_front();
    }
}

void BreakoutStrategy::resetDailyCounters()
{
    long long now = getCurrentTimestamp();
    long long day_ms = 24 * 60 * 60 * 1000;
    
    if (current_day_start_ == 0 || (now - current_day_start_) >= day_ms) {
        daily_trades_count_ = 0;
        current_day_start_ = now;
    }
}

void BreakoutStrategy::resetHourlyCounters()
{
    long long now = getCurrentTimestamp();
    long long hour_ms = 60 * 60 * 1000;
    
    if (current_hour_start_ == 0 || (now - current_hour_start_) >= hour_ms) {
        hourly_trades_count_ = 0;
        current_hour_start_ = now;
    }
}

// ===== Circuit Breaker =====

void BreakoutStrategy::checkCircuitBreaker()
{
    const auto strategy_cfg = Config::getInstance().getBreakoutConfig();
    long long now = getCurrentTimestamp();
    
    if (circuit_breaker_active_ && now >= circuit_breaker_until_) {
        circuit_breaker_active_ = false;
        spdlog::info("[Breakout] Circuit breaker deactivated");
    }
    
    if (consecutive_losses_ >= strategy_cfg.max_consecutive_losses && !circuit_breaker_active_) {
        activateCircuitBreaker();
    }
}

void BreakoutStrategy::activateCircuitBreaker()
{
    circuit_breaker_active_ = true;
    circuit_breaker_until_ = getCurrentTimestamp() + CIRCUIT_BREAKER_COOLDOWN_MS;
    
    spdlog::warn("[Breakout] Circuit breaker activated - {} consecutive losses", 
                 consecutive_losses_);
}

bool BreakoutStrategy::isCircuitBreakerActive() const
{
    return circuit_breaker_active_;
}

// ===== Analyze Breakout (핵심 분석) =====

BreakoutSignalMetrics BreakoutStrategy::analyzeBreakout(
    const std::string& market,
    const analytics::CoinMetrics& metrics,
    const std::vector<Candle>& candles,
    double current_price)
{
    BreakoutSignalMetrics signal;
    (void)market;
    
    // 1. Donchian Channel 계산
    DonchianChannel channel = calculateDonchianChannel(candles, DONCHIAN_PERIOD);
    
    // 가격이 상단 돌파?
    if (current_price <= channel.upper) {
        return signal;
    }
    
    // 2. ATR 체크
    double atr = calculateATR(candles, 14);
    if (atr <= 0) {
        return signal;
    }
    
    double breakout_distance = current_price - channel.upper;
    signal.atr_multiple = breakout_distance / atr;
    
    double min_atr_multiple = computeBreakoutAtrMultipleFloor(metrics);
    if (signal.atr_multiple < min_atr_multiple) {
        return signal;
    }
    
    // 3. 거래량 확인
    signal.volume_confirmation = calculateVolumeConfirmation(candles);
    double min_volume_confirmation = computeBreakoutVolumeConfirmFloor(metrics);
    if (signal.volume_confirmation < min_volume_confirmation) {
        return signal;
    }
    
    // 4. False Breakout 필터
    if (isFalseBreakout(current_price, channel.upper, candles)) {
        signal.false_breakout_probability = 0.8;
        return signal;
    }
    
    signal.false_breakout_probability = 0.2;
    
    // 5. 시장 구조 분석
    MarketStructureAnalysis structure = analyzeMarketStructure(candles);
    
    if (structure.downtrend) {
        signal.false_breakout_probability = 0.9;
        return signal;
    }
    
    // 6. 돌파 유형 결정
    if (structure.ranging && structure.consolidation_bars >= MIN_CONSOLIDATION_BARS) {
        signal.type = BreakoutType::CONSOLIDATION_BREAK;
        signal.strength += 0.15;
    } else if (isVolumeSpikeSignificant(metrics, candles)) {
        signal.type = BreakoutType::VOLUME_BREAKOUT;
        signal.strength += 0.10;
    } else if (structure.uptrend) {
        signal.type = BreakoutType::RESISTANCE_BREAK;
        signal.strength += 0.05;
    } else {
        signal.type = BreakoutType::DONCHIAN_BREAK;
    }
    
    // 7. Volume Profile
    VolumeProfileData volume_profile = calculateVolumeProfile(candles);
    
    // 8. 돌파 강도 계산
    double breakout_strength = calculateBreakoutStrength(
        current_price, channel.upper, channel, volume_profile);
    
    // 9. Order Flow 분석
    double order_flow_score = analyzeOrderFlowImbalance(metrics);

    double spread_penalty = 0.0;
    if (metrics.orderbook_snapshot.valid) {
        double normalized_spread = std::clamp(metrics.orderbook_snapshot.spread_pct / 0.003, 0.0, 1.0);
        spread_penalty = normalized_spread * 0.10;
    }
    
    // 10. 종합 점수 계산
    double total_score = 0.0;
    total_score += breakout_strength * 0.30;
    total_score += signal.volume_confirmation * 0.25;
    total_score += (1.0 - signal.false_breakout_probability) * 0.20;
    total_score += std::min(signal.atr_multiple / 3.0, 1.0) * 0.15;
    total_score += order_flow_score * 0.10;
    total_score -= spread_penalty;
    
    signal.strength = std::clamp(total_score, 0.0, 1.0);
    signal.is_valid = (signal.strength >= computeBreakoutMicroValidityFloor(metrics));
    
    return signal;
}

// ===== Calculate Donchian Channel =====

DonchianChannel BreakoutStrategy::calculateDonchianChannel(const std::vector<Candle>& candles, int period) const {
    DonchianChannel channel;
    if (candles.size() < static_cast<size_t>(period + 1)) return channel;

    double highest = -std::numeric_limits<double>::max();
    double lowest = std::numeric_limits<double>::max();
    
    // [수정] 가장 최근 'period'만큼의 데이터를 정확히 타겟팅 (마지막 캔들은 돌파 대상이므로 제외하고 그 전까지의 고점 탐색)
    size_t start_idx = candles.size() - period - 1;
    for (size_t i = start_idx; i < candles.size() - 1; i++) {
        highest = std::max(highest, candles[i].high);
        lowest = std::min(lowest, candles[i].low);
    }
    
    channel.upper = highest;
    channel.lower = lowest;
    channel.middle = (highest + lowest) / 2.0;
    return channel;
}

// ===== Calculate Support/Resistance =====

SupportResistanceLevels BreakoutStrategy::calculateSupportResistance(
    const std::vector<Candle>& candles) const
{
    SupportResistanceLevels sr;
    
    if (candles.empty()) {
        return sr;
    }
    
    const auto& last = candles.back();
    
    sr.pivot_point = (last.high + last.low + last.close) / 3.0;
    
    sr.r1 = 2 * sr.pivot_point - last.low;
    sr.s1 = 2 * sr.pivot_point - last.high;
    sr.r2 = sr.pivot_point + (last.high - last.low);
    sr.s2 = sr.pivot_point - (last.high - last.low);
    sr.r3 = last.high + 2 * (sr.pivot_point - last.low);
    sr.s3 = last.low - 2 * (last.high - sr.pivot_point);
    
    if (candles.size() >= 50) {
        double swing_high = -std::numeric_limits<double>::max();
        double swing_low = std::numeric_limits<double>::max();
        
        for (size_t i = candles.size() - 50; i < candles.size(); i++) {
            swing_high = std::max(swing_high, candles[i].high);
            swing_low = std::min(swing_low, candles[i].low);
        }
        
        sr.fibonacci_levels.push_back(swing_low + (swing_high - swing_low) * 0.236);
        sr.fibonacci_levels.push_back(swing_low + (swing_high - swing_low) * 0.382);
        sr.fibonacci_levels.push_back(swing_low + (swing_high - swing_low) * 0.500);
        sr.fibonacci_levels.push_back(swing_low + (swing_high - swing_low) * 0.618);
        sr.fibonacci_levels.push_back(swing_low + (swing_high - swing_low) * 0.786);
    }
    
    return sr;
}

// ===== Find Swing Highs =====

std::vector<double> BreakoutStrategy::findSwingHighs(
    const std::vector<Candle>& candles) const
{
    std::vector<double> swing_highs;
    
    if (candles.size() < 5) {
        return swing_highs;
    }
    
    for (size_t i = 2; i < candles.size() - 2; i++) {
        if (candles[i].high > candles[i-1].high && 
            candles[i].high > candles[i-2].high &&
            candles[i].high > candles[i+1].high && 
            candles[i].high > candles[i+2].high) {
            swing_highs.push_back(candles[i].high);
        }
    }
    
    return swing_highs;
}

// ===== Find Swing Lows =====

std::vector<double> BreakoutStrategy::findSwingLows(
    const std::vector<Candle>& candles) const
{
    std::vector<double> swing_lows;
    
    if (candles.size() < 5) {
        return swing_lows;
    }
    
    for (size_t i = 2; i < candles.size() - 2; i++) {
        if (candles[i].low < candles[i-1].low && 
            candles[i].low < candles[i-2].low &&
            candles[i].low < candles[i+1].low && 
            candles[i].low < candles[i+2].low) {
            swing_lows.push_back(candles[i].low);
        }
    }
    
    return swing_lows;
}

// ===== Calculate Volume Profile =====

VolumeProfileData BreakoutStrategy::calculateVolumeProfile(
    const std::vector<Candle>& candles) const
{
    VolumeProfileData profile;
    
    if (candles.empty()) {
        return profile;
    }
    
    double min_price = std::numeric_limits<double>::max();
    double max_price = -std::numeric_limits<double>::max();
    
    for (const auto& candle : candles) {
        min_price = std::min(min_price, candle.low);
        max_price = std::max(max_price, candle.high);
    }
    
    const int bins = 50;
    double bin_size = (max_price - min_price) / bins;
    
    if (bin_size <= 0) {
        return profile;
    }
    
    std::map<int, double> volume_by_bin;
    double total_volume = 0;
    
    for (const auto& candle : candles) {
        double avg_price = (candle.high + candle.low + candle.close) / 3.0;
        int bin = static_cast<int>((avg_price - min_price) / bin_size);
        bin = std::clamp(bin, 0, bins - 1);
        
        volume_by_bin[bin] += candle.volume;
        total_volume += candle.volume;
    }
    
    int poc_bin = 0;
    double max_volume = 0;
    
    for (const auto& [bin, volume] : volume_by_bin) {
        if (volume > max_volume) {
            max_volume = volume;
            poc_bin = bin;
        }
    }
    
    profile.poc = min_price + poc_bin * bin_size + bin_size / 2.0;
    
    double target_volume = total_volume * 0.70;
    double accumulated_volume = volume_by_bin[poc_bin];
    
    int lower_bin = poc_bin;
    int upper_bin = poc_bin;
    
    while (accumulated_volume < target_volume && (lower_bin > 0 || upper_bin < bins - 1)) {
        double lower_vol = (lower_bin > 0) ? volume_by_bin[lower_bin - 1] : 0;
        double upper_vol = (upper_bin < bins - 1) ? volume_by_bin[upper_bin + 1] : 0;
        
        if (lower_vol > upper_vol && lower_bin > 0) {
            lower_bin--;
            accumulated_volume += lower_vol;
        } else if (upper_bin < bins - 1) {
            upper_bin++;
            accumulated_volume += upper_vol;
        } else if (lower_bin > 0) {
            lower_bin--;
            accumulated_volume += lower_vol;
        } else {
            break;
        }
    }
    
    profile.value_area_low = min_price + lower_bin * bin_size;
    profile.value_area_high = min_price + (upper_bin + 1) * bin_size;
    profile.volume_at_price_score = max_volume / total_volume;
    
    return profile;
}

// ===== Analyze Market Structure =====

MarketStructureAnalysis BreakoutStrategy::analyzeMarketStructure(
    const std::vector<Candle>& candles) const
{
    MarketStructureAnalysis structure;
    
    if (candles.size() < 20) {
        return structure;
    }
    
    std::vector<double> swing_highs = findSwingHighs(candles);
    std::vector<double> swing_lows = findSwingLows(candles);
    
    if (swing_highs.size() >= 2 && swing_lows.size() >= 2) {
        bool hh = swing_highs[swing_highs.size() - 1] > swing_highs[swing_highs.size() - 2];
        bool hl = swing_lows[swing_lows.size() - 1] > swing_lows[swing_lows.size() - 2];
        structure.uptrend = hh && hl;
        
        bool lh = swing_highs[swing_highs.size() - 1] < swing_highs[swing_highs.size() - 2];
        bool ll = swing_lows[swing_lows.size() - 1] < swing_lows[swing_lows.size() - 2];
        structure.downtrend = lh && ll;
    }
    
    double range_pct = 0;
    structure.ranging = isConsolidating(candles, range_pct);
    structure.consolidation_range_pct = range_pct;
    
    if (structure.ranging) {
        size_t lookback = std::min(size_t(50), candles.size());
        double range_high = -std::numeric_limits<double>::max();
        double range_low = std::numeric_limits<double>::max();
        
        for (size_t i = candles.size() - lookback; i < candles.size(); i++) {
            range_high = std::max(range_high, candles[i].high);
            range_low = std::min(range_low, candles[i].low);
        }
        
        double range = range_high - range_low;
        int consolidation_count = 0;
        
        for (size_t i = candles.size() - lookback; i < candles.size(); i++) {
            double candle_range = candles[i].high - candles[i].low;
            if (candle_range < range * 0.3) {
                consolidation_count++;
            }
        }
        
        structure.consolidation_bars = consolidation_count;
    }
    
    if (structure.uptrend) {
        double price_change = (candles.back().close - candles[candles.size() - 20].close) 
                            / candles[candles.size() - 20].close;
        structure.swing_strength = std::min(1.0, price_change / 0.10);
    }
    
    return structure;
}

// ===== Is Consolidating =====

bool BreakoutStrategy::isConsolidating(
    const std::vector<Candle>& candles,
    double& range_pct) const
{
    if (candles.size() < MIN_CONSOLIDATION_BARS) {
        return false;
    }
    
    size_t lookback = std::min(size_t(30), candles.size());
    
    double high = -std::numeric_limits<double>::max();
    double low = std::numeric_limits<double>::max();
    
    for (size_t i = candles.size() - lookback; i < candles.size(); i++) {
        high = std::max(high, candles[i].high);
        low = std::min(low, candles[i].low);
    }
    
    range_pct = (high - low) / low;
    
    return range_pct < 0.05;
}

// ===== Is False Breakout =====

bool BreakoutStrategy::isFalseBreakout(
    double current_price,
    double breakout_level,
    const std::vector<Candle>& candles) const
{
    double breakout_pct = (current_price - breakout_level) / breakout_level;
    if (breakout_pct < 0.005) {
        return true;
    }
    
    if (candles.size() >= 2) {
        const auto& prev = candles[candles.size() - 2];
        const auto& curr = candles.back();
        
        if (prev.close > breakout_level && curr.close < breakout_level) {
            return true;
        }
    }
    
    int touch_count = 0;
    size_t lookback = std::min(size_t(50), candles.size());
    
    for (size_t i = candles.size() - lookback; i < candles.size() - 1; i++) {
        if (std::abs(candles[i].high - breakout_level) / breakout_level < 0.01) {
            touch_count++;
        }
    }
    
    if (touch_count >= 3) {
        return true;
    }
    
    return false;
}

// ===== Calculate Breakout Strength =====

double BreakoutStrategy::calculateBreakoutStrength(
    double current_price,
    double breakout_level,
    const DonchianChannel& channel,
    const VolumeProfileData& volume_profile) const
{
    double strength = 0.5;
    
    double distance = (current_price - breakout_level) / breakout_level;
    strength += std::min(0.25, distance / 0.02);
    
    double channel_width = channel.upper - channel.lower;
    if (channel_width > 0) {
        double channel_pct = (current_price - breakout_level) / channel_width;
        strength += std::min(0.15, channel_pct / 0.5);
    }
    
    if (current_price > volume_profile.poc) {
        strength += 0.10;
    }
    
    return std::clamp(strength, 0.0, 1.0);
}

// ===== Is Volume Spike Significant =====

bool BreakoutStrategy::isVolumeSpikeSignificant(
    const analytics::CoinMetrics& metrics,
    const std::vector<Candle>& candles) const
{
    (void)metrics;
    if (candles.size() < 20) {
        return false;
    }
    
    double recent_volume = candles.back().volume;
    
    double avg_volume = 0;
    for (size_t i = candles.size() - 20; i < candles.size() - 1; i++) {
        avg_volume += candles[i].volume;
    }
    avg_volume /= 19.0;
    
    return recent_volume > avg_volume * 1.7;
}

// ===== Calculate Volume Confirmation =====

double BreakoutStrategy::calculateVolumeConfirmation(const std::vector<Candle>& candles) const {
    if (candles.size() < 25) return 0.0;
    
    // [수정] 최근 3분 거래량 평균 (데이터의 맨 뒤)
    double recent_vol = 0;
    for (size_t i = candles.size() - 3; i < candles.size(); i++) {
        recent_vol += candles[i].volume;
    }
    recent_vol /= 3.0;
    
    // [수정] 직전 20분 거래량 평균
    double avg_vol = 0;
    for (size_t i = candles.size() - 23; i < candles.size() - 3; i++) {
        avg_vol += candles[i].volume;
    }
    avg_vol /= 20.0;
    
    return (avg_vol > 0) ? std::min(1.0, recent_vol / (avg_vol * 1.5)) : 0.0;
}

// ===== Calculate ATR =====

double BreakoutStrategy::calculateATR(
    const std::vector<Candle>& candles,
    int period) const
{
    if (candles.size() < static_cast<size_t>(period + 1)) {
        return 0.0;
    }
    
    std::vector<double> true_ranges;
    
    for (size_t i = 1; i < candles.size(); i++) {
        double high_low = candles[i].high - candles[i].low;
        double high_close = std::abs(candles[i].high - candles[i-1].close);
        double low_close = std::abs(candles[i].low - candles[i-1].close);
        
        double tr = std::max({high_low, high_close, low_close});
        true_ranges.push_back(tr);
    }
    
    double atr = 0;
    size_t start = true_ranges.size() > static_cast<size_t>(period) 
                 ? true_ranges.size() - period : 0;
    
    for (size_t i = start; i < true_ranges.size(); i++) {
        atr += true_ranges[i];
    }
    
    int count = static_cast<int>(true_ranges.size() - start);
    return count > 0 ? atr / count : 0.0;
}

// ===== Analyze Order Flow Imbalance =====

double BreakoutStrategy::analyzeOrderFlowImbalance(const analytics::CoinMetrics& metrics)
{
    try {
        nlohmann::json units;
        if (metrics.orderbook_units.is_array() && !metrics.orderbook_units.empty()) {
            units = metrics.orderbook_units;
        } else if (!metrics.market.empty()) {
            nlohmann::json orderbook = getCachedOrderBook(metrics.market);
            if (orderbook.is_array() && !orderbook.empty() && orderbook[0].contains("orderbook_units")) {
                units = orderbook[0]["orderbook_units"];
            } else if (orderbook.contains("orderbook_units")) {
                units = orderbook["orderbook_units"];
            }
        }

        if (!units.is_array() || units.empty()) {
            return 0.5;
        }
        
        double total_bid_volume = 0;
        double total_ask_volume = 0;
        
        for (const auto& unit : units) {
            total_bid_volume += unit.value("bid_size", 0.0);
            total_ask_volume += unit.value("ask_size", 0.0);
        }
        
        double total = total_bid_volume + total_ask_volume;
        if (total == 0) return 0.5;
        
        return total_bid_volume / total;
        
    } catch (const std::exception&) {
        return 0.5;
    }
}

// ===== Calculate Signal Strength =====

double BreakoutStrategy::calculateSignalStrength(
    const BreakoutSignalMetrics& metrics,
    const MarketStructureAnalysis& structure,
    const analytics::CoinMetrics& coin_metrics) const
{
    double strength = metrics.strength;
    
    if (structure.uptrend) {
        strength += 0.05;
    }
    
    if (structure.ranging) {
        strength += 0.10;
    }
    
    if (coin_metrics.liquidity_score > 70.0) {
        strength += 0.05;
    }
    
    return std::clamp(strength, 0.0, 1.0);
}

// ===== Calculate CVaR =====

double BreakoutStrategy::calculateBreakoutCVaR(
    double position_size,
    double volatility) const
{
    double z_score = 1.645;
    return position_size * volatility * z_score;
}

// ===== Kelly Fraction =====

double BreakoutStrategy::calculateKellyFraction(
    double win_rate,
    double avg_win,
    double avg_loss) const
{
    if (avg_loss == 0) return 0.0;
    
    double b = avg_win / avg_loss;
    double kelly = (win_rate * b - (1 - win_rate)) / b;
    
    return std::clamp(kelly, 0.0, 1.0);
}

// ===== Adjust For Volatility =====

double BreakoutStrategy::adjustForVolatility(
    double kelly_size,
    double volatility) const
{
    double vol_factor = 1.0 - std::min(volatility / 0.05, 0.5);
    return kelly_size * vol_factor;
}

// ===== Calculate Volatility =====

double BreakoutStrategy::calculateVolatility(const std::vector<Candle>& candles) const {
    if (candles.size() < 21) return 0.02;
    std::vector<double> returns;
    // [수정] 정방향(Ascending) 정렬 기준 현재 수익률 계산
    for (size_t i = candles.size() - 20; i < candles.size(); ++i) {
        double ret = (candles[i].close - candles[i-1].close) / candles[i-1].close;
        returns.push_back(ret);
    }
    return calculateStdDev(returns, calculateMean(returns));
}

// ===== Should Generate Breakout Signal =====

bool BreakoutStrategy::shouldGenerateBreakoutSignal(
    const BreakoutSignalMetrics& metrics) const
{
    if (!metrics.is_valid) {
        return false;
    }

    if (metrics.false_breakout_probability > FALSE_BREAKOUT_THRESHOLD) {
        return false;
    }

    if (metrics.volume_confirmation < 0.40) {
        return false;
    }
    
    return true;
}

// ===== Helpers =====

double BreakoutStrategy::calculateMean(const std::vector<double>& values) const
{
    if (values.empty()) return 0.0;
    return std::accumulate(values.begin(), values.end(), 0.0) / values.size();
}

double BreakoutStrategy::calculateStdDev(
    const std::vector<double>& values,
    double mean) const
{
    if (values.size() < 2) return 0.0;
    
    double sum_sq = 0.0;
    for (double val : values) {
        double diff = val - mean;
        sum_sq += diff * diff;
    }
    
    return std::sqrt(sum_sq / (values.size() - 1));
}

long long BreakoutStrategy::getCurrentTimestamp() const
{
    if (latest_market_timestamp_ms_ > 0) {
        return latest_market_timestamp_ms_;
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

std::vector<Candle> BreakoutStrategy::parseCandlesFromJson(
    const nlohmann::json& json_data) const
{
    return analytics::TechnicalIndicators::jsonToCandles(json_data);
}

std::vector<Candle> BreakoutStrategy::resampleTo5m(const std::vector<Candle>& candles_1m) const {
    if (candles_1m.size() < 5) return {};
    std::vector<Candle> candles_5m;
    for (size_t i = 0; i + 5 <= candles_1m.size(); i += 5) {
        Candle candle_5m;
        candle_5m.open = candles_1m[i].open;         // [수정] i가 가장 과거
        candle_5m.close = candles_1m[i + 4].close;   // [수정] i+4가 가장 최신
        candle_5m.high = candles_1m[i].high;
        candle_5m.low = candles_1m[i].low;
        candle_5m.volume = 0;
        for (size_t j = i; j < i + 5; ++j) {
            candle_5m.high = std::max(candle_5m.high, candles_1m[j].high);
            candle_5m.low = std::min(candle_5m.low, candles_1m[j].low);
            candle_5m.volume += candles_1m[j].volume;
        }
        candle_5m.timestamp = candles_1m[i].timestamp;
        candles_5m.push_back(candle_5m);
    }
    return candles_5m;
}

} // namespace strategy
} // namespace autolife
