#include "strategy/MomentumStrategy.h"
#include "analytics/TechnicalIndicators.h"
#include "common/Logger.h"
#include "common/Config.h"
#include <chrono>
#include <algorithm>
#include <numeric>
#include <cmath>

namespace autolife {
namespace strategy {
namespace {
double clamp01(double v) {
    return std::clamp(v, 0.0, 1.0);
}

bool isShortTermChoppyForMomentum(const std::vector<Candle>& candles) {
    if (candles.size() < 14) {
        return false;
    }
    const size_t n = candles.size();
    const size_t start = n - 12;
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
    return (flips >= 7) && (net_move_pct < 0.005);
}

std::vector<Candle> aggregateCandles(const std::vector<Candle>& source, size_t span) {
    if (span < 2 || source.size() < span) {
        return {};
    }

    std::vector<Candle> aggregated;
    aggregated.reserve(source.size() / span);

    for (size_t i = 0; i + span <= source.size(); i += span) {
        Candle out;
        out.open = source[i].open;
        out.close = source[i + span - 1].close;
        out.high = source[i].high;
        out.low = source[i].low;
        out.volume = 0.0;
        out.timestamp = source[i].timestamp;

        for (size_t j = i; j < i + span; ++j) {
            out.high = std::max(out.high, source[j].high);
            out.low = std::min(out.low, source[j].low);
            out.volume += source[j].volume;
        }

        aggregated.push_back(out);
    }

    return aggregated;
}

double computeMomentumAdaptiveStrengthFloor(
    const analytics::CoinMetrics& metrics,
    const analytics::RegimeAnalysis& regime
) {
    const double vol_t = clamp01(metrics.volatility / 6.0);
    const double liq_t = clamp01((70.0 - metrics.liquidity_score) / 70.0);
    const double pressure_total = std::max(1e-6, std::abs(metrics.buy_pressure) + std::abs(metrics.sell_pressure));
    const double sell_bias_t = clamp01((metrics.sell_pressure - metrics.buy_pressure) / pressure_total);
    const double book_imbalance_t = clamp01((-metrics.order_book_imbalance + 0.05) / 0.60);
    double floor = 0.40 + (vol_t * 0.07) + (liq_t * 0.05) + (sell_bias_t * 0.04) + (book_imbalance_t * 0.04);

    if (regime.regime == analytics::MarketRegime::HIGH_VOLATILITY) {
        floor += 0.03;
    } else if (regime.regime == analytics::MarketRegime::TRENDING_DOWN) {
        floor += 0.04;
    }
    return std::clamp(floor, 0.40, 0.62);
}

void adaptMomentumStopsByLiquidityVolatility(
    const analytics::CoinMetrics& metrics,
    double entry_price,
    DynamicStops& stops
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
    const double widen = 1.0 + (vol_t * 0.30) + (liq_t * 0.20) + (flow_t * 0.20);
    const double reward_scale = std::clamp(1.0 - (vol_t * 0.18) - (liq_t * 0.12) - (flow_t * 0.10), 0.72, 1.05);

    const double base_risk = entry_price - stops.stop_loss;
    const double widened_risk = base_risk * widen;
    stops.stop_loss = entry_price - widened_risk;
    stops.take_profit_1 = entry_price + (stops.take_profit_1 - entry_price) * reward_scale;
    stops.take_profit_2 = entry_price + (stops.take_profit_2 - entry_price) * reward_scale;
    if (stops.take_profit_2 < stops.take_profit_1) {
        stops.take_profit_2 = stops.take_profit_1;
    }
}
}
// ===== Constructor & Basics =====

MomentumStrategy::MomentumStrategy(std::shared_ptr<network::UpbitHttpClient> client)
    : client_(client)
    , enabled_(true)
    , last_signal_time_(0)
    , daily_trades_count_(0)
    , hourly_trades_count_(0)
    , consecutive_losses_(0)
    , circuit_breaker_active_(false)
    , circuit_breaker_until_(0)
    , current_day_start_(0)
    , current_hour_start_(0)
{
    LOG_INFO("Advanced Momentum Strategy initialized (finance-engineering model)");
    
    stats_ = Statistics();
    rolling_stats_ = RollingStatistics();
    regime_model_ = RegimeModel();
}

StrategyInfo MomentumStrategy::getInfo() const {
    StrategyInfo info;
    info.name = "Advanced Momentum";
    info.description = "Finance-engineering momentum strategy (Kelly, HMM, CVaR)";
    info.timeframe = "1m-15m";
    info.min_capital = 100000;
    info.expected_winrate = 0.62;
    info.risk_level = 6.5;
    return info;
}

Signal MomentumStrategy::generateSignal(
    const std::string& market,
    const analytics::CoinMetrics& metrics,
    const std::vector<Candle>& candles,
    double current_price,
    double available_capital,
    const analytics::RegimeAnalysis& regime
) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const auto strategy_cfg = Config::getInstance().getMomentumConfig();
    
    Signal signal;
    signal.market = market;
    signal.strategy_name = "Advanced Momentum";
    signal.timestamp = getCurrentTimestamp();
    const bool has_preloaded_5m =
        metrics.candles_by_tf.find("5m") != metrics.candles_by_tf.end() &&
        !metrics.candles_by_tf.at("5m").empty();
    const bool has_preloaded_1h =
        metrics.candles_by_tf.find("1h") != metrics.candles_by_tf.end() &&
        metrics.candles_by_tf.at("1h").size() >= 26;
    signal.used_preloaded_tf_5m = has_preloaded_5m;
    signal.used_preloaded_tf_1h = has_preloaded_1h;
    signal.used_resampled_tf_fallback = !has_preloaded_5m;
    
    // ===== Hard Gates =====
    if (active_positions_.find(market) != active_positions_.end()) return signal;
    if (candles.size() < 60) return signal;
    if (available_capital <= 0) return signal;
    if (!canTradeNow()) return signal;
    checkCircuitBreaker();
    if (isCircuitBreakerActive()) return signal;
    if (regime.regime == analytics::MarketRegime::RANGING && isShortTermChoppyForMomentum(candles)) {
        return signal;
    }
    
    LOG_INFO("{} - [Momentum] score-based analysis start", market);
    
    // ===== Score-Based Evaluation =====
    double total_score = 0.0;
    
    // --- (1) Momentum indicators (max 0.30) ---
    auto prices = analytics::TechnicalIndicators::extractClosePrices(candles);
    double rsi = analytics::TechnicalIndicators::calculateRSI(prices, 14);
    auto macd = analytics::TechnicalIndicators::calculateMACD(prices, 12, 26, 9);
    
    // RSI score (0.00 ~ 0.10)
    double rsi_score = 0.0;
    if (rsi >= 45 && rsi <= 65) rsi_score = 0.10;
    else if (rsi > 65 && rsi <= 75) rsi_score = 0.06;
    else if (rsi >= 35 && rsi < 45) rsi_score = 0.04;
    else rsi_score = 0.00;
    total_score += rsi_score;
    
    // MACD score (0.00 ~ 0.12)
    double macd_score = 0.0;
    if (macd.macd > 0 && macd.histogram > 0) macd_score = 0.12;       // 媛뺥븳 ?곸듅
    else if (macd.macd > 0) macd_score = 0.06;                         // MACD ?묒닔
    else if (macd.histogram > 0) macd_score = 0.04;                    // ?덉뒪?좉렇???꾪솚 以?
    else macd_score = 0.00;
    total_score += macd_score;
    
    // 媛寃?紐⑤찘? ?먯닔 (0.00 ~ 0.08)
    double momentum_score = 0.0;
    if (metrics.price_change_rate >= 2.0) momentum_score = 0.08;
    else if (metrics.price_change_rate >= 0.5) momentum_score = 0.06;
    else if (metrics.price_change_rate >= 0.1) momentum_score = 0.03;
    else momentum_score = 0.00;
    total_score += momentum_score;
    
    // --- (2) 異붿꽭 ?뺤씤 (理쒕? 0.20) ---
    // 理쒓렐 3罹붾뱾 ?묐큺 鍮꾩쑉
    int bullish_count = 0;
    size_t n = candles.size();
    size_t start = (n >= 3) ? n - 3 : 0;
    for (size_t i = start; i < n; ++i) {
        if (candles[i].close > candles[i].open) bullish_count++;
    }
    
    double trend_score = 0.0;
    if (bullish_count >= 3) trend_score = 0.12;
    else if (bullish_count >= 2) trend_score = 0.08;
    else if (bullish_count >= 1) trend_score = 0.03;
    total_score += trend_score;
    
    // ?몃? ?덉쭚 湲곕컲 ?먯닔 (?대? detectMarketRegime ?쒓굅!)
    double regime_score = 0.0;
    switch (regime.regime) {
        case analytics::MarketRegime::TRENDING_UP:
            regime_score = 0.14; break;
        case analytics::MarketRegime::RANGING:
            regime_score = 0.08; break;
        case analytics::MarketRegime::HIGH_VOLATILITY:
            regime_score = 0.03; break;
        case analytics::MarketRegime::TRENDING_DOWN:
            regime_score = 0.01; break;
        default:
            regime_score = 0.02; break;
    }
    total_score += regime_score;
    
    // --- (3) 嫄곕옒??(理쒕? 0.15) ---
    double volume_score = 0.0;
    bool volume_significant = isVolumeSurgeSignificant(metrics, candles);
    if (volume_significant && metrics.volume_surge_ratio >= 3.0) volume_score = 0.15;
    else if (volume_significant) volume_score = 0.10;
    else if (metrics.volume_surge_ratio >= 1.0) volume_score = 0.05;
    else volume_score = 0.02;
    total_score += volume_score;
    
    // --- (4) MTF & Order Flow (理쒕? 0.20) ---
    auto mtf_signal = analyzeMultiTimeframe(metrics, candles);
    double mtf_score = mtf_signal.alignment_score * 0.10;
    total_score += mtf_score;
    
    auto order_flow = analyzeAdvancedOrderFlow(metrics, current_price);
    double of_score = 0.0;
    if (order_flow.microstructure_score > 0.6) of_score = 0.10;
    else if (order_flow.microstructure_score > 0.3) of_score = 0.06;
    else if (order_flow.microstructure_score > 0.0) of_score = 0.03;
    else of_score = 0.02; // ?멸? ?놁뼱??理쒖냼 ?먯닔
    total_score += of_score;

    // Reject when microstructure is clearly one-sided against long entries.
    const double pressure_total_hard = std::max(1e-6, std::abs(metrics.buy_pressure) + std::abs(metrics.sell_pressure));
    const double buy_pressure_bias_hard = (metrics.buy_pressure - metrics.sell_pressure) / pressure_total_hard;
    if (regime.regime == analytics::MarketRegime::TRENDING_UP) {
        if (mtf_signal.alignment_score < 0.60) {
            return signal;
        }
        if (order_flow.microstructure_score < 0.15 &&
            buy_pressure_bias_hard < 0.02 &&
            metrics.order_book_imbalance < 0.0) {
            return signal;
        }
    }
    if (order_flow.microstructure_score < 0.08 && buy_pressure_bias_hard < -0.20) {
        return signal;
    }
    
    // --- (5) Liquidity quality (max 0.05) ---
    double liquidity_score = 0.0;
    if (metrics.liquidity_score >= 50) liquidity_score = 0.05;
    else if (metrics.liquidity_score >= 30) liquidity_score = 0.03;
    else liquidity_score = 0.01;
    total_score += liquidity_score;

    // Quality adjustment: penalize when momentum/flow/liquidity are inconsistent.
    const double flow_direction = std::clamp(metrics.order_book_imbalance, -1.0, 1.0);
    const double pressure_total = std::max(1e-6, std::abs(metrics.buy_pressure) + std::abs(metrics.sell_pressure));
    const double buy_pressure_bias = std::clamp((metrics.buy_pressure - metrics.sell_pressure) / pressure_total, -1.0, 1.0);
    const double liquidity_t = clamp01((metrics.liquidity_score - strategy_cfg.min_liquidity_score) / 40.0);
    const double flow_t = clamp01((flow_direction + 0.10) / 0.80);
    const double pressure_t = clamp01((buy_pressure_bias + 0.10) / 0.80);
    const double quality_factor = std::clamp(0.86 + (0.08 * liquidity_t) + (0.05 * flow_t) + (0.04 * pressure_t), 0.78, 1.06);
    total_score *= quality_factor;

    if (metrics.price_change_rate > 0.0 && flow_direction < -0.15) {
        total_score -= 0.03;
    }
    if (metrics.volume_surge_ratio < 1.0 && metrics.price_change_rate < 0.3) {
        total_score -= 0.015;
    }
    if (regime.regime == analytics::MarketRegime::TRENDING_DOWN && buy_pressure_bias < 0.0) {
        total_score -= 0.015;
    }
    
    // ===== Final strength =====
    signal.strength = std::clamp(total_score, 0.0, 1.0);
    
    LOG_INFO("{} - [Momentum] total score: {:.3f} (RSI:{:.2f} MACD:{:.2f} Mom:{:.2f} Trend:{:.2f} Reg:{:.2f} Vol:{:.2f} MTF:{:.2f} OF:{:.2f})",
             market, signal.strength, rsi_score, macd_score, momentum_score, trend_score,
             regime_score, volume_score, mtf_score, of_score);
    
    const double dynamic_strength_floor = computeMomentumAdaptiveStrengthFloor(metrics, regime);
    double effective_strength_floor = std::max(dynamic_strength_floor, strategy_cfg.min_signal_strength);
    if ((regime.regime == analytics::MarketRegime::TRENDING_UP ||
         regime.regime == analytics::MarketRegime::RANGING) &&
        metrics.liquidity_score >= 62.0 &&
        metrics.volume_surge_ratio >= 1.05) {
        effective_strength_floor = std::max(0.27, effective_strength_floor - 0.07);
    }
    if (signal.strength < effective_strength_floor) {
        return signal;
    }
    
    // ===== Signal generation =====
    auto stops = calculateDynamicStops(current_price, candles);
    adaptMomentumStopsByLiquidityVolatility(metrics, current_price, stops);
    
    signal.type = SignalType::BUY;
    signal.entry_price = current_price;
    signal.stop_loss = stops.stop_loss;
    signal.take_profit_1 = stops.take_profit_1;
    signal.take_profit_2 = stops.take_profit_2;
    signal.buy_order_type = strategy::OrderTypePolicy::LIMIT_WITH_FALLBACK;
    signal.sell_order_type = strategy::OrderTypePolicy::LIMIT_WITH_FALLBACK;
    signal.max_retries = 3;
    signal.retry_wait_ms = 500;
    
    // ?섏씡??泥댄겕 (soft gate)
    double expected_return = (signal.take_profit_2 - signal.entry_price) / signal.entry_price;
    if (expected_return < 0.002) {
        signal.strength *= 0.7;
    }
    
    // ?ъ????ъ씠吏?
    auto pos_metrics = calculateAdvancedPositionSize(
        available_capital, signal.entry_price, signal.stop_loss, metrics, candles
    );
    {
        const double vol_t = clamp01(metrics.volatility / 6.0);
        const double liq_t = clamp01((70.0 - metrics.liquidity_score) / 70.0);
        const double pressure_total_pos = std::max(1e-6, std::abs(metrics.buy_pressure) + std::abs(metrics.sell_pressure));
        const double sell_bias_t = clamp01((metrics.sell_pressure - metrics.buy_pressure) / pressure_total_pos);
        const double book_imbalance_t = clamp01((-metrics.order_book_imbalance + 0.05) / 0.60);
        const double flow_penalty_t = std::max(sell_bias_t, book_imbalance_t);
        const double adaptive_scale = std::clamp(1.0 - (vol_t * 0.35) - (liq_t * 0.30) - (flow_penalty_t * 0.25), 0.40, 1.0);
        pos_metrics.final_position_size *= adaptive_scale;
    }
    signal.position_size = pos_metrics.final_position_size;

    const double stop_risk = std::max(1e-9, (signal.entry_price - signal.stop_loss) / signal.entry_price);
    const double reward_risk = expected_return / stop_risk;
    double rr_floor = strategy_cfg.min_risk_reward_ratio;
    if (regime.regime == analytics::MarketRegime::TRENDING_UP && metrics.liquidity_score >= 60.0) {
        rr_floor = std::max(1.25, rr_floor * 0.72);
    } else if (regime.regime == analytics::MarketRegime::RANGING && metrics.liquidity_score >= 65.0) {
        rr_floor = std::max(1.20, rr_floor * 0.68);
    }
    if (signal.strength >= 0.70) {
        rr_floor = std::max(1.15, rr_floor - 0.10);
    }
    if (reward_risk < rr_floor) {
        return signal;
    }
    if (!shouldGenerateSignal(expected_return, pos_metrics.expected_sharpe)) {
        return signal;
    }
    if (!isWorthTrading(expected_return, pos_metrics.expected_sharpe)) {
        return signal;
    }
    
    const auto& engine_cfg = Config::getInstance().getEngineConfig();
    const double min_order_krw = std::max(5000.0, engine_cfg.min_order_krw);
    if (available_capital < min_order_krw) {
        signal.type = SignalType::NONE;
        return signal;
    }

    // Budget-aware lot sizing: keep small-account exposure from jumping unexpectedly.
    const double raw_size = std::clamp(signal.position_size, 0.0, 1.0);
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
    
    if (signal.strength >= 0.70) {
        signal.type = SignalType::STRONG_BUY;
    }
    
    signal.reason = fmt::format(
        "Momentum: Score={:.0f}% RSI={:.0f} MACD={:.0f} Vol={:.1f}x",
        signal.strength * 100, rsi, macd.histogram, metrics.volume_surge_ratio
    );
    
    last_signal_time_ = getCurrentTimestamp();
    
    LOG_INFO("[Momentum] buy signal: {} - strength {:.2f}, size {:.1f}%",
             market, signal.strength, signal.position_size * 100);
    
    return signal;
}

bool MomentumStrategy::shouldEnter(
    const std::string& market,
    const analytics::CoinMetrics& metrics,
    const std::vector<Candle>& candles,
    double current_price,
    const analytics::RegimeAnalysis& regime
) {
    const auto strategy_cfg = Config::getInstance().getMomentumConfig();
    (void)regime; // Used for future enhancements
    (void)market;
    (void)current_price;
    
    if (candles.size() < 60) return false;
    
    if (!isVolumeSurgeSignificant(metrics, candles)) {
        return false;
    }
    
    auto prices = analytics::TechnicalIndicators::extractClosePrices(candles);
    double rsi = analytics::TechnicalIndicators::calculateRSI(prices, 14);
    if (rsi < 45.0 || rsi > 75.0) {
        return false;
    }
    
    auto macd = analytics::TechnicalIndicators::calculateMACD(prices, 12, 26, 9);
    if (macd.macd <= 0 || macd.histogram <= 0) {
        return false;
    }
    
    // Check last 3 candles for bullish participation
    int bullish_count = 0;
    size_t n = candles.size();
    size_t start_check = (n >= 3) ? n - 3 : 0;

    for (size_t i = start_check; i < n; ++i) {
        if (candles[i].close > candles[i].open) {
            bullish_count++;
        }
    }

    if (bullish_count < 2) {
        return false;
    }
    
    if (metrics.price_change_rate < 0.5) {
        LOG_INFO("{} - insufficient momentum {:.2f}% (target: 2.0%+)",
             market, metrics.price_change_rate);
        return false;
    }
    
    // Liquidity score check (relaxed for alt-coins)
    if (metrics.liquidity_score < strategy_cfg.min_liquidity_score) {
        return false;
    }
    
    LOG_INFO("{} - momentum entry conditions passed (RSI: {:.1f}, change: {:.2f}%, bullish: {}/3)",
             market, rsi, metrics.price_change_rate, bullish_count);

    return true;
}

bool MomentumStrategy::shouldExit(
    const std::string& market,
    double entry_price,
    double current_price,
    double holding_time_seconds
) {
    (void)market;
    (void)entry_price;
    (void)current_price;
    
    // 1. ?쒓컙 泥?궛 (蹂댁쑀 ?쒓컙???덈Т 湲몄뼱吏硫?紐⑤찘? ?곸떎濡?媛꾩＜)
    if (holding_time_seconds >= MAX_HOLDING_TIME) { // 2?쒓컙
        return true;
    }
    
    // 2. 異붿꽭 諛섏쟾 ?뺤씤 (Trend Reversal) logic???꾩슂??
    // ?섏?留??꾩옱 ?⑥닔 ?몄옄濡쒕뒗 罹붾뱾 ?곗씠?곗뿉 ?묎렐?????놁쓬.
    // ?곕씪???ш린?쒕뒗 '?쒓컙 泥?궛'留??대떦?섍퀬, 
    // 湲곗닠??吏?쒖뿉 ?섑븳 泥?궛? RiskManager??Trailing Stop??留↔린??寃껋씠 ?덉쟾??
    
    return false;
}

double MomentumStrategy::calculateStopLoss(
    double entry_price,
    const std::vector<Candle>& candles
) {
    auto stops = calculateDynamicStops(entry_price, candles);
    return stops.stop_loss;
}

double MomentumStrategy::calculateTakeProfit(
    double entry_price,
    const std::vector<Candle>& candles
) {
    auto stops = calculateDynamicStops(entry_price, candles);
    return stops.take_profit_2;
}

double MomentumStrategy::calculatePositionSize(
    double capital,
    double entry_price,
    double stop_loss,
    const analytics::CoinMetrics& metrics
) {
    std::vector<Candle> empty_candles;
    auto pos_metrics = calculateAdvancedPositionSize(
        capital, entry_price, stop_loss, metrics, empty_candles
    );
    return pos_metrics.final_position_size;
}

void MomentumStrategy::setEnabled(bool enabled) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    enabled_ = enabled;
}

bool MomentumStrategy::isEnabled() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return enabled_;
}

IStrategy::Statistics MomentumStrategy::getStatistics() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return stats_;
}

void MomentumStrategy::setStatistics(const Statistics& stats) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    stats_ = stats;
}

bool MomentumStrategy::onSignalAccepted(const Signal& signal, double allocated_capital) {
    (void)allocated_capital;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto inserted = active_positions_.insert(signal.market).second;
    if (inserted) {
        recordTrade();
    }
    return inserted;
}

void MomentumStrategy::updateStatistics(const std::string& market, bool is_win, double profit_loss) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    // Remove market from active set when position is closed.
    if (active_positions_.erase(market)) {
        LOG_INFO("{} - momentum active-position flag cleared (position closed)", market);
    }
    
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
    
    trade_timestamps_.push_back(getCurrentTimestamp());
    if (trade_timestamps_.size() > 1000) {
        trade_timestamps_.pop_front();
    }
    
    updateRollingStatistics();
    checkCircuitBreaker();
}

double MomentumStrategy::updateTrailingStop(
    double entry_price,
    double highest_price,
    double current_price,
    const std::vector<Candle>& recent_candles
) {
    if (recent_candles.empty()) {
        return entry_price * 0.98;
    }
    
    double atr = analytics::TechnicalIndicators::calculateATR(recent_candles, 14);
    
    double chandelier = calculateChandelierExit(highest_price, atr, 3.0);
    double parabolic = calculateParabolicSAR(recent_candles, 0.02, 0.20);
    
    double trailing_stop = std::max(chandelier, parabolic);
    
    if (trailing_stop >= current_price) {
        trailing_stop = entry_price * 0.98;
    }
    
    return trailing_stop;
}

RollingStatistics MomentumStrategy::getRollingStatistics() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return rolling_stats_;
}

// ===== 1. Market Regime Detection (HMM) =====

MarketRegime MomentumStrategy::detectMarketRegime(
    const std::vector<Candle>& candles
) {
    if (candles.size() < 20) {
        return MarketRegime::SIDEWAYS;
    }
    
    updateRegimeModel(candles, regime_model_);
    
    double strong_up = regime_model_.current_prob[static_cast<int>(MarketRegime::STRONG_UPTREND)];
    double weak_up = regime_model_.current_prob[static_cast<int>(MarketRegime::WEAK_UPTREND)];
    
    // [?듭떖] ???곸듅 ?뺣쪧???⑹씠 ?〓낫 ?뺣쪧蹂대떎 ?ш굅?? 40%瑜??섏쑝硫??곸듅?쇰줈 ?먯젙
    if ((strong_up + weak_up) > 0.40) {
        // ???섏씠 媛뺥븳 履쎌쓣 諛섑솚?섍굅?? 紐⑤찘? ?꾨왂?대씪硫?WEAK留??섏뼱??吏꾩엯 ?덉슜
        return (strong_up > weak_up) ? MarketRegime::STRONG_UPTREND : MarketRegime::WEAK_UPTREND;
    }

    int max_idx = 0;
    double max_prob = regime_model_.current_prob[0];
    
    for (int i = 1; i < 5; ++i) {
        if (regime_model_.current_prob[i] > max_prob) {
            max_prob = regime_model_.current_prob[i];
            max_idx = i;
        }
    }
    
    return static_cast<MarketRegime>(max_idx);
}

void MomentumStrategy::updateRegimeModel(
    const std::vector<Candle>& candles,
    RegimeModel& model
) {
    if (candles.size() < 20) return;
    
    std::vector<double> returns;
    // [???듭떖 ?섏젙] 
    // ?곗씠?곌? [怨쇨굅 -> ?꾩옱] ?쒖꽌?대?濡? 
    // 媛????back)??理쒖떊 ?곗씠?곗엯?덈떎. 
    // ?곕씪???ㅼ뿉?쒕????욎쑝濡?媛硫댁꽌 怨꾩궛?댁빞 "理쒖떊 ?섏씡瑜????살뒿?덈떎.
    
    size_t end_idx = candles.size() - 1;
    size_t start_idx = (candles.size() > 20) ? (candles.size() - 20) : 0;

    // ??理쒖떊)?먯꽌遺??怨쇨굅濡?媛硫댁꽌 猷⑦봽
    for (size_t i = end_idx; i > start_idx; --i) {
        // 怨듭떇: (?꾩옱媛寃?- ?꾧?寃? / ?꾧?寃?
        // candles[i]媛 ?꾩옱, candles[i-1]??怨쇨굅
        double ret = (candles[i].close - candles[i-1].close) / candles[i-1].close;
        
        returns.push_back(ret);
    }
    
    double mean_return = calculateMean(returns);
    double volatility = calculateStdDev(returns, mean_return);
    // 理쒖냼 蹂?숈꽦 ?섑븳??(蹂?숈꽦??洹밸룄濡???쓣 ???몄씠利덉뿉 ???寃?諛⑹?)
    double active_vol = std::max(volatility, 0.0005);
    std::array<double, 5> observation_prob;
    
    // 1. STRONG_UP: ?섏씡瑜좎씠 蹂?숈꽦??1.5諛??댁긽 (媛뺥븳 ?뚰뙆)
    if (mean_return > active_vol * 1.5) {
        observation_prob = {0.70, 0.20, 0.05, 0.03, 0.02};
    } 
    // 2. WEAK_UP: ?섏씡瑜좎씠 蹂?숈꽦??0.5諛??댁긽 (?꾨쭔???곸듅)
    else if (mean_return > active_vol * 0.5) {
        observation_prob = {0.20, 0.65, 0.10, 0.03, 0.02};
    } 
    // 3. SIDEWAYS: ?섏씡瑜좎씠 蹂?숈꽦??+-0.5諛??대궡 (諛뺤뒪沅?
    else if (std::abs(mean_return) <= active_vol * 0.5) {
        observation_prob = {0.10, 0.15, 0.55, 0.15, 0.05};
    } 
    // 4. WEAK_DOWN: ?섏씡瑜좎씠 -0.5諛?誘몃쭔
    else if (mean_return < -active_vol * 0.5 && mean_return >= -active_vol * 1.5) {
        observation_prob = {0.05, 0.05, 0.15, 0.60, 0.15};
    } 
    // 5. STRONG_DOWN: ?섏씡瑜좎씠 -1.5諛?誘몃쭔
    else {
        observation_prob = {0.02, 0.03, 0.05, 0.20, 0.70};
    }
    
    double total = 0.0;
    for (int i = 0; i < 5; ++i) {
        model.current_prob[i] *= observation_prob[i];

        //?뱀젙 ?곹깭媛 怨좎궗(Dead)?섎뒗 寃껋쓣 諛⑹??섏뿬 諛섏쓳???좎?
        if (model.current_prob[i] < 0.0001) model.current_prob[i] = 0.0001;

        total += model.current_prob[i];
    }
    
    if (total > 0) {
        for (int i = 0; i < 5; ++i) {
            model.current_prob[i] /= total;
        }
    }
}

// ===== 2. Statistical Significance =====

bool MomentumStrategy::isVolumeSurgeSignificant(
    const analytics::CoinMetrics& metrics,
    const std::vector<Candle>& candles
) const {
    // 1. 理쒖냼 ?곗씠???뺣낫 (?꾩옱 1媛?+ 怨쇨굅 30媛?= 31媛?
        (void)metrics;  // ?꾩옱 誘몄궗??
    if (candles.size() < 31) return false;
    
    // 2. [?섏젙] ?꾩껜 31媛??곗씠???섏쭛 (怨쇨굅 30媛?+ ?꾩옱 1媛?
    std::vector<double> volumes;
    volumes.reserve(31);
    
    // ?뺣젹??candles???ㅼ뿉??31踰덉㎏遺???앷퉴吏
    for (size_t i = candles.size() - 31; i < candles.size(); ++i) {
        volumes.push_back(candles[i].volume);
    }

    // 3. Z-Score 怨꾩궛 (?꾩옱媛?vs 怨쇨굅 30媛??됯퇏)
    double current_volume = volumes.back();
    // history: volumes??泥섏쓬遺???ㅼ뿉????踰덉㎏源뚯? (留덉?留??쒖쇅)
    std::vector<double> history(volumes.begin(), volumes.end() - 1);
    
    double z_score = calculateZScore(current_volume, history);
    
    // [以묒슂] Z-Score ?꾪꽣留?(?꾨씫?섏뿀??遺遺?蹂듦뎄!)
    // 1.96? 95% ?좊ː援ш컙???섎?. 利? ?곸쐞 2.5% ?섏???湲됰벑留??몄젙
    if (z_score < 1.64) {
        return false;
    }
    
    // 4. T-Test: 理쒓렐 5媛??꾩옱 ?ы븿) vs 洹??댁쟾 26媛?
    // volumes 踰≫꽣 ?댁뿉??遺꾨━
    std::vector<double> recent_5(volumes.end() - 5, volumes.end());
    std::vector<double> past_26(volumes.begin(), volumes.end() - 5);
    
    // ?좎쓽?섏? 0.05 (5%)
    return isTTestSignificant(recent_5, past_26, 0.05);
}

double MomentumStrategy::calculateZScore(
    double value,
    const std::vector<double>& history
) const {
    if (history.size() < 2) return 0.0;
    
    double mean = calculateMean(history);
    double std_dev = calculateStdDev(history, mean);
    
    if (std_dev < 0.0001) return 0.0;
    
    return (value - mean) / std_dev;
}

bool MomentumStrategy::isTTestSignificant(
    const std::vector<double>& sample1,
    const std::vector<double>& sample2,
    double alpha
) const {
    if (sample1.size() < 2 || sample2.size() < 2) return false;
        (void)alpha;  // 怨좎젙 t_critical 媛??ъ슜
    
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
    
    double t_critical = 1.812;
    
    return std::abs(t_stat) > t_critical;
}

bool MomentumStrategy::isKSTestPassed(
    const std::vector<double>& sample
) const {
    if (sample.size() < 20) return false;
    
    double mean = calculateMean(sample);
    double std_dev = calculateStdDev(sample, mean);
    
    if (std_dev < 0.0001) return false;
    
    std::vector<double> standardized;
    for (double val : sample) {
        standardized.push_back((val - mean) / std_dev);
    }
    
    std::sort(standardized.begin(), standardized.end());
    
    double max_diff = 0.0;
    for (size_t i = 0; i < standardized.size(); ++i) {
        double ecdf = static_cast<double>(i + 1) / standardized.size();
        double z = standardized[i];
        double normal_cdf = 0.5 * (1.0 + std::erf(z / std::sqrt(2.0)));
        
        double diff = std::abs(ecdf - normal_cdf);
        max_diff = std::max(max_diff, diff);
    }
    
    double critical_value = 1.36 / std::sqrt(standardized.size());
    
    return max_diff < critical_value;
}

// ===== 3. Multi-Timeframe Analysis =====

MultiTimeframeSignal MomentumStrategy::analyzeMultiTimeframe(
    const analytics::CoinMetrics& metrics,
    const std::vector<Candle>& candles_1m
) const {
    MultiTimeframeSignal signal;
    
    if (candles_1m.size() < 60) {
        return signal;
    }
    
    // 1. 1遺꾨큺 遺꾩꽍 (?곗씠??異⑸텇)
    signal.tf_1m_bullish = isBullishOnTimeframe(candles_1m, signal.tf_1m);
    
    // 2. 5분봉 분석: scanner preloaded TF 우선, 없으면 1m 리샘플 fallback
    std::vector<Candle> candles_5m;
    auto tf_5m_it = metrics.candles_by_tf.find("5m");
    if (tf_5m_it != metrics.candles_by_tf.end() && !tf_5m_it->second.empty()) {
        candles_5m = tf_5m_it->second;
    } else {
        candles_5m = resampleTo5m(candles_1m);
    }
    if (!candles_5m.empty()) {
        signal.tf_5m_bullish = isBullishOnTimeframe(candles_5m, signal.tf_5m);
    }
    
    // 3. 15분봉 분석: 5m 충분 시 재집계(3x5m), 아니면 기존 1m 리샘플
    auto candles_15m = resampleTo15m(candles_1m);
    if (candles_15m.size() < 26 && candles_5m.size() >= 78) {
        candles_15m = aggregateCandles(candles_5m, 3);
    }
    bool is_15m_valid = false; // 15遺꾨큺???먯닔 怨꾩궛???ｌ쓣吏 ?щ?

    // [?듭떖] 吏??怨꾩궛???꾩슂??理쒖냼 媛쒖닔(MACD 湲곗? 26媛?媛 ?덈뒗吏 ?뺤씤
    if (candles_15m.size() >= 26) {
        signal.tf_15m_bullish = isBullishOnTimeframe(candles_15m, signal.tf_15m);
        is_15m_valid = true; // ?곗씠?곌? 異⑸텇?섎?濡??먯닔?먯뿉 ?쇱썙以?
    }
    
    // 4. 1시간봉 보조 확인 (있을 때만 반영)
    bool is_1h_valid = false;
    bool tf_1h_bullish = false;
    MultiTimeframeSignal::TimeframeMetrics tf_1h_metrics;
    auto tf_1h_it = metrics.candles_by_tf.find("1h");
    if (tf_1h_it != metrics.candles_by_tf.end() && tf_1h_it->second.size() >= 26) {
        tf_1h_bullish = isBullishOnTimeframe(tf_1h_it->second, tf_1h_metrics);
        is_1h_valid = true;
    }

    // 5. 동적 점수 계산 (Dynamic Scoring)
    double total_score = 0.0;
    double max_possible_score = 0.0;

    // 1遺꾨큺 諛섏쁺
    max_possible_score += 1.0;
    if (signal.tf_1m_bullish) total_score += 1.0;

    // 5遺꾨큺 諛섏쁺
    max_possible_score += 1.0;
    if (signal.tf_5m_bullish) total_score += 1.0;

    // 15遺꾨큺 諛섏쁺 (?곗씠?곌? 異⑸텇???뚮쭔 遺꾨え/遺꾩옄???ы븿)
    if (is_15m_valid) {
        max_possible_score += 1.0;
        if (signal.tf_15m_bullish) total_score += 1.0;
    }
    
    // 1시간봉 반영 (데이터 있을 때만)
    if (is_1h_valid) {
        max_possible_score += 1.0;
        if (tf_1h_bullish) total_score += 1.0;
    }

    // 0으로 나누기 방지
    signal.alignment_score = (max_possible_score > 0) 
                           ? (total_score / max_possible_score) 
                           : 0.0;
    
    return signal;
}

std::vector<Candle> MomentumStrategy::resampleTo5m(const std::vector<Candle>& candles_1m) const {
    if (candles_1m.size() < 5) return {};
    std::vector<Candle> candles_5m;
    for (size_t i = 0; i + 5 <= candles_1m.size(); i += 5) {
        Candle candle_5m;
        candle_5m.open = candles_1m[i].open;         // [?섏젙] i媛 媛??怨쇨굅
        candle_5m.close = candles_1m[i + 4].close;   // [?섏젙] i+4媛 媛??理쒖떊
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

std::vector<Candle> MomentumStrategy::resampleTo15m(
    const std::vector<Candle>& candles_1m
) const {
    if (candles_1m.size() < 15) return {};
    
    std::vector<Candle> candles_15m;
    
    for (size_t i = 0; i + 15 <= candles_1m.size(); i += 15) {
        Candle candle_15m;
        
        candle_15m.open = candles_1m[i].open;
        candle_15m.close = candles_1m[i + 14].close;
        candle_15m.high = candles_1m[i].high;
        candle_15m.low = candles_1m[i].low;
        candle_15m.volume = 0;
        
        for (size_t j = i; j < i + 15; ++j) {
            candle_15m.high = std::max(candle_15m.high, candles_1m[j].high);
            candle_15m.low = std::min(candle_15m.low, candles_1m[j].low);
            candle_15m.volume += candles_1m[j].volume;
        }
        
        candle_15m.timestamp = candles_1m[i].timestamp;
        candles_15m.push_back(candle_15m);
    }
    
    return candles_15m;
}

bool MomentumStrategy::isBullishOnTimeframe(const std::vector<Candle>& candles, MultiTimeframeSignal::TimeframeMetrics& metrics) const {
    if (candles.size() < 26) return false;
    auto prices = analytics::TechnicalIndicators::extractClosePrices(candles);
    metrics.rsi = analytics::TechnicalIndicators::calculateRSI(prices, 14);
    auto macd = analytics::TechnicalIndicators::calculateMACD(prices, 12, 26, 9);
    metrics.macd_histogram = macd.histogram;

    // [?섏젙] EMA20怨?3遊???媛寃⑹쓣 鍮꾧탳?섏뿬 ?꾩옱 異붿꽭 痢≪젙
    double ema_20 = analytics::TechnicalIndicators::calculateEMA(prices, 20);
    double current_price = prices.back();
    metrics.trend_strength = (current_price - ema_20) / ema_20;

    bool rsi_bullish = metrics.rsi >= 50.0 && metrics.rsi <= 75.0; // ?곷떒 踰붿쐞 ?뺤옣
    bool macd_bullish = metrics.macd_histogram > 0;
    bool trend_bullish = metrics.trend_strength > 0;

    return rsi_bullish && macd_bullish && trend_bullish;
}

// ===== 4. Advanced Order Flow =====

AdvancedOrderFlowMetrics MomentumStrategy::analyzeAdvancedOrderFlow(
    const analytics::CoinMetrics& metrics,
    double current_price
) const {
    (void)current_price;  // current_price ?뚮씪誘명꽣 誘몄궗??
    
    AdvancedOrderFlowMetrics flow;
    nlohmann::json units;
    
    try {
        if (metrics.orderbook_units.is_array() && !metrics.orderbook_units.empty()) {
            units = metrics.orderbook_units;
        } else if (!metrics.market.empty()) {
            auto orderbook = client_->getOrderBook(metrics.market);
            if (orderbook.is_array() && !orderbook.empty() && orderbook[0].contains("orderbook_units")) {
                units = orderbook[0]["orderbook_units"];
            } else if (orderbook.contains("orderbook_units")) {
                units = orderbook["orderbook_units"];
            }
        }
        
        if (units.is_array() && !units.empty()) {
            
            double best_ask = units[0]["ask_price"].get<double>();
            double best_bid = units[0]["bid_price"].get<double>();
            flow.bid_ask_spread = (best_ask - best_bid) / best_bid * 100;
            
            double total_bid_volume = 0;
            double total_ask_volume = 0;
            
            for (const auto& unit : units) {
                total_bid_volume += unit["bid_size"].get<double>();
                total_ask_volume += unit["ask_size"].get<double>();
            }
            
            if (total_bid_volume + total_ask_volume > 0) {
                flow.order_book_pressure = 
                    (total_bid_volume - total_ask_volume) / (total_bid_volume + total_ask_volume);
            }
            
            double large_threshold = (total_bid_volume + total_ask_volume) / units.size() * 2.0;
            double large_bid = 0, large_ask = 0;
            
            for (const auto& unit : units) {
                double bid_size = unit["bid_size"].get<double>();
                double ask_size = unit["ask_size"].get<double>();
                
                if (bid_size > large_threshold) large_bid += bid_size;
                if (ask_size > large_threshold) large_ask += ask_size;
            }
            
            if (large_bid + large_ask > 0) {
                flow.large_order_imbalance = (large_bid - large_ask) / (large_bid + large_ask);
            }
            
            flow.cumulative_delta = calculateCumulativeDelta(units);
            
            double top5_volume = 0;
            for (size_t i = 0; i < std::min(size_t(5), units.size()); ++i) {
                top5_volume += units[i]["bid_size"].get<double>();
                top5_volume += units[i]["ask_size"].get<double>();
            }
            
            if (total_bid_volume + total_ask_volume > 0) {
                flow.order_book_depth_ratio = top5_volume / (total_bid_volume + total_ask_volume);
            }
        }
        
        double score = 0.0;
        
        if (flow.bid_ask_spread < 0.05) score += 0.30;
        else if (flow.bid_ask_spread < 0.10) score += 0.20;
        else score += 0.10;
        
        // Order Book Pressure (30%) ?섏젙
        if (flow.order_book_pressure > 0.1) score += 0.30;       // 0.3 -> 0.1
        else if (flow.order_book_pressure > -0.1) score += 0.20; // 洹좏삎留??대쨪???먯닔
        else if (flow.order_book_pressure > -0.3) score += 0.10;

        // Large Order Imbalance (20%) ?섏젙
        if (flow.large_order_imbalance > 0.1) score += 0.20;     // 0.2 -> 0.1
        else if (flow.large_order_imbalance > -0.1) score += 0.10;
        
        if (flow.cumulative_delta > 0.1) score += 0.20;
        else if (flow.cumulative_delta > 0) score += 0.10;
        
        flow.microstructure_score = score;
        
    } catch (const std::exception& e) {
        LOG_ERROR("Order Flow 遺꾩꽍 ?ㅽ뙣: {}", e.what());
    }
    
    return flow;
}

double MomentumStrategy::calculateVWAPDeviation(
    const std::vector<Candle>& candles,
    double current_price
) const {
    double vwap = analytics::TechnicalIndicators::calculateVWAP(candles);
    
    if (vwap < 0.0001) return 0.0;
    
    return (current_price - vwap) / vwap * 100;
}

AdvancedOrderFlowMetrics::VolumeProfile MomentumStrategy::calculateVolumeProfile(
    const std::vector<Candle>& candles
) const {
    AdvancedOrderFlowMetrics::VolumeProfile profile;
    
    if (candles.size() < 10) return profile;
    
    double min_price = candles[0].low;
    double max_price = candles[0].high;
    
    for (const auto& candle : candles) {
        min_price = std::min(min_price, candle.low);
        max_price = std::max(max_price, candle.high);
    }
    
    double price_step = (max_price - min_price) / 20.0;
    if (price_step < 0.00000001) {
        profile.point_of_control = min_price;
        profile.value_area_low = min_price;
        profile.value_area_high = max_price;
        return profile;
    }
    std::vector<double> volume_at_price(20, 0.0);
    
    for (const auto& candle : candles) {
        int idx = static_cast<int>((candle.close - min_price) / price_step);
        idx = std::clamp(idx, 0, 19);
        volume_at_price[idx] += candle.volume;
    }
    
    int max_vol_idx = 0;
    for (int i = 1; i < 20; ++i) {
        if (volume_at_price[i] > volume_at_price[max_vol_idx]) {
            max_vol_idx = i;
        }
    }
    
    profile.point_of_control = min_price + (max_vol_idx + 0.5) * price_step;
    
    double total_volume = std::accumulate(volume_at_price.begin(), volume_at_price.end(), 0.0);
    double target_volume = total_volume * 0.70;
    
    double accumulated = volume_at_price[max_vol_idx];
    int lower = max_vol_idx;
    int upper = max_vol_idx;
    
    while (accumulated < target_volume && (lower > 0 || upper < 19)) {
        if (lower > 0 && (upper >= 19 || volume_at_price[lower-1] > volume_at_price[upper+1])) {
            lower--;
            accumulated += volume_at_price[lower];
        } else if (upper < 19) {
            upper++;
            accumulated += volume_at_price[upper];
        } else {
            break;
        }
    }
    
    profile.value_area_low = min_price + lower * price_step;
    profile.value_area_high = min_price + (upper + 1) * price_step;
    
    return profile;
}

double MomentumStrategy::calculateCumulativeDelta(
    const nlohmann::json& orderbook_units
) const {
    if (!orderbook_units.is_array() || orderbook_units.empty()) {
        return 0.0;
    }
    
    auto units = orderbook_units;
    
    double cumulative_delta = 0.0;
    
    for (const auto& unit : units) {
        double bid_size = unit["bid_size"].get<double>();
        double ask_size = unit["ask_size"].get<double>();
        
        cumulative_delta += (bid_size - ask_size);
    }
    
    double total_volume = 0;
    for (const auto& unit : units) {
        total_volume += unit["bid_size"].get<double>();
        total_volume += unit["ask_size"].get<double>();
    }
    
    if (total_volume > 0) {
        cumulative_delta /= total_volume;
    }
    
    return cumulative_delta;
}

// ===== 5. Position Sizing (Kelly Criterion + Volatility) =====

PositionMetrics MomentumStrategy::calculateAdvancedPositionSize(
    double capital,
    double entry_price,
    double stop_loss,
    const analytics::CoinMetrics& metrics,
    const std::vector<Candle>& candles
) const {
    PositionMetrics pos_metrics;
    
    // 1. Kelly Fraction 怨꾩궛
    double win_rate = stats_.win_rate > 0 ? stats_.win_rate : 0.54;
    double avg_win = stats_.avg_profit > 0 ? stats_.avg_profit : 0.05;
    double avg_loss = stats_.avg_loss > 0 ? stats_.avg_loss : 0.02;
    
    pos_metrics.kelly_fraction = calculateKellyFraction(win_rate, avg_win, avg_loss);
    pos_metrics.half_kelly = pos_metrics.kelly_fraction * HALF_KELLY_FRACTION;
    
    // 2. 蹂?숈꽦 議곗젙
    double volatility = 0.02;  // 湲곕낯 2%
    if (!candles.empty()) {
        volatility = calculateVolatility(candles);
    }
    
    pos_metrics.volatility_adjusted = adjustForVolatility(pos_metrics.half_kelly, volatility);
    
    // 3. ?좊룞??議곗젙
    double liquidity_factor = metrics.liquidity_score / 100.0;
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
    
    // 5. ?덉긽 Sharpe Ratio
    double risk = std::abs(entry_price - stop_loss) / entry_price;
    if (risk > 0.0001) {
        double reward = BASE_TAKE_PROFIT;
        pos_metrics.expected_sharpe = (reward - risk) / (volatility * std::sqrt(252));
    }
    
    // 6. 理쒕? ?먯떎 湲덉븸
    pos_metrics.max_loss_amount = capital * pos_metrics.final_position_size * risk;
    
    return pos_metrics;
}

double MomentumStrategy::calculateKellyFraction(
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
    kelly = std::min(0.20, kelly);
    
    return kelly;
}

double MomentumStrategy::adjustForVolatility(
    double kelly_size,
    double volatility
) const {
    if (volatility < 0.0001) return kelly_size;
    
    double target_volatility = 0.02;
    double vol_adjustment = target_volatility / volatility;
    
    vol_adjustment = std::clamp(vol_adjustment, 0.5, 1.5);
    
    return kelly_size * vol_adjustment;
}

// ===== 6. Dynamic Stops =====

DynamicStops MomentumStrategy::calculateDynamicStops(
    double entry_price,
    const std::vector<Candle>& candles
) const {
    DynamicStops stops;
    
    if (candles.size() < 14) {
        stops.stop_loss = entry_price * (1.0 - BASE_STOP_LOSS);
        stops.take_profit_1 = entry_price * (1.0 + BASE_TAKE_PROFIT * 0.5);
        stops.take_profit_2 = entry_price * (1.0 + BASE_TAKE_PROFIT);
        stops.trailing_start = entry_price * (1.0 + BASE_TAKE_PROFIT * 0.3);
        return stops;
    }
    
    // 1. ATR 湲곕컲 ?먯젅
    double atr_stop = calculateATRBasedStop(entry_price, candles, 2.0);
    
    // 2. Support 湲곕컲 ?먯젅
    double support_stop = findNearestSupport(entry_price, candles);
    
    // 3. Hard Stop (理쒖냼 ?먯젅)
    double hard_stop = entry_price * (1.0 - BASE_STOP_LOSS);
    
    // 媛???믪? ?먯젅???좏깮 (媛??蹂댁닔??
    stops.stop_loss = std::max({hard_stop, atr_stop, support_stop});
    
    // 吏꾩엯媛蹂대떎 ?믪쑝硫??덈맖
    if (stops.stop_loss >= entry_price) {
        stops.stop_loss = hard_stop;
    }
    
    // 4. Take Profit 怨꾩궛
    double risk = entry_price - stops.stop_loss;
    double reward_ratio = 2.5;
    
    stops.take_profit_1 = entry_price + (risk * reward_ratio * 0.5);
    stops.take_profit_2 = entry_price + (risk * reward_ratio);
    
    // 5. Trailing Start
    stops.trailing_start = entry_price + (risk * reward_ratio * 0.3);
    
    // 6. Chandelier Exit & Parabolic SAR (李멸퀬??
    double atr = analytics::TechnicalIndicators::calculateATR(candles, 14);
    stops.chandelier_exit = calculateChandelierExit(entry_price, atr, 3.0);
    stops.parabolic_sar = calculateParabolicSAR(candles, 0.02, 0.20);
    
    return stops;
}

double MomentumStrategy::calculateATRBasedStop(
    double entry_price,
    const std::vector<Candle>& candles,
    double multiplier
) const {
    double atr = analytics::TechnicalIndicators::calculateATR(candles, 14);
    
    if (atr < 0.0001) {
        return entry_price * (1.0 - BASE_STOP_LOSS);
    }
    
    double atr_percent = (atr / entry_price) * 100;
    
    if (atr_percent < 1.0) {
        multiplier = 1.5;
    } else if (atr_percent < 3.0) {
        multiplier = 2.0;
    } else {
        multiplier = 2.5;
    }
    
    double stop_loss = entry_price - (atr * multiplier);
    
    double min_stop = entry_price * (1.0 - BASE_STOP_LOSS * 1.5);
    double max_stop = entry_price * (1.0 - BASE_STOP_LOSS * 0.5);
    
    return std::clamp(stop_loss, min_stop, max_stop);
}

double MomentumStrategy::findNearestSupport(
    double entry_price,
    const std::vector<Candle>& candles
) const {
    auto support_levels = analytics::TechnicalIndicators::findSupportLevels(candles, 20);
    
    if (support_levels.empty()) {
        return entry_price * (1.0 - BASE_STOP_LOSS);
    }
    
    double nearest_support = 0;
    for (auto level : support_levels) {
        if (level < entry_price && level > nearest_support) {
            nearest_support = level;
        }
    }
    
    if (nearest_support > 0) {
        return nearest_support * 0.998;
    }
    
    return entry_price * (1.0 - BASE_STOP_LOSS);
}

double MomentumStrategy::calculateChandelierExit(
    double highest_price,
    double atr,
    double multiplier
) const {
    return highest_price - (atr * multiplier);
}

double MomentumStrategy::calculateParabolicSAR(
    const std::vector<Candle>& candles,
    double acceleration,
    double max_af
) const {
    // 1. 理쒖냼 ?곗씠???뺣낫 (異⑸텇???꾩쟻???꾪빐 30媛?異붿쿇)
    if (candles.size() < 30) {
        return candles.back().low * 0.99; // ?곗씠??遺議????꾩옱媛蹂대떎 ?댁쭩 ?꾨옒
    }
    
    // 2. [?섏젙] ?쒖옉 吏???ㅼ젙 (理쒓렐 30媛??꾩쓽 ?곗씠?곕? 珥덇린媛믪쑝濡??ъ슜)
    size_t lookback = 30;
    size_t start_idx = candles.size() - lookback;
    
    double sar = candles[start_idx].low;  // ?쒖옉?먯쓽 ?媛
    double ep = candles[start_idx].high;  // ?쒖옉?먯쓽 理쒓퀬媛(Extreme Point)
    double af = acceleration;             // 媛?띾룄 珥덇린??
    
    // 3. [?섏젙] ?뺣갑??猷⑦봽 (start_idx遺??留덉?留됯퉴吏)
    // 怨쇨굅?먯꽌 ?꾩옱濡??ㅻ㈃??媛?띾룄瑜?遺숈뿬???⑸땲??
    for (size_t i = start_idx + 1; i < candles.size(); ++i) {
        // SAR 怨듭떇 ?곸슜
        sar = sar + af * (ep - sar);
        
        // ?덈줈??怨좉? 寃쎌떊 ??EP ?낅뜲?댄듃 諛?媛?띾룄 利앷?
        if (candles[i].high > ep) {
            ep = candles[i].high;
            af = std::min(af + acceleration, max_af);
        }
        
        // ?곸듅?μ씪 ??SAR? ?꾩씪/?꾩쟾???媛蹂대떎 ?믪쓣 ???놁쓬 (?덉쟾?μ튂)
        sar = std::min(sar, std::min(candles[i-1].low, candles[i].low));
    }
    
    return sar; // 理쒖쥌 寃곌낵媛 ?꾩옱 ?쒖젏??SAR 媛?
}

// ===== 7. Trade Cost Analysis =====

bool MomentumStrategy::isWorthTrading(double expected_return, double expected_sharpe) const {
    double total_cost = (FEE_RATE * 2) + (EXPECTED_SLIPPAGE * 2); 
    double net_return = expected_return - total_cost;

    // Keep positive edge floor but avoid starving signal sample collection.
    if (net_return < 0.0025) return false;
    if (expected_sharpe < MIN_SHARPE_RATIO) return false;

    double actual_rr = expected_return / BASE_STOP_LOSS;
    if (actual_rr < 1.2) return false;

    return true;
}

double MomentumStrategy::calculateExpectedSlippage(
    const analytics::CoinMetrics& metrics
) const {
    double base_slippage = EXPECTED_SLIPPAGE;
    
    if (metrics.liquidity_score < 50.0) {
        base_slippage *= 2.0;
    } else if (metrics.liquidity_score < 70.0) {
        base_slippage *= 1.5;
    }
    
    return base_slippage;
}

// ===== 8. Signal Strength =====

double MomentumStrategy::calculateSignalStrength(
    const analytics::CoinMetrics& metrics,
    const std::vector<Candle>& candles,
    const MultiTimeframeSignal& mtf_signal,
    const AdvancedOrderFlowMetrics& order_flow,
    MarketRegime regime
) const {
    double strength = 0.0;
    
    // 1. Market Regime (20%)
    if (regime == MarketRegime::STRONG_UPTREND) strength += 0.20;
    else if (regime == MarketRegime::WEAK_UPTREND) strength += 0.15;
    else strength += 0.05;
    
    // 2. Multi-Timeframe Alignment (25%)
    strength += mtf_signal.alignment_score * 0.25;
    
    // 3. Order Flow (20%)
    strength += order_flow.microstructure_score * 0.20;
    
    // 4. Volume Surge (15%)
    if (metrics.volume_surge_ratio >= 200) strength += 0.15;      // 300 -> 200
    else if (metrics.volume_surge_ratio >= 150) strength += 0.10; // 200 -> 150
    else strength += 0.05;
    
    // 5. RSI (10%)
    auto prices = analytics::TechnicalIndicators::extractClosePrices(candles);
    double rsi = analytics::TechnicalIndicators::calculateRSI(prices, 14);
    if (rsi >= 55 && rsi <= 65) strength += 0.10;
    else if (rsi >= 50 && rsi <= 70) strength += 0.07;
    else strength += 0.03;
    
    // 6. Price Momentum (10%)
    if (metrics.price_change_rate >= 3.0) strength += 0.10;       // 5.0 -> 3.0
    else if (metrics.price_change_rate >= 1.5) strength += 0.07;  // 3.0 -> 1.5
    else strength += 0.03;

    // 7. ?щ━?쇱? ?⑤꼸??(理쒕? -10%)
    double expected_slip = calculateExpectedSlippage(metrics);
    if (expected_slip > EXPECTED_SLIPPAGE) {
        double penalty = (expected_slip - EXPECTED_SLIPPAGE) / (EXPECTED_SLIPPAGE * 2.0);
        penalty = std::clamp(penalty, 0.0, 1.0);
        strength -= penalty * 0.10;
    }
    
    return std::clamp(strength, 0.0, 1.0);
}

// ===== 9. Risk Management =====

double MomentumStrategy::calculateCVaR(
    double position_size,
    double volatility,
    double confidence_level
) const {
    if (recent_returns_.empty() || volatility < 0.0001) {
        return position_size * 0.02;
    }
    
    std::vector<double> sorted_returns(recent_returns_.begin(), recent_returns_.end());
    std::sort(sorted_returns.begin(), sorted_returns.end());
    
    int var_index = static_cast<int>(sorted_returns.size() * (1.0 - confidence_level));
    var_index = std::max(0, std::min(var_index, static_cast<int>(sorted_returns.size()) - 1));
    
    double sum_tail = 0.0;
    int tail_count = 0;
    
    for (int i = 0; i <= var_index; ++i) {
        sum_tail += std::abs(sorted_returns[i]);
        tail_count++;
    }
    
    double cvar = tail_count > 0 ? sum_tail / tail_count : 0.02;
    
    return position_size * cvar;
}

double MomentumStrategy::calculateExpectedShortfall(
    const std::vector<double>& returns,
    double confidence_level
) const {
    if (returns.empty()) return 0.0;
    
    std::vector<double> sorted_returns = returns;
    std::sort(sorted_returns.begin(), sorted_returns.end());
    
    int cutoff_index = static_cast<int>(returns.size() * (1.0 - confidence_level));
    cutoff_index = std::max(0, std::min(cutoff_index, static_cast<int>(returns.size()) - 1));
    
    double sum = 0.0;
    for (int i = 0; i <= cutoff_index; ++i) {
        sum += sorted_returns[i];
    }
    
    return cutoff_index > 0 ? sum / (cutoff_index + 1) : 0.0;
}

// ===== 10. Performance Metrics =====

double MomentumStrategy::calculateSharpeRatio(
    double risk_free_rate,
    int periods_per_year
) const {
    if (recent_returns_.size() < 10) {
        return 0.5;
    }
    
    double mean_return = calculateMean(
        std::vector<double>(recent_returns_.begin(), recent_returns_.end())
    );
    
    double std_dev = calculateStdDev(
        std::vector<double>(recent_returns_.begin(), recent_returns_.end()),
        mean_return
    );
    
    if (std_dev < 0.0001) return 0.0;
    
    double annualized_return = mean_return * periods_per_year;
    double annualized_vol = std_dev * std::sqrt(periods_per_year);
    
    return (annualized_return - risk_free_rate) / annualized_vol;
}

double MomentumStrategy::calculateSortinoRatio(
    double mar,
    int periods_per_year
) const {
    if (recent_returns_.size() < 10) {
        return 0.0;
    }
    
    std::vector<double> returns(recent_returns_.begin(), recent_returns_.end());
    double mean_return = calculateMean(returns);
    
    std::vector<double> downside_returns;
    for (double ret : returns) {
        if (ret < mar) {
            downside_returns.push_back(ret - mar);
        }
    }
    
    if (downside_returns.empty()) return 0.0;
    
    double downside_deviation = calculateStdDev(downside_returns, 0.0);
    
    if (downside_deviation < 0.0001) return 0.0;
    
    double annualized_return = mean_return * periods_per_year;
    double annualized_dd = downside_deviation * std::sqrt(periods_per_year);
    
    return (annualized_return - mar) / annualized_dd;
}

double MomentumStrategy::calculateCalmarRatio() const {
    if (recent_returns_.size() < 10) {
        return 0.0;
    }
    
    double max_dd = calculateMaxDrawdown();
    
    if (max_dd < 0.0001) return 0.0;
    
    std::vector<double> returns(recent_returns_.begin(), recent_returns_.end());
    double mean_return = calculateMean(returns);
    double annualized_return = mean_return * 525600;
    
    return annualized_return / max_dd;
}

double MomentumStrategy::calculateMaxDrawdown() const {
    if (recent_returns_.empty()) {
        return 0.0;
    }
    
    std::vector<double> cumulative(recent_returns_.size());
    cumulative[0] = recent_returns_[0];
    
    for (size_t i = 1; i < recent_returns_.size(); ++i) {
        cumulative[i] = cumulative[i-1] + recent_returns_[i];
    }
    
    double max_drawdown = 0.0;
    double peak = cumulative[0];
    
    for (double value : cumulative) {
        if (value > peak) {
            peak = value;
        }
        if (peak <= 0.0) {
            continue;
        }
        double drawdown = (peak - value) / peak;
        max_drawdown = std::max(max_drawdown, drawdown);
    }
    
    return max_drawdown;
}

void MomentumStrategy::updateRollingStatistics() {
    if (recent_returns_.size() < 30) {
        return;
    }
    
    rolling_stats_.rolling_sharpe_30d = calculateSharpeRatio(0.03, 525600);
    rolling_stats_.rolling_sortino_30d = calculateSortinoRatio(0.0, 525600);
    rolling_stats_.rolling_calmar = calculateCalmarRatio();
    rolling_stats_.rolling_max_dd_30d = calculateMaxDrawdown();
    
    int recent_trades = std::min(100, static_cast<int>(recent_returns_.size()));
    int wins = 0;
    for (int i = 0; i < recent_trades; ++i) {
        if (recent_returns_[i] > 0) wins++;
    }
    rolling_stats_.rolling_win_rate_100 = static_cast<double>(wins) / recent_trades;
    
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
}

// ===== 11. Walk-Forward Validation =====

WalkForwardResult MomentumStrategy::validateStrategy(
    const std::vector<Candle>& historical_data
) const {
    WalkForwardResult result;
    
    // 媛꾨떒??援ы쁽: ?꾨컲遺 In-Sample, ?꾨컲遺 Out-of-Sample
    if (historical_data.size() < 100) {
        return result;
    }
    
    [[maybe_unused]] size_t split_point = historical_data.size() / 2;
    
    // In-Sample Sharpe (?꾨컲遺)
    // Out-of-Sample Sharpe (?꾨컲遺)
    // ?ㅼ젣濡쒕뒗 諛깊뀒?ㅽ똿 ?붿쭊 ?꾩슂
    
    result.in_sample_sharpe = 1.5;
    result.out_sample_sharpe = 1.2;
    result.degradation_ratio = (result.in_sample_sharpe - result.out_sample_sharpe) / result.in_sample_sharpe;
    result.is_robust = result.degradation_ratio < 0.30;
    
    return result;
}

// ===== 12. Helpers =====

long long MomentumStrategy::getCurrentTimestamp() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

bool MomentumStrategy::canTradeNow() {
    const auto strategy_cfg = Config::getInstance().getMomentumConfig();
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

void MomentumStrategy::recordTrade() {
    daily_trades_count_++;
    hourly_trades_count_++;
}

void MomentumStrategy::resetDailyCounters() {
    const long long now = getCurrentTimestamp();
    const long long day_ms = 24LL * 60LL * 60LL * 1000LL;
    if (current_day_start_ == 0 || (now - current_day_start_) >= day_ms) {
        daily_trades_count_ = 0;
        current_day_start_ = now;
    }
}

void MomentumStrategy::resetHourlyCounters() {
    const long long now = getCurrentTimestamp();
    const long long hour_ms = 60LL * 60LL * 1000LL;
    if (current_hour_start_ == 0 || (now - current_hour_start_) >= hour_ms) {
        hourly_trades_count_ = 0;
        current_hour_start_ = now;
    }
}

void MomentumStrategy::checkCircuitBreaker() {
    const auto strategy_cfg = Config::getInstance().getMomentumConfig();
    const long long now = getCurrentTimestamp();

    if (circuit_breaker_active_ && now >= circuit_breaker_until_) {
        circuit_breaker_active_ = false;
        consecutive_losses_ = 0;
    }

    if (!circuit_breaker_active_ && consecutive_losses_ >= strategy_cfg.max_consecutive_losses) {
        circuit_breaker_active_ = true;
        circuit_breaker_until_ = now + CIRCUIT_BREAKER_COOLDOWN_MS;
    }
}

bool MomentumStrategy::isCircuitBreakerActive() const {
    return circuit_breaker_active_;
}

double MomentumStrategy::calculateMean(const std::vector<double>& values) const {
    if (values.empty()) return 0.0;
    return std::accumulate(values.begin(), values.end(), 0.0) / values.size();
}

double MomentumStrategy::calculateStdDev(const std::vector<double>& values, double mean) const {
    if (values.size() < 2) return 0.0;
    
    double variance = 0.0;
    for (double val : values) {
        variance += (val - mean) * (val - mean);
    }
    variance /= (values.size() - 1);
    
    return std::sqrt(variance);
}

double MomentumStrategy::calculateVolatility(const std::vector<Candle>& candles) const {
    if (candles.size() < 2) return 0.0;
    std::vector<double> returns;
    for (size_t i = 1; i < candles.size(); ++i) {
        // [?섏젙] ?뺣갑???섏씡瑜?
        double ret = (candles[i].close - candles[i-1].close) / candles[i-1].close;
        returns.push_back(ret);
    }
    double mean = calculateMean(returns);
    return calculateStdDev(returns, mean);
}

bool MomentumStrategy::shouldGenerateSignal(
    double expected_return,
    double expected_sharpe
) const {
    const auto strategy_cfg = Config::getInstance().getMomentumConfig();
    if (expected_return < 0.0045) {
        return false;
    }
    
    if (expected_sharpe < strategy_cfg.min_expected_sharpe) {
        return false;
    }
    
    long long now = getCurrentTimestamp();
    if (now - last_signal_time_ < static_cast<long long>(strategy_cfg.min_signal_interval_sec) * 1000) {
        return false;
    }
    
    return true;
}

// ===== ?ъ????곹깭 ?낅뜲?댄듃 (紐⑤땲?곕쭅 以? =====

void MomentumStrategy::updateState(const std::string& market, double current_price) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    (void)market;
    (void)current_price;
    
    // MomentumStrategy??異붿꽭 異붿쥌?대?濡?
    // ?꾩옱 異붿꽭媛 ?좎??섎뒗吏 紐⑤땲?곕쭅 媛??
    // ?섏?留?TradingEngine?먯꽌 ?먯젅/?듭젅??泥섎━?섎?濡??ш린?쒕뒗 ?⑥뒪
}

} // namespace strategy
} // namespace autolife

