#include "strategy/ScalpingStrategy.h"
#include "analytics/TechnicalIndicators.h"
#include "common/Logger.h"
#include "common/Config.h"
#include <chrono>
#include <algorithm>
#include <numeric>
#include <cmath>

#undef max
#undef min

namespace autolife {
namespace strategy {
namespace {
double clamp01(double v) {
    return std::clamp(v, 0.0, 1.0);
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

bool isShortTermChoppyWhipsaw(const std::vector<Candle>& candles) {
    if (candles.size() < 10) {
        return false;
    }

    const size_t n = candles.size();
    const size_t start = n - 8;
    int flips = 0;
    int prev_sign = 0;
    for (size_t i = start + 1; i < n; ++i) {
        const double delta = candles[i].close - candles[i - 1].close;
        int sign = 0;
        if (delta > 0.0) sign = 1;
        else if (delta < 0.0) sign = -1;

        if (prev_sign != 0 && sign != 0 && sign != prev_sign) {
            flips++;
        }
        if (sign != 0) {
            prev_sign = sign;
        }
    }

    const double base_price = candles[start].close;
    if (base_price <= 0.0) {
        return false;
    }
    const double net_move_pct = std::abs(candles.back().close - base_price) / base_price;

    double body_ratio_sum = 0.0;
    int body_ratio_count = 0;
    for (size_t i = start; i < n; ++i) {
        const double range = std::max(1e-9, candles[i].high - candles[i].low);
        const double body = std::abs(candles[i].close - candles[i].open);
        body_ratio_sum += (body / range);
        body_ratio_count++;
    }
    const double avg_body_ratio = (body_ratio_count > 0)
        ? (body_ratio_sum / static_cast<double>(body_ratio_count))
        : 1.0;

    const bool tight_chop = (flips >= 3) && (net_move_pct < 0.012);
    const bool whipsaw_signature = (flips >= 4) && (net_move_pct < 0.020) && (avg_body_ratio < 0.48);
    return tight_chop || whipsaw_signature;
}

double computeScalpingAdaptiveStrengthFloor(
    const analytics::CoinMetrics& metrics,
    const analytics::RegimeAnalysis& regime
) {
    const double vol_t = clamp01(metrics.volatility / 6.0);
    const double liq_t = clamp01((70.0 - metrics.liquidity_score) / 70.0);
    const double pressure_total = std::max(1e-6, std::abs(metrics.buy_pressure) + std::abs(metrics.sell_pressure));
    const double sell_bias_t = clamp01((metrics.sell_pressure - metrics.buy_pressure) / pressure_total);
    const double book_imbalance_t = clamp01((-metrics.order_book_imbalance + 0.05) / 0.60);
    double floor = 0.41 + (vol_t * 0.09) + (liq_t * 0.07) + (sell_bias_t * 0.04) + (book_imbalance_t * 0.04);

    if (regime.regime == analytics::MarketRegime::HIGH_VOLATILITY) {
        floor += 0.04;
    } else if (regime.regime == analytics::MarketRegime::TRENDING_DOWN) {
        floor += 0.03;
    }
    return std::clamp(floor, 0.42, 0.64);
}

std::vector<Candle> aggregateCandlesByStep(const std::vector<Candle>& candles, size_t step) {
    const auto ordered = ensureChronologicalCandles(candles);
    if (step <= 1 || ordered.size() < step) {
        return ordered;
    }
    std::vector<Candle> out;
    out.reserve(ordered.size() / step);
    for (size_t i = 0; i + step <= ordered.size(); i += step) {
        Candle merged;
        merged.open = ordered[i].open;
        merged.close = ordered[i + step - 1].close;
        merged.high = ordered[i].high;
        merged.low = ordered[i].low;
        merged.volume = 0.0;
        merged.timestamp = ordered[i].timestamp;
        for (size_t j = i; j < i + step; ++j) {
            merged.high = std::max(merged.high, ordered[j].high);
            merged.low = std::min(merged.low, ordered[j].low);
            merged.volume += ordered[j].volume;
        }
        out.push_back(merged);
    }
    return out;
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
    return std::clamp((price_move * 16.0) + (step_bias * 0.50), -1.0, 1.0);
}

double computeScalpingHigherTfTrendBias(
    const analytics::CoinMetrics& metrics,
    const std::vector<Candle>& candles_1m
) {
    std::vector<Candle> candles_5m;
    auto tf_5m_it = metrics.candles_by_tf.find("5m");
    if (tf_5m_it != metrics.candles_by_tf.end() && tf_5m_it->second.size() >= 20) {
        candles_5m = ensureChronologicalCandles(tf_5m_it->second);
    } else {
        candles_5m = aggregateCandlesByStep(candles_1m, 5);
    }

    double bias_5m = 0.0;
    if (candles_5m.size() >= 20) {
        bias_5m = computeDirectionalBias(candles_5m, std::min<size_t>(20, candles_5m.size() - 1));
    }

    double bias_1h = 0.0;
    auto tf_1h_it = metrics.candles_by_tf.find("1h");
    if (tf_1h_it != metrics.candles_by_tf.end() && tf_1h_it->second.size() >= 10) {
        const auto candles_1h = ensureChronologicalCandles(tf_1h_it->second);
        bias_1h = computeDirectionalBias(candles_1h, std::min<size_t>(10, candles_1h.size() - 1));
    } else if (candles_5m.size() >= 37) {
        bias_1h = computeDirectionalBias(candles_5m, 36);
    }

    double combined = (bias_5m * 0.60) + (bias_1h * 0.40);
    if (metrics.order_book_imbalance < -0.15) {
        combined -= 0.08;
    } else if (metrics.order_book_imbalance > 0.15) {
        combined += 0.05;
    }
    return std::clamp(combined, -1.0, 1.0);
}

bool hasBullishImpulse(const std::vector<Candle>& candles) {
    if (candles.size() < 4) {
        return false;
    }
    const size_t n = candles.size();
    int bullish_count = 0;
    for (size_t i = n - 3; i < n; ++i) {
        if (candles[i].close > candles[i].open) {
            bullish_count++;
        }
    }
    if (bullish_count < 2) {
        return false;
    }
    return candles[n - 1].close > candles[n - 2].close &&
           candles[n - 2].close > candles[n - 3].close;
}

void adaptScalpingStopsByLiquidityVolatility(
    const analytics::CoinMetrics& metrics,
    double entry_price,
    ScalpingDynamicStops& stops
) {
    if (entry_price <= 0.0 || stops.stop_loss <= 0.0 || stops.stop_loss >= entry_price) {
        return;
    }

    const double vol_t = clamp01(metrics.volatility / 6.0);
    const double liq_t = clamp01((70.0 - metrics.liquidity_score) / 70.0);
    const double pressure_total = std::max(1e-6, std::abs(metrics.buy_pressure) + std::abs(metrics.sell_pressure));
    const double sell_bias_t = clamp01((metrics.sell_pressure - metrics.buy_pressure) / pressure_total);
    const double book_imbalance_t = clamp01((-metrics.order_book_imbalance + 0.05) / 0.60);
    const double flow_t = std::max(sell_bias_t, book_imbalance_t);
    const double widen = 1.0 + (vol_t * 0.35) + (liq_t * 0.20) + (flow_t * 0.20);
    const double reward_scale = std::clamp(1.0 - (vol_t * 0.20) - (liq_t * 0.15) - (flow_t * 0.10), 0.70, 1.05);

    const double base_risk = entry_price - stops.stop_loss;
    const double widened_risk = base_risk * widen;
    stops.stop_loss = entry_price - widened_risk;
    stops.take_profit_1 = entry_price + (stops.take_profit_1 - entry_price) * reward_scale;
    stops.take_profit_2 = entry_price + (stops.take_profit_2 - entry_price) * reward_scale;
    if (stops.take_profit_2 < stops.take_profit_1) {
        stops.take_profit_2 = stops.take_profit_1 * 1.001;
    }
}
}

// ===== Constructor & Basics =====

ScalpingStrategy::ScalpingStrategy(std::shared_ptr<network::UpbitHttpClient> client)
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
    LOG_INFO("Advanced Scalping Strategy initialized (Upbit API optimized)");
    
    stats_ = Statistics();
    rolling_stats_ = ScalpingRollingStatistics();
    stats_ = Statistics();
    rolling_stats_ = ScalpingRollingStatistics();
    
    // Initialize time counters
    current_day_start_ = getCurrentTimestamp();
    current_hour_start_ = getCurrentTimestamp();
}

StrategyInfo ScalpingStrategy::getInfo() const {
    StrategyInfo info;
    info.name = "Advanced Scalping";
    info.description = "Finance-engineering scalping strategy (Kelly, HMM, API optimized)";
    info.timeframe = "1m-3m";
    info.min_capital = 50000;
    info.expected_winrate = 0.65;
    info.risk_level = 7.0;
    return info;
}

Signal ScalpingStrategy::generateSignal(
    const std::string& market,
    const analytics::CoinMetrics& metrics,
    const std::vector<Candle>& candles,
    double current_price,
    double available_capital,
    const analytics::RegimeAnalysis& regime)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    Signal signal;
    signal.market = market;
    signal.strategy_name = "Advanced Scalping";
    signal.timestamp = getCurrentTimestamp();
    const auto tf_5m_it = metrics.candles_by_tf.find("5m");
    const auto tf_1h_it = metrics.candles_by_tf.find("1h");
    const bool has_preloaded_5m =
        (tf_5m_it != metrics.candles_by_tf.end() && tf_5m_it->second.size() >= 20);
    const bool has_preloaded_1h =
        (tf_1h_it != metrics.candles_by_tf.end() && tf_1h_it->second.size() >= 10);
    signal.used_preloaded_tf_5m = has_preloaded_5m;
    signal.used_preloaded_tf_1h = has_preloaded_1h;
    signal.used_resampled_tf_fallback = !has_preloaded_5m;
    
    // ===== Hard Gates (early return on rejection) =====
    // 1) Skip if already in position for this market
    if (active_positions_.find(market) != active_positions_.end()) {
        return signal;
    }
    
    // 2) Circuit breaker protection
    checkCircuitBreaker();
    if (isCircuitBreakerActive()) {
        return signal;
    }
    
    // 3) Respect signal interval limit
    if (!canTradeNow()) {
        return signal;
    }

    // 3.5) Loss-cluster cooldown: suppress re-entry when losses are clustering.
    const long long now_ts = getCurrentTimestamp();
    if (consecutive_losses_ >= 2) {
        long long cooldown_ms = 8LL * 60LL * 1000LL; // 8 min
        if (regime.regime == analytics::MarketRegime::RANGING ||
            regime.regime == analytics::MarketRegime::HIGH_VOLATILITY) {
            cooldown_ms = 15LL * 60LL * 1000LL; // 15 min
        }
        if ((now_ts - last_signal_time_) < cooldown_ms) {
            return signal;
        }
    }
    if (recent_returns_.size() >= 5) {
        int losses_recent = 0;
        double pnl_recent = 0.0;
        for (size_t i = recent_returns_.size() - 5; i < recent_returns_.size(); ++i) {
            const double pnl = recent_returns_[i];
            pnl_recent += pnl;
            if (pnl < 0.0) {
                losses_recent++;
            }
        }
        const double cluster_floor = -std::max(25.0, stats_.avg_loss * 2.0);
        if (losses_recent >= 4 && pnl_recent <= cluster_floor) {
            return signal;
        }
    }
    
    // 4) Minimum candle count
    if (candles.size() < 30) {
        return signal;
    }

    // 4.5) Avoid short-term whipsaw chop where scalping expectancy is structurally poor.
    if (isShortTermChoppyWhipsaw(candles)) {
        return signal;
    }

    const double higher_tf_trend_bias = computeScalpingHigherTfTrendBias(metrics, candles);
    const bool bullish_impulse = hasBullishImpulse(candles);
    const double pressure_total_gate =
        std::max(1e-6, std::abs(metrics.buy_pressure) + std::abs(metrics.sell_pressure));
    const double buy_pressure_bias_gate =
        (metrics.buy_pressure - metrics.sell_pressure) / pressure_total_gate;
    if (higher_tf_trend_bias < -0.22) {
        return signal;
    }
    if (higher_tf_trend_bias < -0.10 && !bullish_impulse) {
        return signal;
    }
    if (regime.regime == analytics::MarketRegime::TRENDING_DOWN &&
        higher_tf_trend_bias < 0.05) {
        return signal;
    }
    if (regime.regime == analytics::MarketRegime::RANGING ||
        regime.regime == analytics::MarketRegime::UNKNOWN) {
        // Pattern analysis: ranging/unknown scalping had persistent negative expectancy.
        return signal;
    }
    if (regime.regime == analytics::MarketRegime::TRENDING_UP) {
        const bool trend_up_strict_quality =
            higher_tf_trend_bias >= 0.28 &&
            bullish_impulse &&
            metrics.volume_surge_ratio >= 1.8 &&
            metrics.liquidity_score >= 70.0 &&
            metrics.order_book_imbalance >= -0.02;
        const bool trend_up_flow_quality =
            higher_tf_trend_bias >= 0.18 &&
            bullish_impulse &&
            metrics.volume_surge_ratio >= 1.45 &&
            metrics.liquidity_score >= 64.0 &&
            metrics.order_book_imbalance >= 0.02 &&
            buy_pressure_bias_gate >= 0.05;
        if (!trend_up_strict_quality && !trend_up_flow_quality) {
            return signal;
        }
        if (metrics.price_change_rate > 1.4 &&
            metrics.volume_surge_ratio < 2.5 &&
            (metrics.order_book_imbalance < 0.05 || buy_pressure_bias_gate < 0.08)) {
            return signal;
        }
        if (metrics.price_change_rate > 2.4 &&
            metrics.volume_surge_ratio < 3.0 &&
            buy_pressure_bias_gate < 0.05 &&
            metrics.order_book_imbalance < 0.0) {
            return signal;
        }
    }

    // 4.6) Regime-aware hard filters to suppress structurally weak entries.
    if (regime.regime == analytics::MarketRegime::RANGING) {
        const double abs_change = std::abs(metrics.price_change_rate);
        if (abs_change < 0.18 && metrics.volume_surge_ratio < 1.25) {
            return signal;
        }
        if (higher_tf_trend_bias < 0.0 && metrics.volume_surge_ratio < 1.5) {
            return signal;
        }
    }
    if (regime.regime == analytics::MarketRegime::HIGH_VOLATILITY) {
        if (metrics.liquidity_score < 58.0 || metrics.volume_surge_ratio < 1.45) {
            return signal;
        }
    }
    if (metrics.order_book_imbalance < -0.20 &&
        metrics.buy_pressure < metrics.sell_pressure &&
        metrics.volume_surge_ratio < 1.4) {
        return signal;
    }
    if (metrics.orderbook_snapshot.valid &&
        metrics.orderbook_snapshot.spread_pct > 0.0032 &&
        metrics.liquidity_score < 68.0) {
        return signal;
    }
    if (daily_trades_count_ >= 7 &&
        (regime.regime == analytics::MarketRegime::HIGH_VOLATILITY ||
         metrics.liquidity_score < 68.0 ||
         metrics.volume_surge_ratio < 1.3)) {
        return signal;
    }
    
    // 5) No available capital
    if (available_capital <= 0) {
        return signal;
    }
    
    LOG_INFO("{} - [Scalping] score-based analysis start", market);
    
    // ===== Score-Based Evaluation =====
    // Weighted category scores, clamped to [0.0, 1.0]
    double total_score = 0.0;
    
    // --- (1) 湲곗닠??吏???먯닔 (理쒕? 0.30) ---
    auto prices = analytics::TechnicalIndicators::extractClosePrices(candles);
    double rsi = analytics::TechnicalIndicators::calculateRSI(prices, 14);
    auto macd = analytics::TechnicalIndicators::calculateMACD(prices, 12, 26, 9);
    
    // RSI ?먯닔 (0.00 ~ 0.12)
    double rsi_score = 0.0;
    if (rsi >= 25 && rsi <= 45) rsi_score = 0.12;       // 怨쇰ℓ??諛섎벑 援ш컙
    else if (rsi > 45 && rsi <= 55) rsi_score = 0.08;    // 以묐┰
    else if (rsi > 55 && rsi <= 70) rsi_score = 0.10;    // 紐⑤찘? 援ш컙
    else if (rsi > 70) rsi_score = 0.03;                  // 怨쇰ℓ??(?꾪뿕)
    else rsi_score = 0.02;                                 // 洹밴낵留ㅻ룄
    total_score += rsi_score;
    
    // MACD ?먯닔 (0.00 ~ 0.10)
    double macd_score = 0.0;
    if (macd.histogram > 0) {
        macd_score = 0.10;  // ?묒쓽 ?덉뒪?좉렇??
    } else {
        // ?댁쟾 MACD 怨꾩궛
        std::vector<double> prev_prices(prices.begin(), prices.end() - 1);
        auto macd_prev = analytics::TechnicalIndicators::calculateMACD(prev_prices, 12, 26, 9);
        if (macd.histogram > macd_prev.histogram) {
            macd_score = 0.06; // ?섎씫 ?뷀솕 (?곸듅 ?꾪솚 以?
        } else {
            macd_score = 0.00; // ?섎씫 媛??
        }
    }
    total_score += macd_score;
    
    // 媛寃?蹂?숇쪧 ?먯닔 (0.00 ~ 0.08)
    double abs_change = std::abs(metrics.price_change_rate);
    double change_score = 0.0;
    if (abs_change >= 0.5) change_score = 0.08;
    else if (abs_change >= 0.2) change_score = 0.06;
    else if (abs_change >= 0.05) change_score = 0.04;
    else change_score = 0.01;
    total_score += change_score;
    
    // --- (2) 罹붾뱾 ?⑦꽩 ?먯닔 (理쒕? 0.15) ---
    size_t n = candles.size();
    const auto& last_candle = candles.back();
    int bullish_count = 0;
    size_t start_check = (n >= 3) ? n - 3 : 0;
    for (size_t i = start_check; i < n; ++i) {
        if (candles[i].close > candles[i].open) bullish_count++;
    }
    
    double pattern_score = 0.0;
    bool is_bullish = last_candle.close > last_candle.open;
    bool has_support = last_candle.low < std::min(last_candle.open, last_candle.close);
    
    if (bullish_count >= 2 && is_bullish) pattern_score = 0.15;
    else if (bullish_count >= 1 && is_bullish) pattern_score = 0.10;
    else if (has_support) pattern_score = 0.05;
    else pattern_score = 0.00;
    total_score += pattern_score;
    
    // --- (3) ?덉쭚 ?먯닔 (理쒕? 0.15) ---
    double regime_score = 0.0;
    switch (regime.regime) {
        case analytics::MarketRegime::TRENDING_UP:
            regime_score = 0.15; break;
        case analytics::MarketRegime::RANGING:
            regime_score = 0.10; break;  // ?ㅼ틮?묒뿉 ?곹빀
        case analytics::MarketRegime::HIGH_VOLATILITY:
            regime_score = 0.05; break;  // ?꾪뿕?섏?留?湲고쉶 ?덉쓬
        case analytics::MarketRegime::TRENDING_DOWN:
            regime_score = 0.00; break;  // ?쏀븳 ?섎꼸??(hard gate ?꾨떂!)
        default:
            regime_score = 0.05; break;
    }
    total_score += regime_score;
    
    // --- (4) 嫄곕옒???먯닔 (理쒕? 0.15) ---
    double volume_score = 0.0;
    if (metrics.volume_surge_ratio >= 3.0) volume_score = 0.15;       // 嫄곕옒????컻
    else if (metrics.volume_surge_ratio >= 1.5) volume_score = 0.10;  // 嫄곕옒???곸듅
    else if (metrics.volume_surge_ratio >= 1.0) volume_score = 0.06;  // ?됯퇏
    else volume_score = 0.02;
    total_score += volume_score;
    
    // --- (5) ?멸? & ?좊룞???먯닔 (理쒕? 0.15) ---
    double orderflow_score = 0.0;
    
    // ?멸? ?곗씠?곌? ?덉쑝硫?遺꾩꽍, ?놁쑝硫?以묐┰ ?먯닔
    auto order_flow = analyzeUltraFastOrderFlow(metrics, current_price);
    if (order_flow.microstructure_score > 0.6) orderflow_score = 0.15;
    else if (order_flow.microstructure_score > 0.3) orderflow_score = 0.10;
    else if (order_flow.microstructure_score > 0.0) orderflow_score = 0.05;
    else orderflow_score = 0.03; // ?멸? ?곗씠???놁뼱??理쒖냼 ?먯닔
    
    // ?좊룞??蹂대꼫??
    if (metrics.liquidity_score >= 70) orderflow_score = std::min(0.15, orderflow_score + 0.03);
    total_score += orderflow_score;
    
    // --- (6) MTF ?먯닔 (理쒕? 0.10) ---
    auto mtf_signal = analyzeScalpingTimeframes(candles);
    double mtf_score = mtf_signal.alignment_score * 0.10;
    total_score += mtf_score;
    double mtf_5m_score = 0.0;
    if (has_preloaded_5m) {
        const auto candles_5m = ensureChronologicalCandles(tf_5m_it->second);
        if (candles_5m.size() >= 4) {
            const size_t start_5m = candles_5m.size() - 4;
            int bullish_5m = 0;
            for (size_t i = start_5m; i < candles_5m.size(); ++i) {
                if (candles_5m[i].close >= candles_5m[i].open) {
                    bullish_5m++;
                }
            }
            if (bullish_5m >= 3) {
                mtf_5m_score += 0.03;
            } else if (bullish_5m == 2) {
                mtf_5m_score += 0.015;
            }
            const double base_5m = candles_5m[start_5m].close;
            if (base_5m > 0.0) {
                const double move_5m = (candles_5m.back().close - base_5m) / base_5m;
                if (move_5m > 0.008) {
                    mtf_5m_score += 0.015;
                } else if (move_5m < -0.010) {
                    mtf_5m_score -= 0.03;
                }
            }
        }
    }
    total_score += mtf_5m_score;

    double trend_score = 0.0;
    if (higher_tf_trend_bias >= 0.30) trend_score = 0.12;
    else if (higher_tf_trend_bias >= 0.15) trend_score = 0.08;
    else if (higher_tf_trend_bias >= 0.05) trend_score = 0.04;
    else if (higher_tf_trend_bias <= -0.20) trend_score = -0.12;
    else if (higher_tf_trend_bias <= -0.08) trend_score = -0.06;
    total_score += trend_score;

    // Quality adjustment: reduce low-quality flow/liquidity combinations.
    const double flow_direction = std::clamp(metrics.order_book_imbalance, -1.0, 1.0);
    const double pressure_total = std::max(1e-6, std::abs(metrics.buy_pressure) + std::abs(metrics.sell_pressure));
    const double buy_pressure_bias = std::clamp((metrics.buy_pressure - metrics.sell_pressure) / pressure_total, -1.0, 1.0);
    const double liquidity_t = clamp01((metrics.liquidity_score - 55.0) / 35.0);
    const double flow_t = clamp01((flow_direction + 0.10) / 0.70);
    const double pressure_t = clamp01((buy_pressure_bias + 0.10) / 0.70);
    const double quality_factor = std::clamp(0.83 + (0.09 * liquidity_t) + (0.05 * flow_t) + (0.03 * pressure_t), 0.72, 1.03);
    total_score *= quality_factor;

    if (metrics.price_change_rate > 0.0 && flow_direction < -0.18) {
        total_score -= 0.04;
    }
    if (metrics.volume_surge_ratio < 1.05 && metrics.price_change_rate < 0.25) {
        total_score -= 0.02;
    }
    if (metrics.buy_pressure < metrics.sell_pressure && flow_direction < -0.15) {
        total_score -= 0.02;
    }
    if (higher_tf_trend_bias < -0.08) {
        total_score -= 0.03;
    } else if (higher_tf_trend_bias > 0.25 && bullish_impulse) {
        total_score += 0.02;
    }
    
    // ===== 理쒖쥌 ?좏샇 媛뺣룄 =====
    signal.strength = std::clamp(total_score, 0.0, 1.0);
    LOG_INFO("{} - [Scalping] total score: {:.3f} (RSI:{:.2f} MACD:{:.2f} Chg:{:.2f} Pat:{:.2f} Reg:{:.2f} Vol:{:.2f} OF:{:.2f} MTF:{:.2f} MTF5m:{:.2f} Trend:{:.2f} Bias:{:.2f})",
             market, signal.strength, rsi_score, macd_score, change_score, pattern_score, 
             regime_score, volume_score, orderflow_score, mtf_score, mtf_5m_score, trend_score, higher_tf_trend_bias);
    
    // ===== Adaptive strength gate =====
    const auto& strategy_cfg = Config::getInstance().getScalpingConfig();
    const double dynamic_strength_floor = computeScalpingAdaptiveStrengthFloor(metrics, regime);
    double effective_strength_floor = std::max(dynamic_strength_floor, strategy_cfg.min_signal_strength);
    if (order_flow.bid_ask_spread > 0.0 &&
        metrics.liquidity_score >= 72.0 &&
        order_flow.bid_ask_spread <= 0.05 &&
        regime.regime != analytics::MarketRegime::TRENDING_DOWN &&
        regime.regime != analytics::MarketRegime::HIGH_VOLATILITY) {
        // Prevent over-suppression in healthy markets: allow slightly lower trigger.
        effective_strength_floor = std::max(0.36, effective_strength_floor - 0.015);
    }
    if ((regime.regime == analytics::MarketRegime::TRENDING_UP ||
         regime.regime == analytics::MarketRegime::RANGING) &&
        metrics.liquidity_score >= 65.0 &&
        order_flow.bid_ask_spread > 0.0 &&
        order_flow.bid_ask_spread <= 0.08 &&
        metrics.volume_surge_ratio >= 1.15) {
        // Keep minimum sample flow in favorable regimes for tunability.
        effective_strength_floor = std::max(0.35, effective_strength_floor - 0.015);
    }
    if (higher_tf_trend_bias >= 0.25) {
        effective_strength_floor = std::max(0.33, effective_strength_floor - 0.02);
    } else if (higher_tf_trend_bias < 0.0) {
        effective_strength_floor = std::min(0.78, effective_strength_floor + 0.03);
    }
    if (regime.regime == analytics::MarketRegime::TRENDING_UP &&
        bullish_impulse &&
        higher_tf_trend_bias >= 0.18 &&
        metrics.liquidity_score >= 72.0 &&
        metrics.volume_surge_ratio >= 1.50 &&
        buy_pressure_bias_gate >= 0.04 &&
        metrics.order_book_imbalance >= 0.0) {
        // In clear bullish-flow conditions, avoid over-pruning mid-strength setups.
        effective_strength_floor = std::max(0.56, effective_strength_floor - 0.11);
    }
    if (higher_tf_trend_bias < -0.12) {
        effective_strength_floor = std::min(0.80, effective_strength_floor + 0.03);
    }
    if (signal.strength < effective_strength_floor) {
        LOG_INFO("{} - [Scalping] dynamic strength gate: {:.3f} < {:.3f} (dynamic {:.3f}, cfg {:.3f})",
                 market, signal.strength, effective_strength_floor,
                 dynamic_strength_floor, strategy_cfg.min_signal_strength);
        return signal;
    }
    
    // ===== Signal generation =====
    auto stops = calculateScalpingDynamicStops(current_price, candles);
    adaptScalpingStopsByLiquidityVolatility(metrics, current_price, stops);
    
    signal.type = SignalType::BUY;
    signal.entry_price = current_price;
    signal.stop_loss = stops.stop_loss;
    signal.take_profit_1 = stops.take_profit_1;
    signal.take_profit_2 = stops.take_profit_2;
    signal.breakeven_trigger = stops.breakeven_trigger;
    signal.trailing_start = stops.trailing_start;
    signal.buy_order_type = strategy::OrderTypePolicy::LIMIT_WITH_FALLBACK;
    signal.sell_order_type = strategy::OrderTypePolicy::LIMIT_WITH_FALLBACK;
    signal.max_retries = 3;
    signal.retry_wait_ms = 500;
    
    // Hard-gate weak edge before sizing.
    const auto& engine_cfg = Config::getInstance().getEngineConfig();
    const double expected_return = (signal.take_profit_2 - signal.entry_price) / signal.entry_price;
    const double fee_rate = Config::getInstance().getFeeRate();

    double observed_spread_pct = order_flow.bid_ask_spread;
    if (observed_spread_pct <= 0.0 && metrics.orderbook_snapshot.valid) {
        observed_spread_pct = metrics.orderbook_snapshot.spread_pct * 100.0;
    }
    if (observed_spread_pct > 0.0) {
        double spread_hard_limit = 0.24;
        if (regime.regime == analytics::MarketRegime::HIGH_VOLATILITY) {
            spread_hard_limit = 0.34;
        } else if (regime.regime == analytics::MarketRegime::TRENDING_DOWN) {
            spread_hard_limit = 0.22;
        }
        if (observed_spread_pct > spread_hard_limit) {
            LOG_INFO("{} - [Scalping] spread hard gate: {:.3f}% > {:.3f}%",
                     market, observed_spread_pct, spread_hard_limit);
            return signal;
        }
    }

    // Cost model: fee + slippage floor + spread-dependent execution penalty.
    const double spread_penalty = std::max(0.0, observed_spread_pct) / 100.0 * 0.7;
    const double slippage_assumption = std::clamp(engine_cfg.max_slippage_pct * 0.35, 0.0007, 0.0015);
    const double round_trip_cost = (fee_rate * 2.0) + (slippage_assumption * 2.0) + spread_penalty;

    double edge_multiplier = 1.07;
    if (regime.regime == analytics::MarketRegime::HIGH_VOLATILITY ||
        regime.regime == analytics::MarketRegime::TRENDING_DOWN) {
        edge_multiplier = 1.12;
    } else if (metrics.liquidity_score < 60.0) {
        edge_multiplier = 1.10;
    }
    if (signal.strength >= 0.72 &&
        metrics.liquidity_score >= 70.0 &&
        observed_spread_pct > 0.0 &&
        observed_spread_pct <= 0.07 &&
        order_flow.microstructure_score >= 0.40 &&
        regime.regime != analytics::MarketRegime::TRENDING_DOWN) {
        edge_multiplier = std::max(0.99, edge_multiplier - 0.03);
    }
    if (higher_tf_trend_bias < 0.0) {
        edge_multiplier = std::max(edge_multiplier, 1.13);
    }
    if (higher_tf_trend_bias < -0.15) {
        edge_multiplier = std::max(edge_multiplier, 1.18);
    } else if (higher_tf_trend_bias > 0.30) {
        edge_multiplier = std::max(0.96, edge_multiplier - 0.05);
    }

    const double confidence_bonus = std::max(0.0, signal.strength - 0.65) * 0.0006;
    double micro_quality_bonus = 0.0;
    if (metrics.liquidity_score >= 70.0 &&
        observed_spread_pct > 0.0 &&
        observed_spread_pct <= 0.08 &&
        order_flow.microstructure_score >= 0.50 &&
        regime.regime != analytics::MarketRegime::TRENDING_DOWN) {
        micro_quality_bonus = 0.00025;
    }
    if (higher_tf_trend_bias > 0.25 && bullish_impulse) {
        micro_quality_bonus += 0.00015;
    }
    const double adjusted_expected_return = expected_return + confidence_bonus;
    const double adjusted_expected_return_with_quality = adjusted_expected_return + micro_quality_bonus;

    if (adjusted_expected_return_with_quality <= round_trip_cost * edge_multiplier) {
        LOG_INFO("{} - [Scalping] edge gate: exp {:.4f} <= cost {:.4f} * {:.2f}",
                 market, adjusted_expected_return_with_quality, round_trip_cost, edge_multiplier);
        return signal;
    }
    const double stop_risk = std::max(1e-9, (signal.entry_price - signal.stop_loss) / signal.entry_price);
    const double reward_risk = expected_return / stop_risk;
    double rr_floor = std::max(1.25, strategy_cfg.min_risk_reward_ratio * 0.80);
    if (higher_tf_trend_bias < 0.0) {
        rr_floor = std::max(rr_floor, 1.40);
    }
    if (higher_tf_trend_bias < -0.15) {
        rr_floor = std::max(rr_floor, 1.55);
    } else if (higher_tf_trend_bias > 0.25) {
        rr_floor = std::max(1.15, rr_floor - 0.10);
    }
    if (reward_risk < rr_floor) {
        return signal;
    }
    
    // ?ъ????ъ씠吏?
    auto pos_metrics = calculateScalpingPositionSize(
        available_capital, signal.entry_price, signal.stop_loss, metrics, candles
    );
    
    {
        const double vol_t = clamp01(metrics.volatility / 6.0);
        const double liq_t = clamp01((70.0 - metrics.liquidity_score) / 70.0);
        const double pressure_total_pos = std::max(1e-6, std::abs(metrics.buy_pressure) + std::abs(metrics.sell_pressure));
        const double sell_bias_t = clamp01((metrics.sell_pressure - metrics.buy_pressure) / pressure_total_pos);
        const double book_imbalance_t = clamp01((-metrics.order_book_imbalance + 0.05) / 0.60);
        const double flow_penalty_t = std::max(sell_bias_t, book_imbalance_t);
        double adaptive_scale = std::clamp(1.0 - (vol_t * 0.40) - (liq_t * 0.35) - (flow_penalty_t * 0.25), 0.35, 1.0);
        if (higher_tf_trend_bias < -0.10) {
            adaptive_scale *= 0.80;
        } else if (higher_tf_trend_bias < 0.0) {
            adaptive_scale *= 0.88;
        } else if (higher_tf_trend_bias > 0.25) {
            adaptive_scale *= 1.08;
        }
        adaptive_scale = std::clamp(adaptive_scale, 0.28, 1.05);
        pos_metrics.final_position_size *= adaptive_scale;
    }
    const double min_order_krw = std::max(5000.0, engine_cfg.min_order_krw);
    if (available_capital < min_order_krw) {
        signal.type = SignalType::NONE;
        return signal;
    }
    // Budget-aware lot sizing: quantize to minimum-order lots for small accounts.
    const double raw_size = std::clamp(pos_metrics.final_position_size, 0.0, 1.0);
    double desired_order_krw = available_capital * raw_size;
    const double fee_reserve = std::clamp(engine_cfg.order_fee_reserve_pct, 0.0, 0.02);
    const double spendable_capital = available_capital / (1.0 + fee_reserve);
    double max_order_krw = std::min(engine_cfg.max_order_krw, spendable_capital);
    if (available_capital <= engine_cfg.small_account_tier1_capital_krw) {
        const double tier_cap = std::clamp(engine_cfg.small_account_tier1_max_order_pct, 0.01, 1.0);
        max_order_krw = std::min(max_order_krw, std::max(min_order_krw, available_capital * tier_cap));
    } else if (available_capital <= engine_cfg.small_account_tier2_capital_krw) {
        const double tier_cap = std::clamp(engine_cfg.small_account_tier2_max_order_pct, 0.01, 1.0);
        max_order_krw = std::min(max_order_krw, std::max(min_order_krw, available_capital * tier_cap));
    }

    const int max_lots = std::max(1, static_cast<int>(std::floor(max_order_krw / min_order_krw)));
    int desired_lots = static_cast<int>(std::floor(desired_order_krw / min_order_krw));
    desired_lots = std::clamp(desired_lots, 1, max_lots);
    double order_amount = static_cast<double>(desired_lots) * min_order_krw;
    signal.position_size = std::clamp(order_amount / available_capital, 0.0, 1.0);
    
    // Upgrade to strong buy on high score
    if (signal.strength >= 0.70) {
        signal.type = SignalType::STRONG_BUY;
    }
    
    signal.reason = fmt::format(
        "Scalping: Score={:.0f}% Regime={} RSI={:.0f} Vol={:.1f}x Trend={:.2f}",
        signal.strength * 100,
        static_cast<int>(regime.regime),
        rsi,
        metrics.volume_surge_ratio,
        higher_tf_trend_bias
    );
    
    last_signal_time_ = getCurrentTimestamp();
    
    LOG_INFO("[Scalping] buy signal: {} - strength {:.2f}, size {:.1f}%, order {:.0f} KRW",
             market, signal.strength, signal.position_size * 100, order_amount);
    
    return signal;
}

bool ScalpingStrategy::shouldEnter(
    const std::string& market,
    const analytics::CoinMetrics& metrics,
    const std::vector<Candle>& candles,
    double current_price,
    const analytics::RegimeAnalysis& regime
) {
    (void)current_price;
    
    if (candles.size() < 30) return false;

    const double higher_tf_trend_bias = computeScalpingHigherTfTrendBias(metrics, candles);
    const double pressure_total_gate =
        std::max(1e-6, std::abs(metrics.buy_pressure) + std::abs(metrics.sell_pressure));
    const double buy_pressure_bias_gate =
        (metrics.buy_pressure - metrics.sell_pressure) / pressure_total_gate;
    if (higher_tf_trend_bias < -0.22) {
        return false;
    }
    if (higher_tf_trend_bias < -0.10 && !hasBullishImpulse(candles)) {
        return false;
    }
    if (regime.regime == analytics::MarketRegime::TRENDING_DOWN &&
        higher_tf_trend_bias < 0.05) {
        return false;
    }
    if (regime.regime == analytics::MarketRegime::RANGING ||
        regime.regime == analytics::MarketRegime::UNKNOWN) {
        return false;
    }
    if (regime.regime == analytics::MarketRegime::TRENDING_UP) {
        const bool trend_up_strict_quality =
            higher_tf_trend_bias >= 0.28 &&
            hasBullishImpulse(candles) &&
            metrics.volume_surge_ratio >= 1.8 &&
            metrics.liquidity_score >= 70.0 &&
            metrics.order_book_imbalance >= -0.02;
        const bool trend_up_flow_quality =
            higher_tf_trend_bias >= 0.18 &&
            hasBullishImpulse(candles) &&
            metrics.volume_surge_ratio >= 1.45 &&
            metrics.liquidity_score >= 64.0 &&
            metrics.order_book_imbalance >= 0.02 &&
            buy_pressure_bias_gate >= 0.05;
        if (!trend_up_strict_quality && !trend_up_flow_quality) {
            return false;
        }
        if (metrics.price_change_rate > 1.4 &&
            metrics.volume_surge_ratio < 2.5 &&
            (metrics.order_book_imbalance < 0.05 || buy_pressure_bias_gate < 0.08)) {
            return false;
        }
        if (metrics.price_change_rate > 2.4 &&
            metrics.volume_surge_ratio < 3.0 &&
            buy_pressure_bias_gate < 0.05 &&
            metrics.order_book_imbalance < 0.0) {
            return false;
        }
    }
    
    double dynamic_liquidity_score = 50.0;
    // 1) Volume spike check (or minimum liquidity fallback)
    bool is_spike = isVolumeSpikeSignificant(metrics, candles);
    if (!is_spike && metrics.liquidity_score < 30.0) {
        LOG_INFO("{} - stage 1 rejected (Spike: {}, LiqScore: {:.1f})",
             market, is_spike, metrics.liquidity_score);
        // Reject when both spike and liquidity are weak
        return false;
    }
    
    // 2) RSI range check
    auto prices = analytics::TechnicalIndicators::extractClosePrices(candles);
    double rsi = analytics::TechnicalIndicators::calculateRSI(prices, 14);
    
    // Use configurable RSI bounds
    auto config = Config::getInstance().getScalpingConfig();
    if (rsi < config.rsi_lower || rsi > config.rsi_upper) {
        LOG_INFO("{} - RSI out of range: {:.1f} (target: {}-{})",
                 market, rsi, config.rsi_lower, config.rsi_upper);
        return false;
    }
    
    // 3) MACD check (positive or rising histogram)
    auto macd_current = analytics::TechnicalIndicators::calculateMACD(prices, 12, 26, 9);
    // Previous MACD using candles excluding latest close
    std::vector<double> prev_prices(prices.begin(), prices.end() - 1);
    auto macd_prev = analytics::TechnicalIndicators::calculateMACD(prev_prices, 12, 26, 9);

    double current_hist = macd_current.histogram;
    double prev_hist = macd_prev.histogram;

    // Allow pass if histogram is positive or improving
    bool is_macd_positive = current_hist > 0;
    bool is_macd_rising = current_hist > prev_hist; 

    if (!is_macd_positive && !is_macd_rising) {
        LOG_INFO("{} - MACD filter rejected (Hist: {:.4f}, Prev: {:.4f})", market, current_hist, prev_hist);
        return false;
    }
    
    // 4) Minimum price movement check
    double abs_change = std::abs(metrics.price_change_rate);
    if (abs_change < 0.05) {
        LOG_INFO("{} - volatility too low ({:.2f}%)", market, abs_change);
        return false;
    }
    if (regime.regime == analytics::MarketRegime::RANGING &&
        higher_tf_trend_bias < 0.0 &&
        metrics.volume_surge_ratio < 1.5) {
        return false;
    }
    
    // 5) Rebound / support validation
    size_t n = candles.size();
    const auto& last_candle = candles.back();

    int bullish_count = 0;
    size_t start_check = (n >= 3) ? n - 3 : 0;
    for (size_t i = start_check; i < n; ++i) {
        if (candles[i].close > candles[i].open) bullish_count++;
    }

    LOG_INFO("{} - last candle O:{}, C:{}, L:{}, Bullish: {}/3",
             market, last_candle.open, last_candle.close, last_candle.low, bullish_count);

    bool is_bullish = last_candle.close > last_candle.open;
    // Slightly wider flat-band tolerance for noisy candles
    bool is_flat = std::abs(last_candle.close - last_candle.open) <= (last_candle.open * 0.0007); 
    bool has_support = last_candle.low < std::min(last_candle.open, last_candle.close); 

    bool pass_rebound = false;

    // Case 1: at least one bullish candle in recent three
    if (bullish_count >= 1) {
        if (is_bullish || is_flat) pass_rebound = true;
        else if (has_support && (last_candle.close >= last_candle.open * 0.999)) pass_rebound = true; 
    } 
    // Case 2: no bullish candle but flat-with-support pattern
    else if (has_support && is_flat) {
        // Treat as early rebound if sell pressure is absorbed near support
        pass_rebound = true;
    }

    if (!pass_rebound) {
        LOG_INFO("{} - rebound validation failed (Bullish: {}/3, Flat: {}, Support: {})",
                 market, bullish_count, is_flat, has_support);
        return false;
    }
    
    // 6) Liquidity floor
    if (metrics.liquidity_score < dynamic_liquidity_score) {
        return false;
    }
    
    LOG_INFO("{} - scalping entry conditions passed (RSI: {:.1f}, change: {:.2f}%, trend_bias: {:.2f})",
              market, rsi, metrics.price_change_rate, higher_tf_trend_bias);
    
    return true;
}

bool ScalpingStrategy::shouldExit(
    const std::string& market,
    double entry_price,
    double current_price,
    double holding_time_seconds
) {
    (void)market;
    (void)entry_price;
    (void)current_price;

    // ?쒓컙 ?먯젅 (5遺?
    if (holding_time_seconds >= MAX_HOLDING_TIME) {
        return true;
    }
    
    return false;
}

double ScalpingStrategy::calculateStopLoss(
    double entry_price,
    const std::vector<Candle>& candles
) {
    auto stops = calculateScalpingDynamicStops(entry_price, candles);
    return stops.stop_loss;
}

double ScalpingStrategy::calculateTakeProfit(
    double entry_price,
    const std::vector<Candle>& candles
) {
    auto stops = calculateScalpingDynamicStops(entry_price, candles);
    return stops.take_profit_2;
}

double ScalpingStrategy::calculatePositionSize(
    double capital,
    double entry_price,
    double stop_loss,
    const analytics::CoinMetrics& metrics
) {
    std::vector<Candle> empty_candles;
    auto pos_metrics = calculateScalpingPositionSize(
        capital, entry_price, stop_loss, metrics, empty_candles
    );
    return pos_metrics.final_position_size;
}

void ScalpingStrategy::setEnabled(bool enabled) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    enabled_ = enabled;
}

bool ScalpingStrategy::isEnabled() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return enabled_;
}

IStrategy::Statistics ScalpingStrategy::getStatistics() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return stats_;
}

void ScalpingStrategy::setStatistics(const Statistics& stats) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    stats_ = stats;
}

bool ScalpingStrategy::onSignalAccepted(const Signal& signal, double allocated_capital) {
    (void)allocated_capital;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto inserted = active_positions_.insert(signal.market).second;
    if (inserted) {
        recordTrade();
    }
    return true;
}

void ScalpingStrategy::updateStatistics(const std::string& market, bool is_win, double profit_loss) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    // Remove market from active set when position is closed.
    if (active_positions_.erase(market)) {
        LOG_INFO("{} - scalping active-position flag cleared (position closed)", market);
    }
    
    // Existing statistics flow
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
    if (recent_returns_.size() > 500) {
        recent_returns_.pop_front();
    }
    
    trade_timestamps_.push_back(getCurrentTimestamp());
    if (trade_timestamps_.size() > 500) {
        trade_timestamps_.pop_front();
    }
    
    updateScalpingRollingStatistics();
    checkCircuitBreaker();
}

double ScalpingStrategy::updateTrailingStop(
    double entry_price,
    double highest_price,
    double current_price
) {
    double trailing_stop = highest_price * 0.99;
    
    if (trailing_stop >= current_price) {
        trailing_stop = entry_price * 0.99;
    }
    
    return trailing_stop;
}

bool ScalpingStrategy::shouldMoveToBreakeven(
    double entry_price,
    double current_price
) {
    return current_price >= entry_price * (1.0 + BREAKEVEN_TRIGGER);
}

ScalpingRollingStatistics ScalpingStrategy::getRollingStatistics() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return rolling_stats_;
}

// ===== API ?몄텧 愿由?(?낅퉬??Rate Limit 以?? =====

bool ScalpingStrategy::canMakeOrderBookAPICall() const {
    long long now = getCurrentTimestamp();
    
    // 1珥??대궡 ?몄텧 ?잛닔
    int calls_last_second = 0;
    for (auto ts : api_call_timestamps_) {
        if (now - ts < 1000) {
            calls_last_second++;
        }
    }
    
    return calls_last_second < MAX_ORDERBOOK_CALLS_PER_SEC;
}

bool ScalpingStrategy::canMakeCandleAPICall() const {
    long long now = getCurrentTimestamp();
    
    int calls_last_second = 0;
    for (auto ts : api_call_timestamps_) {
        if (now - ts < 1000) {
            calls_last_second++;
        }
    }
    
    return calls_last_second < MAX_CANDLE_CALLS_PER_SEC;
}

void ScalpingStrategy::recordAPICall() const {
    api_call_timestamps_.push_back(getCurrentTimestamp());
    
    // 10珥??댁긽 ??湲곕줉 ?쒓굅
    while (!api_call_timestamps_.empty() && 
           getCurrentTimestamp() - api_call_timestamps_.front() > 10000) {
        api_call_timestamps_.pop_front();
    }
}

nlohmann::json ScalpingStrategy::getCachedOrderBook(const std::string& market) {
    long long now = getCurrentTimestamp();
    
    // 罹먯떆 ?좏슚??泥댄겕
    if (now - last_orderbook_fetch_time_ < ORDERBOOK_CACHE_MS && 
        !cached_orderbook_.empty()) {
        return cached_orderbook_;
    }
    
    // Rate Limit 泥댄겕
    if (!canMakeOrderBookAPICall()) {
        LOG_WARN("OrderBook API Rate Limit ?꾨떖, 罹먯떆 ?ъ슜");
        return cached_orderbook_;
    }
    
    // ?덈줈 議고쉶
    try {
        recordAPICall();
        cached_orderbook_ = client_->getOrderBook(market);
        last_orderbook_fetch_time_ = now;
    } catch (const std::exception& e) {
        LOG_ERROR("OrderBook 議고쉶 ?ㅽ뙣: {}", e.what());
    }
    
    return cached_orderbook_;
}

std::vector<Candle> ScalpingStrategy::getCachedCandles(const std::string& market, int count) {
    long long now = getCurrentTimestamp();
    if (candle_cache_.find(market) != candle_cache_.end() && now - candle_cache_time_[market] < CANDLE_CACHE_MS) {
        return candle_cache_[market];
    }

    if (!canMakeCandleAPICall()) return candle_cache_.count(market) ? candle_cache_[market] : std::vector<Candle>();

    try {
        recordAPICall();
        auto candles_json = client_->getCandles(market, "1", count);
        auto candles = analytics::TechnicalIndicators::jsonToCandles(candles_json);
        candle_cache_[market] = candles;
        candle_cache_time_[market] = now;
        return candles;
    } catch (const std::exception& e) {
        LOG_ERROR("Candle 議고쉶 ?ㅽ뙣: {}", e.what());
        return candle_cache_.count(market) ? candle_cache_[market] : std::vector<Candle>();
    }
}

// ===== 嫄곕옒 鍮덈룄 愿由?=====

bool ScalpingStrategy::canTradeNow() {
    resetDailyCounters();
    resetHourlyCounters();
    
    auto config = Config::getInstance().getScalpingConfig();
    
    if (daily_trades_count_ >= config.max_daily_trades) {
        return false;
    }
    
    if (hourly_trades_count_ >= config.max_hourly_trades) {
        return false;
    }
    
    return true;
}

void ScalpingStrategy::recordTrade() {
    daily_trades_count_++;
    hourly_trades_count_++;
}

void ScalpingStrategy::resetDailyCounters() {
    long long now = getCurrentTimestamp();
    long long day_in_ms = 86400000;
    
    if (now - current_day_start_ >= day_in_ms) {
        daily_trades_count_ = 0;
        current_day_start_ = now;
        LOG_INFO("Daily scalping trade counter reset");
    }
}

void ScalpingStrategy::resetHourlyCounters() {
    long long now = getCurrentTimestamp();
    long long hour_in_ms = 3600000;
    
    if (now - current_hour_start_ >= hour_in_ms) {
        hourly_trades_count_ = 0;
        current_hour_start_ = now;
    }
}

// ===== Circuit Breaker =====

void ScalpingStrategy::checkCircuitBreaker() {
    auto config = Config::getInstance().getScalpingConfig();
    if (consecutive_losses_ >= config.max_consecutive_losses && !circuit_breaker_active_) {
        activateCircuitBreaker();
    }
    
    long long now = getCurrentTimestamp();
    if (circuit_breaker_active_ && now >= circuit_breaker_until_) {
        circuit_breaker_active_ = false;
        consecutive_losses_ = 0;
        LOG_INFO("Scalping circuit breaker released");
    }
}

void ScalpingStrategy::activateCircuitBreaker() {
    circuit_breaker_active_ = true;
    circuit_breaker_until_ = getCurrentTimestamp() + CIRCUIT_BREAKER_COOLDOWN_MS;
    LOG_WARN("Scalping circuit breaker activated: {} consecutive losses, 1h cooldown", consecutive_losses_);
}

bool ScalpingStrategy::isCircuitBreakerActive() const {
    return circuit_breaker_active_;
}

long long ScalpingStrategy::getCurrentTimestamp() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

// ===== 1. Market Microstate Detection (HMM) =====

// ===== HMM Removed =====

bool ScalpingStrategy::isVolumeSpikeSignificant(
    const analytics::CoinMetrics& metrics,
    const std::vector<Candle>& candles
) const {
    (void)metrics;

    if (candles.size() < 21) return false;
    
    // ?꾩옱(T=0, 留??? 嫄곕옒??
    double current_volume = candles.back().volume;
    
    // ?덉뒪?좊━: T-20 ~ T-1 (珥?20媛?
    std::vector<double> volumes;
    volumes.reserve(20);
    
    // candles.size() - 21 遺??candles.size() - 1 源뚯? (吏곸쟾 罹붾뱾源뚯?)
    for (size_t i = candles.size() - 21; i < candles.size() - 1; ++i) {
        volumes.push_back(candles[i].volume);
    }
    
    // volumes 踰≫꽣??援ъ꽦: [T-20, T-19, ..., T-2, T-1] (珥?20媛?
    
    // Z-Score (?꾩옱 嫄곕옒?됱씠 怨쇨굅 20媛??됯퇏 ?鍮??쇰쭏????덈굹)
    double z_score = calculateZScore(current_volume, volumes);
    
    if (z_score < 1.15) { 
        return false;
    }
    
    // T-Test: 理쒓렐 3遺?T-2, T-1, T-0) vs 怨쇨굅(T-20 ~ T-3)
    // T-0(?꾩옱)瑜??ы븿?댁빞 '吏湲???異붿꽭瑜??????덉쓬
    
    std::vector<double> recent_3;
    recent_3.push_back(candles[candles.size()-3].volume);
    recent_3.push_back(candles[candles.size()-2].volume);
    recent_3.push_back(candles[candles.size()-1].volume); // ?꾩옱
    
    std::vector<double> past_17; // T-20 ~ T-4 (珥?17媛?
    for(size_t i = candles.size()-20; i <= candles.size()-4; ++i) {
        past_17.push_back(candles[i].volume);
    }
    
    return isTTestSignificant(recent_3, past_17, 0.35);
}

double ScalpingStrategy::calculateZScore(
    double value,
    const std::vector<double>& history
) const {
    if (history.size() < 2) return 0.0;
    
    double mean = calculateMean(history);
    double std_dev = calculateStdDev(history, mean);
    
    if (std_dev < 0.0001) return 0.0;
    
    return (value - mean) / std_dev;
}

bool ScalpingStrategy::isTTestSignificant(
    const std::vector<double>& sample1,
    const std::vector<double>& sample2,
    double alpha
) const {
    (void)alpha;  // ??異붽?

    if (sample1.size() < 2 || sample2.size() < 2) return false;
    
    double mean1 = calculateMean(sample1);
    double mean2 = calculateMean(sample2);
    
    double std1 = calculateStdDev(sample1, mean1);
    double std2 = calculateStdDev(sample2, mean2);
    
    double n1 = static_cast<double>(sample1.size());
    double n2 = static_cast<double>(sample2.size());
    
    double pooled_std = std::sqrt(
        ((n1 - 1) * std1 * std1 + (n2 - 1) * std2 * std2) / (n1 + n2 - 2)
    );
    
    if (pooled_std < 0.0001) return false;
    
    double t_stat = (mean1 - mean2) / (pooled_std * std::sqrt(1.0/n1 + 1.0/n2));
    
    double t_critical = 1.645;  // alpha = 0.10
    
    return std::abs(t_stat) > t_critical;
}

// ===== 3. Multi-Timeframe Analysis (1m, 3m) =====

ScalpingMultiTimeframeSignal ScalpingStrategy::analyzeScalpingTimeframes(
    const std::vector<Candle>& candles_1m
) const {
    ScalpingMultiTimeframeSignal signal;
    if (candles_1m.size() < 30) return signal;

    // [湲곗〈 怨쇰ℓ??泥댄겕 ?좎??섎릺 ?⑸룄 蹂寃?
    signal.tf_1m_oversold = isOversoldOnTimeframe(candles_1m, signal.tf_1m);
    auto candles_3m = resampleTo3m(candles_1m);

    double score = 0.0;
    
    // 1. 1遺꾨큺 諛⑺뼢??泥댄겕 (醫낃?媛 ?쒓?蹂대떎 ?믨굅??RSI媛 以묐┰ ?댁긽)
    if (candles_1m.back().close >= candles_1m.back().open) score += 0.4;
    
    // 2. 3遺꾨큺 ?뺣젹 泥댄겕
    if (!candles_3m.empty()) {
        const auto& last_3m = candles_3m.back();
        // 3遺꾨큺???묐큺?닿굅?? 理쒖냼???섎씫??硫덉톬?ㅻ㈃ ?먯닔 遺??
        if (last_3m.close >= last_3m.open) score += 0.4;
        
        // 3. (蹂대꼫?? ?곸쐞 遺꾨큺?먯꽌 怨쇰ℓ?꾩??ㅺ? 諛섎벑 以묒씠硫???媛?곗젏
        signal.tf_3m_oversold = isOversoldOnTimeframe(candles_3m, signal.tf_3m);
        if (signal.tf_3m_oversold) score += 0.2;
    }

    signal.alignment_score = score;
    return signal;
}

std::vector<Candle> ScalpingStrategy::resampleTo3m(
    const std::vector<Candle>& candles_1m
) const {
    if (candles_1m.size() < 3) return {};
    
    std::vector<Candle> candles_3m;
    
    size_t n = candles_1m.size();
    // 3??諛곗닔濡?留욎텛湲??꾪빐 ?욌?遺꾩쓣 踰꾨┝ (理쒖떊 ?곗씠?곕? ?대━湲??꾪븿)
    size_t start_idx = n % 3; 
    
    for (size_t i = start_idx; i + 3 <= n; i += 3) {
        Candle candle_3m;
        
        // [?섏젙] 1遺꾨큺? ?쒓컙??Asc)?대?濡?
        // i: ?쒖옉(Open), i+2: ??Close)
        candle_3m.open = candles_1m[i].open;
        candle_3m.close = candles_1m[i + 2].close;
        candle_3m.high = candles_1m[i].high;
        candle_3m.low = candles_1m[i].low;
        candle_3m.volume = 0;
        
        for (size_t j = i; j < i + 3; ++j) {
            candle_3m.high = std::max(candle_3m.high, candles_1m[j].high);
            candle_3m.low = std::min(candle_3m.low, candles_1m[j].low);
            candle_3m.volume += candles_1m[j].volume;
        }
        
        // ??꾩뒪?ы봽???쒖옉 ?쒓컙 or ???쒓컙? 蹂댄넻 罹붾뱾 ?쒖옉 ?쒓컙???곷땲??
        candle_3m.timestamp = candles_1m[i].timestamp;
        candles_3m.push_back(candle_3m);
    }
    
    return candles_3m;
}

bool ScalpingStrategy::isOversoldOnTimeframe(
    const std::vector<Candle>& candles,
    ScalpingMultiTimeframeSignal::ScalpingTimeframeMetrics& metrics
) const {
    if (candles.size() < 14) return false;
    
    auto prices = analytics::TechnicalIndicators::extractClosePrices(candles);
    
    // 1. RSI 怨꾩궛 (媛??理쒖떊 prices.back()??湲곗?????
    metrics.rsi = analytics::TechnicalIndicators::calculateRSI(prices, 14);
    
    // 2. Stochastic RSI (?곕━媛 怨좎튇 理쒖떊 ?곗씠??湲곗? ?⑥닔 ?몄텧)
    auto stoch_result = analytics::TechnicalIndicators::calculateStochastic(candles, 14, 3);
    metrics.stoch_rsi = stoch_result.k;
    
    // 3. [?듭떖 ?섏젙] Instant Momentum (吏꾩쭨 理쒓렐 3媛?罹붾뱾)
    if (candles.size() >= 4) { // 3媛??꾩쓣 蹂대젮硫?理쒖냼 4媛쒓? ?덉쟾??
        // ?뺣젹???곗씠?곗뿉??媛??留덉?留됱씠 '吏湲?now)', ?ㅼ뿉??4踰덉㎏媛 '3媛???
        double price_now = candles.back().close;
        double price_3_ago = candles[candles.size() - 4].close;
        
        metrics.instant_momentum = (price_now - price_3_ago) / price_3_ago;
    }
    
    // 4. 怨쇰ℓ??議곌굔 (?ㅼ틮???뱁솕)
    // RSI媛 30~40 ?ъ씠?대㈃?? ?ㅽ넗罹먯뒪?깆씠 諛붾떏?닿굅??'吏湲??뱀옣' 媛寃⑹씠 怨좉컻瑜?????momentum > 0)
    bool rsi_oversold = metrics.rsi >= 30.0 && metrics.rsi <= 45.0; // 40? ?덈Т 鍮〓묀?댁꽌 45濡??댁쭩 ?꾪솕
    bool stoch_oversold = metrics.stoch_rsi < 25.0; // ?ㅽ넗罹먯뒪?깆? ???뺤떎??諛붾떏(25) ?뺤씤
    bool momentum_positive = metrics.instant_momentum > 0.0005; // 誘몄꽭??諛섎벑(0.05%) ?뺤씤
    
    return rsi_oversold && (stoch_oversold || momentum_positive);
}

// ===== 4. Ultra-Fast Order Flow =====

UltraFastOrderFlowMetrics ScalpingStrategy::analyzeUltraFastOrderFlow(
    const analytics::CoinMetrics& metrics,
    double current_price
) {
    (void)current_price;  // ??異붽?

    UltraFastOrderFlowMetrics flow;
    nlohmann::json units;

    try {
        if (metrics.orderbook_units.is_array() && !metrics.orderbook_units.empty()) {
            units = metrics.orderbook_units;
        } else if (!metrics.market.empty()) {
            auto orderbook = getCachedOrderBook(metrics.market);
            if (orderbook.is_array() && !orderbook.empty() && orderbook[0].contains("orderbook_units")) {
                units = orderbook[0]["orderbook_units"];
            } else if (orderbook.contains("orderbook_units")) {
                units = orderbook["orderbook_units"];
            }
        }

        if (!units.is_array() || units.empty()) {
            return flow;
        }
        
        double best_ask = units[0]["ask_price"].get<double>();
        double best_bid = units[0]["bid_price"].get<double>();
        flow.bid_ask_spread = (best_ask - best_bid) / best_bid * 100;
        
        // Instant Pressure (?곸쐞 3?덈꺼)
        double top3_bid = 0, top3_ask = 0;
        for (size_t i = 0; i < std::min(size_t(3), units.size()); ++i) {
            top3_bid += units[i]["bid_size"].get<double>();
            top3_ask += units[i]["ask_size"].get<double>();
        }
        
        if (top3_bid + top3_ask > 0) {
            flow.instant_pressure = (top3_bid - top3_ask) / (top3_bid + top3_ask);
        }
        
        // Order Flow Delta (?꾩껜)
        double total_bid = 0, total_ask = 0;
        for (const auto& unit : units) {
            total_bid += unit["bid_size"].get<double>();
            total_ask += unit["ask_size"].get<double>();
        }
        
        if (total_bid + total_ask > 0) {
            flow.order_flow_delta = (total_bid - total_ask) / (total_bid + total_ask);
        }
        
        // Tape Reading Score
        flow.tape_reading_score = calculateTapeReadingScore(units);
        
        // Micro Imbalance (?멸? 1-2?덈꺼 吏묒쨷??
        if (units.size() >= 2) {
            double level1_bid = units[0]["bid_size"].get<double>();
            double level2_bid = units[1]["bid_size"].get<double>();
            double level1_ask = units[0]["ask_size"].get<double>();
            double level2_ask = units[1]["ask_size"].get<double>();
            
            double level12_imbalance = ((level1_bid + level2_bid) - (level1_ask + level2_ask));
            if (total_bid + total_ask > 0) {
                flow.micro_imbalance = level12_imbalance / (total_bid + total_ask);
            }
        }
        
        // Microstructure Score 怨꾩궛
        double score = 0.0;
        
        // Spread (25%)
        if (flow.bid_ask_spread < 0.03) score += 0.25;
        else if (flow.bid_ask_spread < 0.05) score += 0.18;
        else score += 0.10;
        
        // Instant Pressure (30%)
        if (flow.instant_pressure > 0.2) score += 0.30;       // 0.4 -> 0.2
        else if (flow.instant_pressure > 0.0) score += 0.20;  // 0.2 -> 0.0
        else if (flow.instant_pressure > -0.2) score += 0.10; // ?섎씫 ?뺣젰???쏀빐???먯닔 遺??
        
        // Order Flow Delta (20%) - ?섏젙??
        if (flow.order_flow_delta > 0.1) score += 0.20;      // 0.2 -> 0.1
        else if (flow.order_flow_delta > -0.1) score += 0.12; // -0.1源뚯???'洹좏삎'?쇰줈 ?몄젙
        
        // Tape Reading (15%)
        score += flow.tape_reading_score * 0.15;
        
        // Micro Imbalance (10%)
        if (flow.micro_imbalance > 0.05) score += 0.10; // 0.1 -> 0.05
        else if (flow.micro_imbalance > -0.05) score += 0.05;
        
        flow.microstructure_score = score;
        
    } catch (const std::exception& e) {
        LOG_ERROR("Order Flow 遺꾩꽍 ?ㅽ뙣: {}", e.what());
    }
    
    return flow;
}

double ScalpingStrategy::calculateTapeReadingScore(
    const nlohmann::json& orderbook_units
) const {
    if (!orderbook_units.is_array() || orderbook_units.empty()) {
        return 0.0;
    }
    
    auto units = orderbook_units;
    if (units.size() < 5) return 0.0;
    
    // ?곸쐞 5?덈꺼???멸? 洹좏삎??
    double score = 0.0;
    
    // 1. 留ㅼ닔 ?멸?媛 ?먯쭊?곸쑝濡?利앷??섎뒗吏 (怨꾨떒??吏吏)
    bool bid_support = true;
    for (size_t i = 1; i < std::min(size_t(5), units.size()); ++i) {
        double current_bid = units[i]["bid_size"].get<double>();
        double prev_bid = units[i-1]["bid_size"].get<double>();
        
        if (current_bid < prev_bid * 0.5) {  // 湲됯꺽??媛먯냼
            bid_support = false;
            break;
        }
    }
    
    if (bid_support) score += 0.5;
    
    // 2. 留ㅻ룄 ?멸?媛 鍮덉빟?쒖? (????쏀븿)
    double avg_ask = 0;
    for (size_t i = 0; i < std::min(size_t(5), units.size()); ++i) {
        avg_ask += units[i]["ask_size"].get<double>();
    }
    avg_ask /= std::min(size_t(5), units.size());
    
    double avg_bid = 0;
    for (size_t i = 0; i < std::min(size_t(5), units.size()); ++i) {
        avg_bid += units[i]["bid_size"].get<double>();
    }
    avg_bid /= std::min(size_t(5), units.size());
    
    if (avg_bid > avg_ask * 1.2) {
        score += 0.5;
    } else if (avg_bid > avg_ask) {
        score += 0.3;
    }
    
    return std::min(1.0, score);
}

double ScalpingStrategy::calculateMomentumAcceleration(
    const std::vector<Candle>& candles
) const {
    // 1. 理쒖냼 ?곗씠???뺣낫 (理쒓렐 6媛?罹붾뱾 ?꾩슂)
    if (candles.size() < 6) return 0.0;
    
    // 2. [?듭떖 ?섏젙] ?몃뜳??湲곗???'?ㅼ뿉?쒕???濡?蹂寃?
    size_t n = candles.size();
    
    // 理쒓렐 紐⑤찘? (T-0 vs T-2) : 媛??理쒓렐 3媛?罹붾뱾??蹂?붿쑉
    // (?꾩옱媛 - 2遊됱쟾媛) / 2遊됱쟾媛
    double recent_now = candles[n - 1].close;
    double recent_past = candles[n - 3].close;
    double recent_momentum = (recent_now - recent_past) / recent_past;
    
    // ?댁쟾 紐⑤찘? (T-3 vs T-5) : 洹?吏곸쟾 3媛?罹붾뱾??蹂?붿쑉
    // (3遊됱쟾媛 - 5遊됱쟾媛) / 5遊됱쟾媛
    double prev_start = candles[n - 4].close;
    double prev_end = candles[n - 6].close;
    double prev_momentum = (prev_start - prev_end) / prev_end;
    
    // 3. 媛?띾룄 諛섑솚 (理쒓렐 ??- ?댁쟾 ??
    // 寃곌낵媛 ?묒닔?쇰㈃ ?곸듅 ?섏씠 ?먯젏 ???몄?怨??덈떎???살엯?덈떎.
    return recent_momentum - prev_momentum;
}
// ===== 5. Position Sizing (Kelly Criterion + Ultra-Short Vol) =====

ScalpingPositionMetrics ScalpingStrategy::calculateScalpingPositionSize(
    double capital,
    double entry_price,
    double stop_loss,
    const analytics::CoinMetrics& metrics,
    const std::vector<Candle>& candles
) const {
    ScalpingPositionMetrics pos_metrics;
    
    // 1. Kelly Fraction
    double win_rate = stats_.win_rate > 0 ? stats_.win_rate : 0.56;
    double avg_win = stats_.avg_profit > 0 ? stats_.avg_profit : 0.02;
    double avg_loss = stats_.avg_loss > 0 ? stats_.avg_loss : 0.01;
    
    pos_metrics.kelly_fraction = calculateKellyFraction(win_rate, avg_win, avg_loss);
    pos_metrics.half_kelly = pos_metrics.kelly_fraction * HALF_KELLY_FRACTION;
    
    // 2. 蹂?숈꽦 議곗젙 (珥덈떒???
    double volatility = 0.015;  // 湲곕낯 1.5%
    if (!candles.empty()) {
        volatility = calculateUltraShortVolatility(candles);
    }
    
    pos_metrics.volatility_adjusted = adjustForUltraShortVolatility(
        pos_metrics.half_kelly, volatility
    );
    
    // 3. ?좊룞??議곗젙
    double liquidity_factor = std::min(1.0, metrics.liquidity_score / 80.0);
    pos_metrics.final_position_size = pos_metrics.volatility_adjusted * liquidity_factor;
    
    // 4. ?ъ????쒗븳 諛?理쒖냼 湲덉븸 蹂댁젙 (理쒖쥌 ?섏젙蹂?

    // (1) ?쇰떒 ?ㅼ젙??理쒕? 鍮꾩쨷 ?쒗븳??癒쇱? 寃곷땲??
    pos_metrics.final_position_size = std::min(pos_metrics.final_position_size, MAX_POSITION_SIZE);
    
    // (2) ???꾩껜 ?먯궛蹂대떎 留롮씠 ???섎뒗 ?놁쑝誘濡?理쒖쥌 諛⑹뼱??
    if (pos_metrics.final_position_size > 1.0) {
        pos_metrics.final_position_size = 1.0;
    }
    
    // (3) ?낅퉬??理쒖냼 二쇰Ц 湲덉븸 ?섎쭔 ?좎슚
    const double min_order_krw = std::max(5000.0, Config::getInstance().getEngineConfig().min_order_krw);
    if (capital < min_order_krw) {
        pos_metrics.final_position_size = 0.0;
    }
    
    // 5. ?덉긽 Sharpe
    double risk = std::abs(entry_price - stop_loss) / entry_price;
    if (risk > 0.0001) {
        double reward = BASE_TAKE_PROFIT;
        pos_metrics.expected_sharpe = (reward - risk) / (volatility * std::sqrt(105120));
    }
    
    // 6. 理쒕? ?먯떎 湲덉븸
    pos_metrics.max_loss_amount = capital * pos_metrics.final_position_size * risk;
    
    return pos_metrics;
}

double ScalpingStrategy::calculateKellyFraction(
    double win_rate,
    double avg_win,
    double avg_loss
) const {
    if (avg_loss < 0.0001) return 0.01;
    
    double b = avg_win / avg_loss;
    double p = win_rate;
    double q = 1.0 - win_rate;
    
    double kelly = (p * b - q) / b;
    
    kelly = std::max(0.0, kelly);
    kelly = std::min(0.15, kelly);  // ?ㅼ틮?? 理쒕? 15%
    
    return kelly;
}

double ScalpingStrategy::adjustForUltraShortVolatility(
    double kelly_size,
    double volatility
) const {
    if (volatility < 0.0001) return kelly_size;
    
    double target_volatility = 0.015;  // 1.5%
    double vol_adjustment = target_volatility / volatility;
    
    vol_adjustment = std::clamp(vol_adjustment, 0.4, 1.5);
    
    return kelly_size * vol_adjustment;
}

// ===== 6. Dynamic Stops (Scalping) =====

ScalpingDynamicStops ScalpingStrategy::calculateScalpingDynamicStops(
    double entry_price,
    const std::vector<Candle>& candles
) const {
    ScalpingDynamicStops stops;
    
    if (candles.size() < 10) {
        stops.stop_loss = entry_price * (1.0 - BASE_STOP_LOSS);
        stops.take_profit_1 = entry_price * (1.0 + BASE_TAKE_PROFIT * 0.5);
        stops.take_profit_2 = entry_price * (1.0 + BASE_TAKE_PROFIT);
        stops.breakeven_trigger = entry_price * (1.0 + BREAKEVEN_TRIGGER);
        stops.trailing_start = entry_price * (1.0 + BASE_TAKE_PROFIT * 0.3);
        double min_tp1 = entry_price * (1.0 + MIN_TP1_PCT);
        double min_tp2 = entry_price * (1.0 + MIN_TP2_PCT);
        stops.take_profit_1 = std::max(stops.take_profit_1, min_tp1);
        stops.take_profit_2 = std::max(stops.take_profit_2, min_tp2);
        if (stops.take_profit_2 <= stops.take_profit_1) {
            stops.take_profit_2 = stops.take_profit_1 * 1.001;
        }
        return stops;
    }
    
    // 1. Micro ATR 湲곕컲 ?먯젅
    double micro_atr_stop = calculateMicroATRBasedStop(entry_price, candles);
    
    // 2. Hard Stop
    double hard_stop = entry_price * (1.0 - BASE_STOP_LOSS);
    
    // ?믪? ?먯젅???좏깮
    stops.stop_loss = std::max(hard_stop, micro_atr_stop);
    
    if (stops.stop_loss >= entry_price) {
        stops.stop_loss = hard_stop;
    }
    
    // 3. Take Profit (怨좎젙 鍮꾩쑉)
    double risk = entry_price - stops.stop_loss;
    double reward_ratio = 2.0;  // ?ㅼ틮?? 1:2
    
    stops.take_profit_1 = entry_price + (risk * reward_ratio * 0.5);  // 1% (50% 泥?궛)
    stops.take_profit_2 = entry_price + (risk * reward_ratio);         // 2% (?꾩껜 泥?궛)
    double min_tp1 = entry_price * (1.0 + MIN_TP1_PCT);
    double min_tp2 = entry_price * (1.0 + MIN_TP2_PCT);
    stops.take_profit_1 = std::max(stops.take_profit_1, min_tp1);
    stops.take_profit_2 = std::max(stops.take_profit_2, min_tp2);
    if (stops.take_profit_2 <= stops.take_profit_1) {
        stops.take_profit_2 = stops.take_profit_1 * 1.001;
    }
    
    // 4. Breakeven Trigger (1% ?섏씡??
    stops.breakeven_trigger = entry_price * (1.0 + BREAKEVEN_TRIGGER);
    
    // 5. Trailing Start
    stops.trailing_start = entry_price * (1.0 + BASE_TAKE_PROFIT * 0.5);
    
    return stops;
}

double ScalpingStrategy::calculateMicroATRBasedStop(
    double entry_price,
    const std::vector<Candle>& candles
) const {
    // 珥덈떒???吏㏃? ATR (5湲곌컙)
    double atr = analytics::TechnicalIndicators::calculateATR(candles, 5);
    
    if (atr < 0.0001) {
        return entry_price * (1.0 - BASE_STOP_LOSS);
    }
    
    double atr_percent = (atr / entry_price) * 100;
    
    double multiplier = 1.2;  // ?ㅼ틮?? ????댄듃
    if (atr_percent < 0.5) {
        multiplier = 1.0;
    } else if (atr_percent < 1.0) {
        multiplier = 1.2;
    } else {
        multiplier = 1.5;
    }
    
    double stop_loss = entry_price - (atr * multiplier);
    
    double min_stop = entry_price * (1.0 - BASE_STOP_LOSS * 1.5);
    double max_stop = entry_price * (1.0 - BASE_STOP_LOSS * 0.5);
    
    return std::clamp(stop_loss, min_stop, max_stop);
}

// ===== 7. Trade Cost Analysis =====

bool ScalpingStrategy::isWorthScalping(double expected_return, double expected_sharpe) const {
    // 1. 理쒖냼 鍮꾩슜 怨꾩궛 (?섏닔猷??щ━?쇱?) - ?닿굔 ?덈? ???紐삵븯???좎엯?덈떎.
    double total_cost = (UPBIT_FEE_RATE * 2) + (EXPECTED_SLIPPAGE * 2); 
    double net_return = expected_return - total_cost;

    // 2. ?쒖닔??湲곗?: 0.05% (5bp)
    // ?섏닔猷??쇨퀬 而ㅽ뵾媛믪씠?쇰룄 ?⑥쑝硫??쇰떒 '湲고쉶'?쇨퀬 遊낅땲??
    if (net_return < 0.0005) return false;

    // 3. ?ㅽ봽吏??湲곗?: 0.3 (?꾪솕)
    // 珥덈떒?(Scalping)??罹붾뱾 紐?媛쒕쭔 蹂닿퀬 ?ㅼ뼱媛湲??뚮Ц???ㅽ봽吏?섍? ?믨쾶 ?섏삤湲??대졄?듬땲??
    // 湲곗〈 0.5~1.0? ?덈Т 媛?뱁븯??0.3?쇰줈 ??떠蹂댁꽭??
    if (expected_sharpe < 0.3) return false; 

    // 4. ?먯씡鍮?R/R) 湲곗?: 0.8 (?꾪솕)
    // ?ㅼ틮?묒? ?밸쪧濡?癒밴퀬?щ뒗 嫄곗?, ?먯씡鍮꾨줈 癒밴퀬?щ뒗 寃??꾨떃?덈떎.
    // '癒뱀쓣 嫄?0.8 : ?껋쓣 嫄?1' ?뺣룄留??섏뼱???밸쪧??65%硫?臾댁“嫄??대뱷?낅땲??
    double actual_rr = expected_return / BASE_STOP_LOSS;
    if (actual_rr < 0.8) return false; 

    return true;
}

// ===== 8. Signal Strength =====

double ScalpingStrategy::calculateScalpingSignalStrength(
    const analytics::CoinMetrics& metrics, const std::vector<Candle>& candles,
    const ScalpingMultiTimeframeSignal& mtf_signal, const UltraFastOrderFlowMetrics& order_flow,
    MarketMicrostate microstate) const 
{
    double strength = 0.0;
    // 1. ?쒖옣 ?곹깭 ?먯닔
    if (microstate == MarketMicrostate::OVERSOLD_BOUNCE) strength += 0.20;
    else if (microstate == MarketMicrostate::MOMENTUM_SPIKE) strength += 0.20;
    else if (microstate == MarketMicrostate::BREAKOUT) strength += 0.15; 
    else if (microstate == MarketMicrostate::CONSOLIDATION) strength += 0.10;

    strength += mtf_signal.alignment_score * 0.15;

    // 2. ?ㅻ뜑?뚮줈??蹂대꼫??臾명꽦 ?꾪솕
    double of_weight = 0.30;
    double effective_of_score = order_flow.microstructure_score;
    // [?섏젙] 0.5 -> 0.3?쇰줈 ?꾪솕?섏뿬 ?됱떆?먮룄 媛以묒튂 遺??
    if (effective_of_score > 0.3) { 
        effective_of_score = std::min(1.0, effective_of_score + 0.10);
    }
    strength += effective_of_score * of_weight;

    // 3. RSI 援ш컙 ?섑븳???뺤옣 (?듭떖 ?섏젙)
    auto prices = analytics::TechnicalIndicators::extractClosePrices(candles);
    double rsi = analytics::TechnicalIndicators::calculateRSI(prices, 14);
    
    // [?섏젙] 25(李먮컮??遺???먯닔瑜?二쇰룄濡?踰붿쐞 ?뺤옣
    if (rsi >= 25 && rsi <= 45) strength += 0.15; 
    else if (rsi > 45 && rsi < 55) strength += 0.05;
    else if (rsi >= 55 && rsi <= 75) strength += 0.10;

    // 4. 嫄곕옒???먯닔
    if (metrics.volume_surge_ratio >= 150) strength += 0.15;
    else if (metrics.volume_surge_ratio >= 110) strength += 0.10; 
    else strength += 0.05;

    // 5. ?좊룞??蹂댁젙
    double liquidity_score = std::min(metrics.liquidity_score / 100.0, 1.0);
    strength += liquidity_score * 0.05;

    return std::clamp(strength, 0.0, 1.0);
}

// ===== 9. Risk Management (CVaR) =====

double ScalpingStrategy::calculateScalpingCVaR(
    double position_size,
    double volatility
) const {
    if (recent_returns_.empty() || volatility < 0.0001) {
        return position_size * 0.015;
    }
    
    std::vector<double> sorted_returns(recent_returns_.begin(), recent_returns_.end());
    std::sort(sorted_returns.begin(), sorted_returns.end());
    
    int var_index = static_cast<int>(sorted_returns.size() * 0.05);  // 95% ?좊ː援ш컙
    var_index = std::max(0, std::min(var_index, static_cast<int>(sorted_returns.size()) - 1));
    
    double sum_tail = 0.0;
    int tail_count = 0;
    
    for (int i = 0; i <= var_index; ++i) {
        sum_tail += std::abs(sorted_returns[i]);
        tail_count++;
    }
    
    double cvar = tail_count > 0 ? sum_tail / tail_count : 0.015;
    
    return position_size * cvar;
}

// ===== 10. Performance Metrics =====

double ScalpingStrategy::calculateScalpingSharpeRatio(
    int periods_per_year
) const {
    if (recent_returns_.size() < 10) {
        return 0.5;
    }
    
    std::vector<double> returns(recent_returns_.begin(), recent_returns_.end());
    double mean_return = calculateMean(returns);
    double std_dev = calculateStdDev(returns, mean_return);
    
    if (std_dev < 0.0001) return 0.0;
    
    double annualized_return = mean_return * periods_per_year;
    double annualized_vol = std_dev * std::sqrt(periods_per_year);
    
    double risk_free_rate = 0.03;
    
    return (annualized_return - risk_free_rate) / annualized_vol;
}

double ScalpingStrategy::calculateScalpingSortinoRatio(
    int periods_per_year
) const {
    if (recent_returns_.size() < 10) {
        return 0.0;
    }
    
    std::vector<double> returns(recent_returns_.begin(), recent_returns_.end());
    double mean_return = calculateMean(returns);
    
    std::vector<double> downside_returns;
    for (double ret : returns) {
        if (ret < 0) {
            downside_returns.push_back(ret);
        }
    }
    
    if (downside_returns.empty()) return 0.0;
    
    double downside_deviation = calculateStdDev(downside_returns, 0.0);
    
    if (downside_deviation < 0.0001) return 0.0;
    
    double annualized_return = mean_return * periods_per_year;
    double annualized_dd = downside_deviation * std::sqrt(periods_per_year);
    
    return annualized_return / annualized_dd;
}

void ScalpingStrategy::updateScalpingRollingStatistics() {
    if (recent_returns_.size() < 10) {
        return;
    }
    
    rolling_stats_.rolling_sharpe_1h = calculateScalpingSharpeRatio(105120);
    rolling_stats_.rolling_sharpe_24h = calculateScalpingSharpeRatio(105120);
    rolling_stats_.rolling_sortino_1h = calculateScalpingSortinoRatio(105120);
    
    int recent_trades = std::min(50, static_cast<int>(recent_returns_.size()));
    int wins = 0;
    for (int i = 0; i < recent_trades; ++i) {
        if (recent_returns_[i] > 0) wins++;
    }
    rolling_stats_.rolling_win_rate_50 = static_cast<double>(wins) / recent_trades;
    
    double total_profit = 0, total_loss = 0;
    for (int i = 0; i < recent_trades; ++i) {
        if (recent_returns_[i] > 0) {
            total_profit += recent_returns_[i];
        } else {
            total_loss += std::abs(recent_returns_[i]);
        }
    }
    
    if (total_loss > 0) {
        rolling_stats_.rolling_profit_factor = total_profit / total_loss;
    }
    
    // ?됯퇏 蹂댁쑀 ?쒓컙 (珥?
    if (!recent_holding_times_.empty()) {
        rolling_stats_.avg_holding_time_seconds = calculateMean(
            std::vector<double>(recent_holding_times_.begin(), recent_holding_times_.end())
        );
    }
}

// ===== 11. Helpers =====

double ScalpingStrategy::calculateMean(const std::vector<double>& values) const {
    if (values.empty()) return 0.0;
    return std::accumulate(values.begin(), values.end(), 0.0) / values.size();
}

double ScalpingStrategy::calculateStdDev(
    const std::vector<double>& values,
    double mean
) const {
    if (values.size() < 2) return 0.0;
    
    double variance = 0.0;
    for (double val : values) {
        variance += (val - mean) * (val - mean);
    }
    variance /= (values.size() - 1);
    
    return std::sqrt(variance);
}

double ScalpingStrategy::calculateUltraShortVolatility(const std::vector<Candle>& candles) const {
    if (candles.size() < 10) return 0.0;
    
    std::vector<double> returns;
    size_t start = candles.size() - 10;
    for (size_t i = start + 1; i < candles.size(); ++i) {
        // [?섏젙] (?꾩옱 - ?댁쟾) / ?댁쟾
        double ret = (candles[i].close - candles[i-1].close) / candles[i-1].close;
        returns.push_back(ret);
    }
    
    double mean = calculateMean(returns);
    return calculateStdDev(returns, mean);
}

bool ScalpingStrategy::shouldGenerateScalpingSignal(
    double expected_return,
    double expected_sharpe
) const {
    if (expected_return < 0.008) {  // 0.8% 誘몃쭔
        return false;
    }
    
    if (expected_sharpe < MIN_EXPECTED_SHARPE) {
        return false;
    }
    
    long long now = getCurrentTimestamp();
    if (now - last_signal_time_ < MIN_SIGNAL_INTERVAL_SEC * 1000) {
        return false;
    }
    
    return true;
}

// ===== ?ъ????곹깭 ?낅뜲?댄듃 (紐⑤땲?곕쭅 以? =====

void ScalpingStrategy::updateState(const std::string& market, double current_price) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    (void)market;
    (void)current_price;
    
    // ScalpingStrategy??珥덈떒??대?濡??ㅼ떆媛?異붿쟻????以묒슂
    // ?섏?留??꾩슂??留덉씠?щ줈?ㅽ뀒?댄듃 ?낅뜲?댄듃 媛??
   
    // [?좏깮?ы빆] 媛??理쒓렐 媛寃⑹쑝濡?留덉씠?щ줈?곹깭 ?ы룊媛
    // ?섏?留??ㅼ틮?묒씠誘濡?????쒓컙??吏㏃븘 ?遺遺?泥?궛 ?꾩뿉 ?좏샇 ?щ텇??????
}

} // namespace strategy
} // namespace autolife
