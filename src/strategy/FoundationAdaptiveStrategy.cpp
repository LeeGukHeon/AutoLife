#include "strategy/FoundationAdaptiveStrategy.h"

#include "analytics/TechnicalIndicators.h"

#include <algorithm>
#include <chrono>
#include <cmath>

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
        return signal;
    }
    if (!shouldEnter(market, metrics, candles, current_price, regime)) {
        return signal;
    }

    const std::vector<double> closes = analytics::TechnicalIndicators::extractClosePrices(candles);
    if (closes.size() < 60) {
        return signal;
    }

    const double ema_fast = analytics::TechnicalIndicators::calculateEMA(closes, 12);
    const double ema_slow = analytics::TechnicalIndicators::calculateEMA(closes, 48);
    const double rsi = analytics::TechnicalIndicators::calculateRSI(closes, 14);
    const double atr = safeAtr(candles, current_price);
    const double atr_pct = atr / std::max(1e-9, current_price);

    const double risk_pct = clampRiskPctByRegime(regime.regime, atr_pct);
    const double reward_risk = targetRewardRiskByRegime(regime.regime);

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

    if (signal.position_size <= 0.0) {
        signal.type = SignalType::NONE;
        return signal;
    }

    signal.expected_return_pct = (signal.take_profit_2 - signal.entry_price) / signal.entry_price;
    signal.expected_risk_pct = (signal.entry_price - signal.stop_loss) / signal.entry_price;
    const double implied_win_prob = std::clamp(0.47 + (strength * 0.18), 0.44, 0.66);
    signal.expected_value =
        (implied_win_prob * signal.expected_return_pct) -
        ((1.0 - implied_win_prob) * signal.expected_risk_pct);

    signal.signal_filter = std::clamp(0.52 + (strength * 0.30), 0.55, 0.85);
    signal.buy_order_type = OrderTypePolicy::LIMIT_WITH_FALLBACK;
    signal.sell_order_type = OrderTypePolicy::LIMIT_WITH_FALLBACK;
    signal.max_retries = 2;
    signal.retry_wait_ms = 700;
    signal.reason = "foundation_adaptive_regime_entry";
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
    if (metrics.liquidity_score < 45.0 || metrics.volume_surge_ratio < 0.75) {
        return false;
    }

    const std::vector<double> closes = analytics::TechnicalIndicators::extractClosePrices(candles);
    if (closes.size() < 60) {
        return false;
    }

    const double ema_fast = analytics::TechnicalIndicators::calculateEMA(closes, 12);
    const double ema_slow = analytics::TechnicalIndicators::calculateEMA(closes, 48);
    const double rsi = analytics::TechnicalIndicators::calculateRSI(closes, 14);

    switch (regime.regime) {
        case analytics::MarketRegime::TRENDING_UP:
            return (current_price > ema_fast &&
                    ema_fast > ema_slow &&
                    rsi >= 45.0 && rsi <= 72.0 &&
                    metrics.order_book_imbalance > -0.12);

        case analytics::MarketRegime::RANGING: {
            const auto bb = analytics::TechnicalIndicators::calculateBollingerBands(closes, current_price, 20, 2.0);
            return (current_price <= bb.middle &&
                    rsi <= 42.0 &&
                    metrics.order_book_imbalance > -0.20);
        }

        case analytics::MarketRegime::TRENDING_DOWN:
        case analytics::MarketRegime::HIGH_VOLATILITY:
            return (rsi <= 32.0 &&
                    current_price > ema_fast &&
                    metrics.buy_pressure >= metrics.sell_pressure &&
                    metrics.liquidity_score >= 62.0 &&
                    metrics.volume_surge_ratio >= 1.10);

        case analytics::MarketRegime::UNKNOWN:
        default:
            return (current_price > ema_fast &&
                    rsi < 52.0 &&
                    metrics.order_book_imbalance > -0.10);
    }
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
    if (metrics.liquidity_score < 55.0) {
        max_notional *= 0.80;
    }
    if (metrics.volatility > 4.0) {
        max_notional *= 0.85;
    }
    const double min_notional = capital * 0.03;
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

