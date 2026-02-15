#include "strategy/MeanReversionStrategy.h"
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
constexpr double kRelaxedMinReversionProb = 0.58;
constexpr double kRelaxedMinConfidence = 0.50;

double clamp01(double v) {
    return std::clamp(v, 0.0, 1.0);
}

std::filesystem::path getMeanReversionAdaptiveStatsPath() {
    return utils::PathUtils::resolveRelativePath("state/mean_reversion_entry_stats.json");
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

const char* meanReversionTypeToLabel(MeanReversionType t) {
    switch (t) {
        case MeanReversionType::BOLLINGER_OVERSOLD: return "BOLLINGER_OVERSOLD";
        case MeanReversionType::RSI_OVERSOLD: return "RSI_OVERSOLD";
        case MeanReversionType::Z_SCORE_EXTREME: return "Z_SCORE_EXTREME";
        case MeanReversionType::KALMAN_DEVIATION: return "KALMAN_DEVIATION";
        case MeanReversionType::VWAP_DEVIATION: return "VWAP_DEVIATION";
        default: return "UNSPECIFIED";
    }
}

int makeMeanReversionEntryKey(MeanReversionType t, analytics::MarketRegime regime) {
    return (static_cast<int>(t) * 100) + static_cast<int>(regime);
}

long long normalizeTimestampMs(long long ts) {
    if (ts > 0 && ts < 1000000000000LL) {
        return ts * 1000LL;
    }
    return ts;
}

bool isMeanReversionRegimeTradable(
    const analytics::CoinMetrics& metrics,
    const analytics::RegimeAnalysis& regime
) {
    if (regime.regime == analytics::MarketRegime::TRENDING_DOWN) {
        return false;
    }
    if (regime.regime == analytics::MarketRegime::HIGH_VOLATILITY &&
        metrics.liquidity_score < 45.0) {
        return false;
    }
    return true;
}

double computeMeanReversionAdaptiveLiquidityFloor(
    const analytics::CoinMetrics& metrics,
    const analytics::RegimeAnalysis& regime,
    double base_floor
) {
    const double vol_t = clamp01(metrics.volatility / 6.0);
    const double volume_relief_t = clamp01((metrics.volume_surge_ratio - 1.0) / 2.0);
    double floor = base_floor + (vol_t * 7.0) - (volume_relief_t * 6.0);

    if (regime.regime == analytics::MarketRegime::HIGH_VOLATILITY) {
        floor += 2.0;
    } else if (regime.regime == analytics::MarketRegime::TRENDING_DOWN) {
        floor += 8.0;
    } else if (regime.regime == analytics::MarketRegime::RANGING) {
        floor -= 2.0;
    }
    return std::clamp(floor, 35.0, 80.0);
}

double computeMeanReversionAdaptiveStrengthFloor(
    const analytics::CoinMetrics& metrics,
    const analytics::RegimeAnalysis& regime,
    double base_floor
) {
    const double vol_t = clamp01(metrics.volatility / 6.0);
    const double liq_stress_t = clamp01((70.0 - metrics.liquidity_score) / 70.0);
    const double floor = base_floor + (vol_t * 0.05) + (liq_stress_t * 0.07);

    if (regime.regime == analytics::MarketRegime::RANGING) {
        return std::clamp(floor - 0.06, 0.28, 0.65);
    }
    if (regime.regime == analytics::MarketRegime::HIGH_VOLATILITY) {
        return std::clamp(floor - 0.03, 0.30, 0.66);
    }
    if (regime.regime == analytics::MarketRegime::TRENDING_DOWN) {
        return std::clamp(floor + 0.05, 0.35, 0.72);
    }
    return std::clamp(floor - 0.01, 0.30, 0.66);
}

double computeMeanReversionAdaptiveRRFloor(
    const analytics::CoinMetrics& metrics,
    const analytics::RegimeAnalysis& regime
) {
    const double vol_t = clamp01(metrics.volatility / 6.0);
    const double liq_t = clamp01((metrics.liquidity_score - 50.0) / 50.0);
    double rr_floor = 1.28 + (vol_t * 0.20) - (liq_t * 0.12);

    if (regime.regime == analytics::MarketRegime::RANGING) {
        rr_floor -= 0.10;
    } else if (regime.regime == analytics::MarketRegime::TRENDING_DOWN) {
        rr_floor += 0.18;
    } else if (regime.regime == analytics::MarketRegime::HIGH_VOLATILITY) {
        rr_floor += 0.05;
    }
    return std::clamp(rr_floor, 1.10, 1.55);
}
}

// ===== Constructor =====

MeanReversionStrategy::MeanReversionStrategy(std::shared_ptr<network::UpbitHttpClient> client)
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
    rolling_stats_.avg_reversion_time = 0.0;
    rolling_stats_.total_reversions_detected = 0;
    rolling_stats_.successful_reversions = 0;
    rolling_stats_.avg_reversion_accuracy = 0.0;
    
    spdlog::info("[MeanReversionStrategy] Initialized - Statistical Mean Reversion + Kalman Filter");
    loadAdaptiveEntryStats();
}

// ===== Strategy Info =====

StrategyInfo MeanReversionStrategy::getInfo() const
{
    StrategyInfo info;
    info.name = "Mean Reversion Strategy";
    info.description = "Statistical mean reversion with Kalman Filter, Hurst Exponent, and multi-signal fusion";
    info.risk_level = 6.0;
    info.timeframe = "15m";
    info.min_capital = 100000.0;
    info.expected_winrate = 0.68;
    return info;
}

// ===== Main Signal Generation =====

Signal MeanReversionStrategy::generateSignal(
    const std::string& market,
    const analytics::CoinMetrics& metrics,
    const std::vector<Candle>& candles,
    double current_price,
    double available_capital,
    const analytics::RegimeAnalysis& regime)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const auto strategy_cfg = Config::getInstance().getMeanReversionConfig();
    
    Signal signal;
    signal.type = SignalType::NONE;
    signal.market = market;
    signal.strategy_name = "Mean Reversion Strategy";
    signal.timestamp = getCurrentTimestamp();
    pending_entry_keys_.erase(market);
    if (!candles.empty()) {
        latest_market_timestamp_ms_ = normalizeTimestampMs(candles.back().timestamp);
        signal.timestamp = latest_market_timestamp_ms_;
    }
    
    // ===== Hard Gates =====
    if (active_positions_.find(market) != active_positions_.end()) return signal;
    if (available_capital <= 0) return signal;
    if (!isMeanReversionRegimeTradable(metrics, regime)) return signal;
    
    std::vector<Candle> candles_5m;
    auto tf_5m_it = metrics.candles_by_tf.find("5m");
    const bool used_preloaded_5m = (tf_5m_it != metrics.candles_by_tf.end() && tf_5m_it->second.size() >= 80);
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
    if (candles_5m.size() < 40) return signal;
    const bool degraded_5m_mode = candles_5m.size() < 80;
    if (!canTradeNow()) return signal;
    
    checkCircuitBreaker();
    if (isCircuitBreakerActive()) return signal;
    
    // ===== Kalman Filter Update =====
    updateKalmanFilter(market, current_price);
    
    // ===== Score-Based Evaluation =====
    double total_score = 0.0;
    
    // --- (1) ?됯퇏 ?뚭? 遺꾩꽍 (理쒕? 0.40) ---
    MeanReversionSignalMetrics reversion = analyzeMeanReversion(market, metrics, candles_5m, current_price);
    const int entry_key = makeMeanReversionEntryKey(reversion.type, regime.regime);
    const double adaptive_bias = getAdaptiveEntryBias(entry_key);
    double min_reversion_prob = 0.70;
    double min_confidence = 0.56;
    double min_expected_reversion = 0.005;
    if (adaptive_bias <= -0.18 && regime.regime != analytics::MarketRegime::RANGING) {
        return signal;
    }
    if (adaptive_bias < 0.0) {
        const double penalty = std::min(0.10, -adaptive_bias * 0.30);
        min_reversion_prob += penalty;
        min_confidence += (penalty * 0.80);
        min_expected_reversion += (penalty * 0.004);
    } else if (adaptive_bias > 0.0) {
        const double bonus = std::min(0.05, adaptive_bias * 0.20);
        min_reversion_prob = std::max(0.58, min_reversion_prob - bonus);
        min_confidence = std::max(0.48, min_confidence - (bonus * 0.80));
        min_expected_reversion = std::max(0.003, min_expected_reversion - (bonus * 0.002));
    }
    const bool reversion_quality_ok =
        reversion.is_valid &&
        reversion.reversion_probability >= min_reversion_prob &&
        reversion.confidence >= min_confidence &&
        reversion.expected_reversion_pct >= min_expected_reversion;
    if (!reversion_quality_ok) {
        return signal;
    }
    total_score += reversion.strength * 0.40;
    
    // --- (2) ?좊룞??(理쒕? 0.10) ---
    const double adaptive_liq_floor = computeMeanReversionAdaptiveLiquidityFloor(
        metrics, regime, strategy_cfg.min_liquidity_score);
    const double liq_floor = adaptive_liq_floor * 0.85;
    if (metrics.liquidity_score >= adaptive_liq_floor) total_score += 0.10;
    else if (metrics.liquidity_score >= liq_floor) total_score += 0.07;
    else if (metrics.liquidity_score >= adaptive_liq_floor * 0.70) total_score += 0.04;
    
    // --- (3) ?덉쭚 (理쒕? 0.15) ---
    double regime_score = 0.0;
    switch (regime.regime) {
        case analytics::MarketRegime::RANGING: regime_score = 0.20; break;
        case analytics::MarketRegime::HIGH_VOLATILITY: regime_score = 0.12; break;
        case analytics::MarketRegime::TRENDING_UP: regime_score = 0.05; break;
        case analytics::MarketRegime::TRENDING_DOWN: regime_score = 0.03; break;
        default: regime_score = 0.05; break;
    }
    total_score += regime_score;
    
    // --- (4) RSI ??쟾 ?좏샇 (理쒕? 0.20) ---
    auto prices = analytics::TechnicalIndicators::extractClosePrices(candles_5m);
    double rsi = analytics::TechnicalIndicators::calculateRSI(prices, 14);
    
    if (rsi < 30) total_score += 0.20;         // 媛뺥븳 怨쇰ℓ??
    else if (rsi < 40) total_score += 0.12;     // 怨쇰ℓ??
    else if (rsi < 50) total_score += 0.05;     // ?쎄컙 ??쓬
    // ?됯퇏 ?뚭???怨쇰ℓ?꾩뿉??留ㅼ닔?섎?濡?RSI ?믪쑝硫??먯닔 ?놁쓬
    
    // --- (5) 嫄곕옒???뺤씤 (理쒕? 0.15) ---
    if (metrics.volume_surge_ratio >= 2.0) total_score += 0.15;
    else if (metrics.volume_surge_ratio >= 1.0) total_score += 0.08;
    else total_score += 0.03;

    if (reversion.strength < 0.30 &&
        (regime.regime == analytics::MarketRegime::RANGING ||
         regime.regime == analytics::MarketRegime::HIGH_VOLATILITY) &&
        metrics.liquidity_score >= adaptive_liq_floor * 0.90 &&
        metrics.volume_surge_ratio >= 0.85 &&
        rsi >= 30.0 && rsi <= 55.0 &&
        std::abs(metrics.price_change_rate) <= 1.2) {
        total_score += 0.14;
    }
    
    // ===== 理쒖쥌 ?먯젙 =====
    if (degraded_5m_mode) {
        total_score *= 0.92;
    }
    signal.strength = std::clamp(total_score, 0.0, 1.0);

    const double effective_strength_floor = computeMeanReversionAdaptiveStrengthFloor(
        metrics, regime, strategy_cfg.min_signal_strength) + (degraded_5m_mode ? 0.04 : 0.0);
    double adaptive_strength_floor = effective_strength_floor;
    if (adaptive_bias < 0.0) {
        adaptive_strength_floor = std::min(0.90, adaptive_strength_floor + std::min(0.10, -adaptive_bias * 0.35));
    } else if (adaptive_bias > 0.0) {
        adaptive_strength_floor = std::max(0.28, adaptive_strength_floor - std::min(0.05, adaptive_bias * 0.25));
    }
    const bool fallback_entry_ok =
        (regime.regime == analytics::MarketRegime::RANGING) &&
        (metrics.liquidity_score >= adaptive_liq_floor * 0.88) &&
        (metrics.volume_surge_ratio >= 0.80) &&
        (rsi >= 28.0 && rsi <= 50.0) &&
        (metrics.price_change_rate <= 0.8 && metrics.price_change_rate >= -1.5);
    if (signal.strength < adaptive_strength_floor) {
        if (!fallback_entry_ok) {
            return signal;
        }
        signal.strength = adaptive_strength_floor;
    }
    
    // ===== ?좏샇 ?앹꽦 =====
    signal.type = SignalType::BUY;
    signal.entry_price = current_price;
    signal.stop_loss = calculateStopLoss(current_price, candles_5m);
    const double risk_mr = std::max(0.0, current_price - signal.stop_loss);
    const double stop_risk = std::max(1e-9, risk_mr / current_price);
    const double rr_floor = computeMeanReversionAdaptiveRRFloor(metrics, regime);
    const double tp2_from_rr_pct = stop_risk * std::max(1.25, rr_floor + 0.05);
    const double tp2_from_reversion_pct = std::max(0.0, reversion.expected_reversion_pct * 0.85);
    const double target_tp2_pct = std::clamp(
        std::max({BASE_TAKE_PROFIT_2, tp2_from_rr_pct, tp2_from_reversion_pct}),
        BASE_TAKE_PROFIT_1,
        0.08
    );
    const double target_tp1_pct = std::clamp(
        std::max(BASE_TAKE_PROFIT_1, stop_risk * 0.90),
        BASE_TAKE_PROFIT_1,
        target_tp2_pct * 0.75
    );
    signal.take_profit_1 = current_price * (1.0 + target_tp1_pct);
    signal.take_profit_2 = current_price * (1.0 + target_tp2_pct);
    signal.buy_order_type = strategy::OrderTypePolicy::LIMIT_WITH_FALLBACK;
    signal.sell_order_type = strategy::OrderTypePolicy::LIMIT_WITH_FALLBACK;
    signal.max_retries = 3;
    signal.retry_wait_ms = 800;
    
    // ?ъ????ъ씠吏?
    signal.position_size = calculatePositionSize(available_capital, current_price, signal.stop_loss, metrics);
    const double min_size_for_order = (available_capital > 0.0)
        ? (MIN_ORDER_AMOUNT_KRW / available_capital)
        : 1.0;
    if (signal.position_size < min_size_for_order) {
        signal.position_size = min_size_for_order;
    }
    signal.position_size = std::clamp(signal.position_size, 0.0, 1.0);

    const double expected_return = (signal.take_profit_2 - signal.entry_price) / signal.entry_price;
    const double reward_risk = expected_return / stop_risk;
    if (reward_risk < rr_floor) {
        signal.type = SignalType::NONE;
        return signal;
    }

    double order_amount = available_capital * signal.position_size;
    if (order_amount < MIN_ORDER_AMOUNT_KRW) {
        signal.type = SignalType::NONE;
        return signal;
    }
    
    if (signal.strength >= 0.70) {
        signal.type = SignalType::STRONG_BUY;
    }
    signal.entry_archetype = meanReversionTypeToLabel(reversion.type);
    pending_entry_keys_[market] = entry_key;
    
    spdlog::info("[MeanReversion] ?렞 BUY Signal - {} | Strength: {:.3f} | RSI: {:.1f}",
                 market, signal.strength, rsi);
    
    last_signal_time_ = getCurrentTimestamp();
    rolling_stats_.total_reversions_detected++;
    
    return signal;
}

// ===== Entry Decision =====

bool MeanReversionStrategy::shouldEnter(
    const std::string& market,
    const analytics::CoinMetrics& metrics,
    const std::vector<Candle>& candles,
    double current_price,
    const analytics::RegimeAnalysis& regime)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const auto strategy_cfg = Config::getInstance().getMeanReversionConfig();
    if (!candles.empty()) {
        latest_market_timestamp_ms_ = normalizeTimestampMs(candles.back().timestamp);
    }

    if (active_positions_.find(market) != active_positions_.end()) {
        return false;
    }

    std::vector<Candle> candles_5m;
    auto tf_5m_it = metrics.candles_by_tf.find("5m");
    if (tf_5m_it != metrics.candles_by_tf.end() && tf_5m_it->second.size() >= 80) {
        candles_5m = tf_5m_it->second;
    } else {
        candles_5m = resampleTo5m(candles);
    }

    if (candles_5m.size() < 40) {
        return false;
    }

    if (!isMeanReversionRegimeTradable(metrics, regime)) {
        return false;
    }

    const double adaptive_liq_floor = computeMeanReversionAdaptiveLiquidityFloor(
        metrics, regime, strategy_cfg.min_liquidity_score);
    if (metrics.liquidity_score < adaptive_liq_floor * 0.85) {
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

    updateKalmanFilter(market, current_price);

    MeanReversionSignalMetrics reversion = analyzeMeanReversion(market, metrics, candles_5m, current_price);
    return shouldGenerateMeanReversionSignal(reversion);
}

bool MeanReversionStrategy::onSignalAccepted(const Signal& signal, double allocated_capital)
{
    (void)allocated_capital;

    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (signal.market.empty()) {
        return false;
    }

    if (active_positions_.find(signal.market) != active_positions_.end()) {
        return false;
    }

    updateKalmanFilter(signal.market, signal.entry_price);
    auto& kalman = getKalmanState(signal.market);

    MeanReversionPositionData pos_data;
    pos_data.market = signal.market;
    pos_data.entry_price = signal.entry_price;
    pos_data.target_mean = kalman.estimated_mean;
    if (kalman.estimated_mean != 0.0) {
        pos_data.initial_deviation = (signal.entry_price - kalman.estimated_mean) / kalman.estimated_mean;
    } else {
        pos_data.initial_deviation = 0.0;
    }
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

    spdlog::info("[MeanReversion] Position Registered - {} | Entry: {:.2f} | Target: {:.2f}",
                 signal.market, signal.entry_price, kalman.estimated_mean);
    return true;
}

// ===== Exit Decision =====

bool MeanReversionStrategy::shouldExit(
    const std::string& market,
    double entry_price,
    double current_price,
    double holding_time_seconds)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    // [?섏젙] position_data_ 留??ъ슜
    if (position_data_.find(market) == position_data_.end()) {
        return false;
    }
    
    auto& pos_data = position_data_[market];
    
    double profit_pct = (current_price - entry_price) / entry_price;
    double holding_minutes = holding_time_seconds / 60.0;
    
    if (current_price > pos_data.highest_price) {
        pos_data.highest_price = current_price;
    }
    
    // ===== 1. ?먯젅 (Stop Loss) =====
    // pos_data.trailing_stop? 吏꾩엯 ??珥덇린 ?먯젅媛濡??ㅼ젙??
    if (current_price <= pos_data.trailing_stop) {
        spdlog::info("[MeanReversion] Stop Loss / TS - {} | Price: {:.2f} <= {:.2f}", 
                     market, current_price, pos_data.trailing_stop);
        return true; // [以묒슂] ?ш린??erase ?섏? ?딆쓬 (?붿쭊??updateStatistics ?몄텧 ??泥섎━)
    }
    
    // ===== 2. Mean ?꾨떖 泥댄겕 (紐⑺몴 ?됯퇏媛 ?뚭?) =====
    double deviation_from_mean = 0.0;
    if (pos_data.target_mean != 0) {
        deviation_from_mean = std::abs(current_price - pos_data.target_mean) / pos_data.target_mean;
    }
    
    // ?됯퇏媛 洹쇱젒(0.5% ?ㅼ감)?섍퀬 ?섏씡 ?곹깭????
    if (deviation_from_mean < 0.005 && profit_pct > 0.01) {
        spdlog::info("[MeanReversion] Mean Reached - {} | Profit: {:.2f}% | Deviation: {:.2f}%",
                     market, profit_pct * 100, deviation_from_mean * 100);
        return true;
    }
    
    // ===== 3. ?몃젅?쇰쭅 ?ㅽ깙 ?낅뜲?댄듃 =====
    if (profit_pct >= TRAILING_ACTIVATION) {
        double new_trailing = pos_data.highest_price * (1.0 - TRAILING_DISTANCE);
        if (new_trailing > pos_data.trailing_stop) {
            pos_data.trailing_stop = new_trailing;
        }
    }
    
    // ===== 4. ?듭젅 2 (4%) - ?꾨웾 泥?궛 =====
    if (!pos_data.tp2_hit && profit_pct >= BASE_TAKE_PROFIT_2) {
        spdlog::info("[MeanReversion] Take Profit 2 - {} | Profit: {:.2f}%", market, profit_pct * 100);
        return true;
    }
    
    // ===== 5. ?듭젅 1 (2%) - 遺遺?泥?궛 湲곕줉 (?꾨웾 泥?궛? ?꾨떂) =====
    if (!pos_data.tp1_hit && profit_pct >= BASE_TAKE_PROFIT_1 && holding_minutes > 30.0) {
        pos_data.tp1_hit = true;
        spdlog::info("[MeanReversion] Take Profit 1 Hit - {}", market);
        // ?ш린??true 由ы꽩 ???꾨웾 泥?궛?섎?濡?false ?좎? (?붿쭊??遺遺꾩껌??泥섎━)
    }
    
    // ===== 6. ?쒓컙 ?쒗븳 (4?쒓컙) =====
    if (holding_minutes >= MAX_HOLDING_TIME_MINUTES) {
        spdlog::info("[MeanReversion] Max Holding Time - {} | Profit: {:.2f}%", market, profit_pct * 100);
        return true;
    }
    
    // ===== 7. Breakeven ?대룞 =====
    if (shouldMoveToBreakeven(entry_price, current_price)) {
        double breakeven_price = entry_price * 1.002; // ?섏닔猷??ы븿
        if (pos_data.trailing_stop < breakeven_price) {
            pos_data.trailing_stop = breakeven_price;
        }
    }
    
    return false;
}

// ===== Calculate Stop Loss =====

double MeanReversionStrategy::calculateStopLoss(double entry_price, const std::vector<Candle>& candles) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (candles.size() < 16) return entry_price * (1.0 - BASE_STOP_LOSS);

    std::vector<double> true_ranges;
    // [?섏젙] 媛??留덉?留??꾩옱) 15媛?罹붾뱾???ㅼ뿉?쒕????묒쓬
    size_t end_idx = candles.size();
    for (size_t i = end_idx - 1; i > end_idx - 15; --i) {
        double high_low = candles[i].high - candles[i].low;
        double high_close = std::abs(candles[i].high - candles[i-1].close);
        double low_close = std::abs(candles[i].low - candles[i-1].close);
        true_ranges.push_back(std::max({high_low, high_close, low_close}));
    }
    
    double atr = calculateMean(true_ranges);
    double stop_pct = std::clamp(atr / entry_price * 1.5, BASE_STOP_LOSS, 0.04);
    
    return entry_price * (1.0 - stop_pct);
}

// ===== Calculate Take Profit =====

double MeanReversionStrategy::calculateTakeProfit(
    double entry_price,
    const std::vector<Candle>& candles)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    (void)candles;  // candles ?뚮씪誘명꽣 誘몄궗??
    return entry_price * (1.0 + BASE_TAKE_PROFIT_2);
}

// ===== Calculate Position Size =====

double MeanReversionStrategy::calculatePositionSize(
    double capital,
    double entry_price,
    double stop_loss,
    const analytics::CoinMetrics& metrics)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    (void)capital;  // capital ?뚮씪誘명꽣 誘몄궗??
    (void)metrics;  // metrics ?뚮씪誘명꽣 誘몄궗??
    
    double risk_pct = (entry_price - stop_loss) / entry_price;
    
    if (risk_pct <= 0) {
        return 0.05;
    }
    
    double win_rate = stats_.win_rate > 0 ? stats_.win_rate : 0.68;
    double avg_win = stats_.avg_profit > 0 ? stats_.avg_profit : BASE_TAKE_PROFIT_1;
    double avg_loss = stats_.avg_loss > 0 ? stats_.avg_loss : BASE_STOP_LOSS;
    
    double kelly = calculateKellyFraction(win_rate, avg_win, avg_loss);
    double half_kelly = kelly * 0.5;
    
    double volatility = calculateVolatility(metrics.candles);
    double vol_adj = adjustForVolatility(half_kelly, volatility);
    
    // Confidence adjustment (?듦퀎???좊ː??諛섏쁺)
    double confidence = 0.75;  // 湲곕낯媛?
    double conf_adj = adjustForConfidence(vol_adj, confidence);
    
    double position_size = std::clamp(conf_adj, 0.05, MAX_POSITION_SIZE);
    
    return position_size;
}

// ===== Enabled =====

void MeanReversionStrategy::setEnabled(bool enabled)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    enabled_ = enabled;
    spdlog::info("[MeanReversionStrategy] Enabled: {}", enabled);
}

bool MeanReversionStrategy::isEnabled() const
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return enabled_;
}

// ===== Statistics =====

IStrategy::Statistics MeanReversionStrategy::getStatistics() const
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return stats_;
}

// [?섏젙] market ?몄옄 異붽? 諛??ъ???紐⑸줉 ??젣 濡쒖쭅
void MeanReversionStrategy::updateStatistics(const std::string& market, bool is_win, double profit_loss)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    int closed_entry_key = 0;
    auto key_it = active_entry_keys_.find(market);
    if (key_it != active_entry_keys_.end()) {
        closed_entry_key = key_it->second;
        active_entry_keys_.erase(key_it);
    }
    pending_entry_keys_.erase(market);
    
    // [以묒슂] ?ъ???紐⑸줉?먯꽌 ?쒓굅 (?ъ쭊???덉슜)
    if (active_positions_.erase(market)) {
        // active_positions_???덉뿀?ㅻ㈃ position_data_?먮룄 ?덉쓣 寃껋엫
        position_data_.erase(market);
        spdlog::info("[MeanReversion] Position Cleared - {} (Ready for next trade)", market);
    } else {
        // ?뱀떆 紐⑤Ⅴ??map?먯꽌???뺤씤 ?ъ궡
        position_data_.erase(market);
    }
    
    // --- ?듦퀎 ?낅뜲?댄듃 (湲곗〈 濡쒖쭅 ?좎?) ---
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
    if (closed_entry_key != 0) {
        recordAdaptiveEntryOutcome(closed_entry_key, is_win, profit_loss);
    }
}

void MeanReversionStrategy::setStatistics(const Statistics& stats)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    stats_ = stats;
}

// ===== Trailing Stop =====

double MeanReversionStrategy::updateTrailingStop(
    double entry_price,
    double highest_price,
    double current_price)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    (void)current_price;  // current_price ?뚮씪誘명꽣 誘몄궗??
    
    double profit_pct = (highest_price - entry_price) / entry_price;
    
    if (profit_pct >= TRAILING_ACTIVATION) {
        return highest_price * (1.0 - TRAILING_DISTANCE);
    }
    
    return 0.0;
}

// ===== Breakeven =====

bool MeanReversionStrategy::shouldMoveToBreakeven(
    double entry_price,
    double current_price)
{
    double profit_pct = (current_price - entry_price) / entry_price;
    return profit_pct >= BREAKEVEN_TRIGGER;
}

// ===== Rolling Statistics =====

MeanReversionRollingStatistics MeanReversionStrategy::getRollingStatistics() const
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return rolling_stats_;
}

double MeanReversionStrategy::getAdaptiveEntryBias(int entry_key) const
{
    const auto it = adaptive_entry_stats_.find(entry_key);
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

void MeanReversionStrategy::recordAdaptiveEntryOutcome(int entry_key, bool is_win, double profit_loss)
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

void MeanReversionStrategy::loadAdaptiveEntryStats()
{
    if (disableAdaptiveEntryStateIo()) {
        spdlog::info("[MeanReversion] Adaptive entry stats I/O disabled by environment.");
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    try {
        const auto path = getMeanReversionAdaptiveStatsPath();
        if (!std::filesystem::exists(path)) {
            return;
        }
        std::ifstream in(path.string(), std::ios::binary);
        if (!in.is_open()) {
            spdlog::warn("[MeanReversion] Failed to open adaptive stats file: {}", path.string());
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
            "[MeanReversion] Adaptive entry stats loaded: path={} entries={}",
            path.string(),
            adaptive_entry_stats_.size()
        );
    } catch (const std::exception& e) {
        spdlog::warn("[MeanReversion] Failed to load adaptive entry stats: {}", e.what());
    }
}

void MeanReversionStrategy::saveAdaptiveEntryStats() const
{
    if (disableAdaptiveEntryStateIo()) {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    try {
        const auto path = getMeanReversionAdaptiveStatsPath();
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
            spdlog::warn("[MeanReversion] Failed to open adaptive stats for write: {}", path.string());
            return;
        }
        out << payload.dump(2);
    } catch (const std::exception& e) {
        spdlog::warn("[MeanReversion] Failed to save adaptive entry stats: {}", e.what());
    }
}

void MeanReversionStrategy::updateRollingStatistics()
{
    rolling_stats_.rolling_win_rate = stats_.win_rate;
    
    if (!recent_holding_times_.empty()) {
        double sum = std::accumulate(recent_holding_times_.begin(), recent_holding_times_.end(), 0.0);
        rolling_stats_.avg_holding_time_minutes = (sum / recent_holding_times_.size()) / 60.0;
    }
    
    if (!reversion_time_history_.empty()) {
        double sum = std::accumulate(reversion_time_history_.begin(), reversion_time_history_.end(), 0.0);
        rolling_stats_.avg_reversion_time = (sum / reversion_time_history_.size()) / 60.0;
    }
    
    rolling_stats_.rolling_profit_factor = stats_.profit_factor;
    
    if (rolling_stats_.total_reversions_detected > 0) {
        rolling_stats_.avg_reversion_accuracy = 
            static_cast<double>(rolling_stats_.successful_reversions) / rolling_stats_.total_reversions_detected;
    }
    
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

bool MeanReversionStrategy::canMakeOrderBookAPICall() const
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

bool MeanReversionStrategy::canMakeCandleAPICall() const
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

void MeanReversionStrategy::recordAPICall() const
{
    long long now = getCurrentTimestamp();
    api_call_timestamps_.push_back(now);
    
    while (!api_call_timestamps_.empty() && (now - api_call_timestamps_.front()) > 5000) {
        api_call_timestamps_.pop_front();
    }
}

nlohmann::json MeanReversionStrategy::getCachedOrderBook(const std::string& market)
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
        spdlog::warn("[MeanReversion] Failed to fetch orderbook: {}", e.what());
    }
    
    return cached_orderbook_;
}

std::vector<Candle> MeanReversionStrategy::getCachedCandles(
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
        nlohmann::json json_data = client_->getCandles(market, "15", count);
        auto candles = parseCandlesFromJson(json_data);
        
        candle_cache_[market] = candles;
        candle_cache_time_[market] = now;
        recordAPICall();
        return candles;
    } catch (const std::exception& e) {
        spdlog::warn("[MeanReversion] Failed to fetch candles: {}", e.what());
        if (candle_cache_.count(market)) {
            return candle_cache_[market];
        }
        return std::vector<Candle>();
    }
}

// ===== Trade Frequency Management =====

bool MeanReversionStrategy::canTradeNow()
{
    const auto strategy_cfg = Config::getInstance().getMeanReversionConfig();
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

void MeanReversionStrategy::recordTrade()
{
    daily_trades_count_++;
    hourly_trades_count_++;
    
    long long now = getCurrentTimestamp();
    trade_timestamps_.push_back(now);
    
    if (trade_timestamps_.size() > 100) {
        trade_timestamps_.pop_front();
    }
}

void MeanReversionStrategy::resetDailyCounters()
{
    long long now = getCurrentTimestamp();
    long long day_ms = 24 * 60 * 60 * 1000;
    
    if (current_day_start_ == 0 || (now - current_day_start_) >= day_ms) {
        daily_trades_count_ = 0;
        current_day_start_ = now;
    }
}

void MeanReversionStrategy::resetHourlyCounters()
{
    long long now = getCurrentTimestamp();
    long long hour_ms = 60 * 60 * 1000;
    
    if (current_hour_start_ == 0 || (now - current_hour_start_) >= hour_ms) {
        hourly_trades_count_ = 0;
        current_hour_start_ = now;
    }
}

// ===== Circuit Breaker =====

void MeanReversionStrategy::checkCircuitBreaker()
{
    const auto strategy_cfg = Config::getInstance().getMeanReversionConfig();
    long long now = getCurrentTimestamp();
    
    if (circuit_breaker_active_ && now >= circuit_breaker_until_) {
        circuit_breaker_active_ = false;
        spdlog::info("[MeanReversion] Circuit breaker deactivated");
    }
    
    if (consecutive_losses_ >= strategy_cfg.max_consecutive_losses && !circuit_breaker_active_) {
        activateCircuitBreaker();
    }
}

void MeanReversionStrategy::activateCircuitBreaker()
{
    circuit_breaker_active_ = true;
    circuit_breaker_until_ = getCurrentTimestamp() + CIRCUIT_BREAKER_COOLDOWN_MS;
    
    spdlog::warn("[MeanReversion] Circuit breaker activated - {} consecutive losses", 
                 consecutive_losses_);
}

bool MeanReversionStrategy::isCircuitBreakerActive() const
{
    return circuit_breaker_active_;
}

// ===== Kalman Filter =====

void MeanReversionStrategy::updateKalmanFilter(
    const std::string& market,
    double observed_price)
{
    auto& state = getKalmanState(market);
    
    // Prediction step
    double predicted_mean = state.estimated_mean;
    double predicted_variance = state.estimated_variance + state.process_noise;
    
    // Update step
    state.kalman_gain = predicted_variance / (predicted_variance + state.measurement_noise);
    state.prediction_error = observed_price - predicted_mean;
    
    state.estimated_mean = predicted_mean + state.kalman_gain * state.prediction_error;
    state.estimated_variance = (1.0 - state.kalman_gain) * predicted_variance;
}

KalmanFilterState& MeanReversionStrategy::getKalmanState(const std::string& market)
{
    if (kalman_states_.find(market) == kalman_states_.end()) {
        kalman_states_[market] = KalmanFilterState();
    }
    return kalman_states_[market];
}

// ===== Statistical Analysis =====

StatisticalMetrics MeanReversionStrategy::calculateStatisticalMetrics(
    const std::vector<Candle>& candles) const
{
    StatisticalMetrics stats;
    
    if (candles.size() < 100) {
        return stats;
    }
    
    std::vector<double> prices = extractPrices(candles, "close");
    
    // 1. Multi-period Z-Scores
    if (prices.size() >= 20) {
        std::vector<double> recent_20(prices.end() - 20, prices.end());
        stats.z_score_20 = calculateZScore(prices.back(), recent_20);
    }
    
    if (prices.size() >= 50) {
        std::vector<double> recent_50(prices.end() - 50, prices.end());
        stats.z_score_50 = calculateZScore(prices.back(), recent_50);
    }
    
    if (prices.size() >= 100) {
        std::vector<double> recent_100(prices.end() - 100, prices.end());
        stats.z_score_100 = calculateZScore(prices.back(), recent_100);
    }
    
    // 2. Hurst Exponent (?됯퇏?뚭? vs 異붿꽭 ?먮퀎)
    stats.hurst_exponent = calculateHurstExponent(candles);
    
    // 3. Half-Life (?뚭? ?띾룄)
    stats.half_life = calculateHalfLife(candles);
    
    // 4. ADF Test (?뺤긽??寃??
    stats.adf_statistic = calculateADFStatistic(prices);
    stats.is_stationary = (stats.adf_statistic < -2.86);  // 5% significance level
    
    // 5. Autocorrelation (?먭린?곴?)
    stats.autocorrelation = calculateAutocorrelation(prices, 1);
    
    return stats;
}

double MeanReversionStrategy::calculateZScore(
    double value,
    const std::vector<double>& history) const
{
    if (history.empty()) return 0.0;
    
    double mean = calculateMean(history);
    double std_dev = calculateStdDev(history, mean);
    
    if (std_dev == 0) return 0.0;
    
    return (value - mean) / std_dev;
}

double MeanReversionStrategy::calculateHurstExponent(
    const std::vector<Candle>& candles) const
{
    // ?곗씠??遺議???湲곕낯媛?(Random Walk)
    if (candles.size() < 100) return 0.5;
    
    std::vector<double> prices = extractPrices(candles, "close");
    
    // Rescaled Range (R/S) Analysis
    std::vector<int> lags = {10, 20, 30, 50, 75, 100};
    std::vector<double> log_rs;
    std::vector<double> log_n;
    
    for (int lag : lags) {
        if (prices.size() < static_cast<size_t>(lag * 2)) continue;
        
        int num_subseries = static_cast<int>(prices.size() / lag);
        std::vector<double> rs_values;
        rs_values.reserve(num_subseries); // 硫붾え由??덉빟 (?깅뒫 ?μ긽)
        
        for (int i = 0; i < num_subseries; i++) {
            // ?쒕툕?쒕━利?異붿텧
            auto start_it = prices.begin() + i * lag;
            auto end_it = start_it + lag;
            
            // 1. ?됯퇏 怨꾩궛 (STL ?ъ슜?쇰줈 理쒖쟻??
            double sum = std::accumulate(start_it, end_it, 0.0);
            double mean = sum / lag;
            
            // 2. ?쒖??몄감 & Range ?숈떆 怨꾩궛 (猷⑦봽 ?듯빀)
            double sum_sq_diff = 0.0;
            double current_cumsum = 0.0;
            double max_cumsum = -1e9; // 留ㅼ슦 ?묒? ?섎줈 珥덇린??
            double min_cumsum = 1e9;  // 留ㅼ슦 ???섎줈 珥덇린??
            
            // R/S 遺꾩꽍?먯꽌???꾩쟻 ?몄감???쒖옉?먯쓣 0?쇰줈 媛꾩＜?섎뒗 寃껋씠 ?뺥솗??
            max_cumsum = std::max(max_cumsum, 0.0); 
            min_cumsum = std::min(min_cumsum, 0.0);

            for (auto it = start_it; it != end_it; ++it) {
                double val = *it;
                double diff = val - mean;
                
                // ?쒖??몄감??
                sum_sq_diff += diff * diff;
                
                // Range??(?꾩쟻 ??
                current_cumsum += diff;
                if (current_cumsum > max_cumsum) max_cumsum = current_cumsum;
                if (current_cumsum < min_cumsum) min_cumsum = current_cumsum;
            }
            
            double variance = sum_sq_diff / (lag - 1); // Sample Variance
            double std_dev = std::sqrt(variance);
            double range = max_cumsum - min_cumsum;
            
            // [?덉쟾?μ튂] ?쒖??몄감??Range媛 0??媛源뚯슦硫?臾댁떆 (log(0) 諛⑹?)
            if (std_dev > 1e-9 && range > 1e-9) {
                rs_values.push_back(range / std_dev);
            }
        }
        
        if (!rs_values.empty()) {
            double avg_rs = calculateMean(rs_values);
            
            // [?덉쟾?μ튂] 理쒖쥌 ?됯퇏???좏슚???뚮쭔 濡쒓렇 怨꾩궛
            if (avg_rs > 1e-9) {
                log_rs.push_back(std::log(avg_rs));
                log_n.push_back(std::log(static_cast<double>(lag)));
            }
        }
    }
    
    // Linear regression: log(R/S) = H * log(n) + c
    // ?먯씠 理쒖냼 3媛쒕뒗 ?덉뼱???좊ː?????덉쓬
    if (log_rs.size() < 3) return 0.5;
    
    double mean_x = calculateMean(log_n);
    double mean_y = calculateMean(log_rs);
    
    double numerator = 0.0;
    double denominator = 0.0;
    
    for (size_t i = 0; i < log_n.size(); i++) {
        double dx = log_n[i] - mean_x;
        double dy = log_rs[i] - mean_y;
        
        numerator += dx * dy;
        denominator += dx * dx;
    }
    
    // 遺꾨え媛 0??寃쎌슦 諛⑹?
    if (std::abs(denominator) < 1e-9) return 0.5;

    double hurst = numerator / denominator;
    
    // 寃곌낵媛??대옩??(0.0 ~ 1.0)
    return std::clamp(hurst, 0.0, 1.0);
}

double MeanReversionStrategy::calculateHalfLife(
    const std::vector<Candle>& candles) const
{
    if (candles.size() < 50) return 0.0;
    
    std::vector<double> prices = extractPrices(candles, "close");
    
    // Ornstein-Uhlenbeck process: dy = theta * (mu - y) * dt + sigma * dW
    // Half-life = ln(2) / theta
    
    std::vector<double> price_changes;
    std::vector<double> lagged_prices;
    
    for (size_t i = 1; i < prices.size(); i++) {
        price_changes.push_back(prices[i] - prices[i-1]);
        lagged_prices.push_back(prices[i-1]);
    }
    
    // Linear regression: price_change = alpha + theta * lagged_price
    double mean_x = calculateMean(lagged_prices);
    double mean_y = calculateMean(price_changes);
    
    double numerator = 0.0;
    double denominator = 0.0;
    
    for (size_t i = 0; i < lagged_prices.size(); i++) {
        numerator += (lagged_prices[i] - mean_x) * (price_changes[i] - mean_y);
        denominator += (lagged_prices[i] - mean_x) * (lagged_prices[i] - mean_x);
    }
    
    double theta = (denominator != 0) ? -numerator / denominator : 0.0;
    
    if (theta <= 0) return 0.0;
    
    double half_life = std::log(2.0) / theta;
    
    return std::clamp(half_life, 0.0, 1000.0);
}

double MeanReversionStrategy::calculateADFStatistic(
    const std::vector<double>& prices) const
{
    if (prices.size() < 50) return 0.0;
    
    // Augmented Dickey-Fuller Test
    // Simplified version: ?y_t = 慣 + 棺*y_{t-1} + 琯_t
    
    std::vector<double> diff;
    std::vector<double> lagged;
    
    for (size_t i = 1; i < prices.size(); i++) {
        diff.push_back(prices[i] - prices[i-1]);
        lagged.push_back(prices[i-1]);
    }
    
    double mean_x = calculateMean(lagged);
    double mean_y = calculateMean(diff);
    
    double numerator = 0.0;
    double denominator = 0.0;
    
    for (size_t i = 0; i < lagged.size(); i++) {
        numerator += (lagged[i] - mean_x) * (diff[i] - mean_y);
        denominator += (lagged[i] - mean_x) * (lagged[i] - mean_x);
    }
    
    double beta = (denominator != 0) ? numerator / denominator : 0.0;
    
    // Calculate standard error
    double sum_sq_error = 0.0;
    for (size_t i = 0; i < lagged.size(); i++) {
        double predicted = mean_y + beta * (lagged[i] - mean_x);
        double error = diff[i] - predicted;
        sum_sq_error += error * error;
    }
    
    double std_error = std::sqrt(sum_sq_error / (lagged.size() - 2));
    double se_beta = std_error / std::sqrt(denominator);
    
    double t_stat = (se_beta != 0) ? beta / se_beta : 0.0;
    
    return t_stat;
}

double MeanReversionStrategy::calculateAutocorrelation(
    const std::vector<double>& prices,
    int lag) const
{
    if (prices.size() < static_cast<size_t>(lag + 10)) return 0.0;
    
    double mean = calculateMean(prices);
    
    double numerator = 0.0;
    double denominator = 0.0;
    
    for (size_t i = lag; i < prices.size(); i++) {
        numerator += (prices[i] - mean) * (prices[i - lag] - mean);
    }
    
    for (size_t i = 0; i < prices.size(); i++) {
        denominator += (prices[i] - mean) * (prices[i] - mean);
    }
    
    return (denominator != 0) ? numerator / denominator : 0.0;
}

// ===== Bollinger Bands =====

BollingerBands MeanReversionStrategy::calculateBollingerBands(const std::vector<Candle>& candles, int period, double std_dev_mult) const {
    BollingerBands bb;
    if (candles.size() < static_cast<size_t>(period)) return bb;

    std::vector<double> prices;
    // [?섏젙] 媛??理쒓렐 'period'留뚰겮??醫낃?瑜?異붿텧
    for (size_t i = candles.size() - period; i < candles.size(); ++i) {
        prices.push_back(candles[i].close);
    }

    bb.middle = calculateMean(prices);
    double std_dev = calculateStdDev(prices, bb.middle);
    bb.upper = bb.middle + (std_dev_mult * std_dev);
    bb.lower = bb.middle - (std_dev_mult * std_dev);
    
    if (bb.middle > 0) bb.bandwidth = (bb.upper - bb.lower) / bb.middle;
    
    // [?섏젙] ?꾩옱媛(媛??理쒖떊) 湲곗??쇰줈 ?꾩튂 ?뚯븙
    double current_price = candles.back().close;
    if (bb.upper != bb.lower) bb.percent_b = (current_price - bb.lower) / (bb.upper - bb.lower);

    return bb;
}

bool MeanReversionStrategy::isBBSqueeze(const BollingerBands& bb) const
{
    return bb.bandwidth < BB_SQUEEZE_THRESHOLD;
}

// ===== Multi-Period RSI =====

MultiPeriodRSI MeanReversionStrategy::calculateMultiPeriodRSI(
    const std::vector<Candle>& candles) const
{
    MultiPeriodRSI rsi;
    
    if (candles.size() >= 30) {
        rsi.rsi_7 = calculateRSI(candles, 7);
        rsi.rsi_14 = calculateRSI(candles, 14);
        rsi.rsi_21 = calculateRSI(candles, 21);
        
        // Weighted composite
        rsi.rsi_composite = (rsi.rsi_7 * 0.2 + rsi.rsi_14 * 0.5 + rsi.rsi_21 * 0.3);
        
        rsi.oversold_7 = (rsi.rsi_7 < RSI_OVERSOLD);
        rsi.oversold_14 = (rsi.rsi_14 < RSI_OVERSOLD);
        rsi.oversold_21 = (rsi.rsi_21 < RSI_OVERSOLD);
        
        rsi.oversold_count = (rsi.oversold_7 ? 1 : 0) + 
                            (rsi.oversold_14 ? 1 : 0) + 
                            (rsi.oversold_21 ? 1 : 0);
    }
    
    return rsi;
}

double MeanReversionStrategy::calculateRSI(
    const std::vector<Candle>& candles,
    int period) const
{
    if (candles.size() < static_cast<size_t>(period + 1)) {
        return 50.0;
    }
    
    std::vector<double> gains;
    std::vector<double> losses;
    
    for (size_t i = candles.size() - period; i < candles.size(); i++) {
        double change = candles[i].close - candles[i-1].close;
        if (change > 0) {
            gains.push_back(change);
            losses.push_back(0);
        } else {
            gains.push_back(0);
            losses.push_back(std::abs(change));
        }
    }
    
    double avg_gain = calculateMean(gains);
    double avg_loss = calculateMean(losses);
    
    if (avg_loss == 0) return 100.0;
    
    double rs = avg_gain / avg_loss;
    double rsi = 100.0 - (100.0 / (1.0 + rs));
    
    return rsi;
}

// ===== VWAP Analysis =====

VWAPAnalysis MeanReversionStrategy::calculateVWAPAnalysis(
    const std::vector<Candle>& candles,
    double current_price) const
{
    VWAPAnalysis vwap;
    
    if (candles.size() < 20) {
        return vwap;
    }
    
    double sum_pv = 0.0;
    double sum_v = 0.0;
    std::vector<double> deviations;
    
    // Calculate VWAP
    for (const auto& candle : candles) {
        double typical_price = (candle.high + candle.low + candle.close) / 3.0;
        sum_pv += typical_price * candle.volume;
        sum_v += candle.volume;
    }
    
    vwap.vwap = (sum_v > 0) ? sum_pv / sum_v : current_price;
    
    // Calculate standard deviation of price from VWAP
    for (const auto& candle : candles) {
        double typical_price = (candle.high + candle.low + candle.close) / 3.0;
        deviations.push_back(std::abs(typical_price - vwap.vwap));
    }
    
    double std_dev = calculateStdDev(deviations, calculateMean(deviations));
    
    vwap.vwap_upper = vwap.vwap + std_dev;
    vwap.vwap_lower = vwap.vwap - std_dev;
    
    if (vwap.vwap > 0) {
        vwap.current_deviation_pct = (current_price - vwap.vwap) / vwap.vwap;
        vwap.deviation_z_score = (std_dev > 0) ? 
            (current_price - vwap.vwap) / std_dev : 0.0;
    }
    
    return vwap;
}

// ===== Market Regime Detection =====

MRMarketRegime MeanReversionStrategy::detectMarketRegime(
    const StatisticalMetrics& stats) const
{
    // Hurst Exponent 湲곕컲 ?먮퀎
    if (stats.hurst_exponent < HURST_MEAN_REVERTING) {
        return MRMarketRegime::MEAN_REVERTING;
    } else if (stats.hurst_exponent > 0.55) {
        return MRMarketRegime::TRENDING;
    } else if (stats.hurst_exponent >= 0.45 && stats.hurst_exponent <= 0.55) {
        return MRMarketRegime::RANDOM_WALK;
    }
    
    return MRMarketRegime::UNKNOWN;
}

// ===== Mean Reversion Analysis (?듭떖) =====

MeanReversionSignalMetrics MeanReversionStrategy::analyzeMeanReversion(
    const std::string& market,
    const analytics::CoinMetrics& metrics,
    const std::vector<Candle>& candles,
    double current_price)
{
    MeanReversionSignalMetrics signal;
    
    // 1. Statistical Metrics 怨꾩궛
    StatisticalMetrics stats = calculateStatisticalMetrics(candles);
    
    // 2. Market Regime ?먮퀎
    signal.regime = detectMarketRegime(stats);
    
    // Mean Reverting ?곹깭媛 ?꾨땲硫?嫄곕옒 ?덊븿
    if (signal.regime == MRMarketRegime::UNKNOWN) {
        return signal;
    }
    
    // 3. Bollinger Bands
    BollingerBands bb = calculateBollingerBands(candles, static_cast<int>(BB_PERIOD), BB_STD_DEV);
    
    // 4. Multi-Period RSI
    MultiPeriodRSI rsi = calculateMultiPeriodRSI(candles);
    
    // 5. VWAP Analysis
    VWAPAnalysis vwap = calculateVWAPAnalysis(candles, current_price);
    
    // 6. Kalman Filter State
    auto& kalman = getKalmanState(market);
    
    // 7. 怨쇰ℓ???뺤씤 (?щ윭 議곌굔 以??섎굹?쇰룄 留뚯”)
    bool is_bb_oversold = (current_price <= bb.lower);
    bool is_rsi_oversold = (rsi.oversold_count >= 2);  // 理쒖냼 2媛?湲곌컙?먯꽌 怨쇰ℓ??
    bool is_z_score_extreme = (stats.z_score_20 <= Z_SCORE_EXTREME || 
                               stats.z_score_50 <= Z_SCORE_EXTREME);
    bool is_vwap_oversold = (current_price < vwap.vwap_lower);
    bool is_kalman_oversold = (current_price < kalman.estimated_mean * 0.98);
    
    int oversold_signals = (is_bb_oversold ? 1 : 0) + 
                          (is_rsi_oversold ? 1 : 0) + 
                          (is_z_score_extreme ? 1 : 0) + 
                          (is_vwap_oversold ? 1 : 0) + 
                          (is_kalman_oversold ? 1 : 0);
    
    // 理쒖냼 3媛??댁긽??怨쇰ℓ???좏샇 ?꾩슂
    int min_oversold_signals = 2;
    if (signal.regime == MRMarketRegime::TRENDING) {
        min_oversold_signals = 3;
    } else if (signal.regime == MRMarketRegime::RANDOM_WALK) {
        min_oversold_signals = 2;
    }
    if (oversold_signals < min_oversold_signals) {
        return signal;
    }
    
    // 8. Signal Type 寃곗젙
    if (is_bb_oversold && is_rsi_oversold) {
        signal.type = MeanReversionType::BOLLINGER_OVERSOLD;
    } else if (is_z_score_extreme) {
        signal.type = MeanReversionType::Z_SCORE_EXTREME;
    } else if (is_kalman_oversold) {
        signal.type = MeanReversionType::KALMAN_DEVIATION;
    } else if (is_vwap_oversold) {
        signal.type = MeanReversionType::VWAP_DEVIATION;
    } else {
        signal.type = MeanReversionType::RSI_OVERSOLD;
    }
    
    // 9. Reversion Probability 怨꾩궛
    signal.reversion_probability = calculateReversionProbability(stats, bb, rsi, vwap);
    
    double prob_floor = kRelaxedMinReversionProb;
    if (signal.regime == MRMarketRegime::RANDOM_WALK) {
        prob_floor = 0.55;
    } else if (signal.regime == MRMarketRegime::TRENDING) {
        prob_floor = 0.64;
    }
    if (signal.reversion_probability < prob_floor) {
        return signal;
    }
    
    // 10. Expected Reversion Target
    signal.expected_reversion_pct = estimateReversionTarget(current_price, bb, vwap, kalman);
    
    // 11. Time to Revert
    signal.time_to_revert = estimateReversionTime(stats.half_life, 
                                                   std::abs(current_price - kalman.estimated_mean) / kalman.estimated_mean);
    
    // 12. Confidence (?듦퀎???좊ː??
    signal.confidence = 0.0;
    signal.confidence += (stats.is_stationary ? 0.2 : 0.0);
    signal.confidence += (stats.hurst_exponent < 0.4 ? 0.2 : 0.1);
    signal.confidence += (stats.half_life > 0 && stats.half_life < 50 ? 0.2 : 0.0);
    signal.confidence += (std::abs(stats.autocorrelation) > 0.3 ? 0.2 : 0.0);
    signal.confidence += (oversold_signals >= 4 ? 0.2 : 0.1);
    if (signal.regime == MRMarketRegime::TRENDING) {
        signal.confidence -= 0.05;
    }
    signal.confidence = std::clamp(signal.confidence, 0.0, 1.0);
    
    // 13. Signal Strength 怨꾩궛
    signal.strength = calculateSignalStrength(signal, stats, metrics);
    
    // 14. Validation
    const auto strategy_cfg = Config::getInstance().getMeanReversionConfig();
    const double base_strength_floor = std::max(0.30, strategy_cfg.min_signal_strength);
    double valid_strength_floor = base_strength_floor;
    if (signal.regime == MRMarketRegime::RANDOM_WALK) {
        valid_strength_floor = std::max(0.30, base_strength_floor - 0.06);
    } else if (signal.regime == MRMarketRegime::TRENDING) {
        valid_strength_floor = std::max(0.36, base_strength_floor + 0.04);
    }
    const bool trending_reversion_quality =
        signal.regime != MRMarketRegime::TRENDING ||
        (oversold_signals >= 4 &&
         signal.expected_reversion_pct >= 0.004 &&
         stats.hurst_exponent <= 0.62);
    signal.is_valid = (signal.strength >= valid_strength_floor &&
                      signal.reversion_probability >= prob_floor &&
                      signal.confidence >= kRelaxedMinConfidence &&
                      trending_reversion_quality);
    
    return signal;
}

double MeanReversionStrategy::calculateReversionProbability(
    const StatisticalMetrics& stats,
    const BollingerBands& bb,
    const MultiPeriodRSI& rsi,
    const VWAPAnalysis& vwap) const
{
    double probability = 0.5;  // Base 50%
    
    // 1. Hurst Exponent (媛뺥븳 ?됯퇏?뚭??쇱닔濡??뺣쪧 利앷?)
    if (stats.hurst_exponent < 0.40) {
        probability += 0.20;
    } else if (stats.hurst_exponent < 0.45) {
        probability += 0.10;
    }
    
    // 2. Stationarity (?뺤긽??
    if (stats.is_stationary) {
        probability += 0.10;
    }
    
    // 3. Half-Life (?곸젙 踰붿쐞)
    if (stats.half_life > 5 && stats.half_life < 50) {
        probability += 0.10;
    }
    
    // 4. Bollinger Bands (?섎떒 ?뚰뙆)
    if (bb.percent_b < 0.1) {
        probability += 0.15;
    }
    
    // 5. RSI (?ㅼ쨷 怨쇰ℓ??
    if (rsi.oversold_count >= 3) {
        probability += 0.15;
    } else if (rsi.oversold_count >= 2) {
        probability += 0.10;
    }
    
    // 6. Z-Score (洹밸떒??怨쇰ℓ??
    if (stats.z_score_50 <= -2.5) {
        probability += 0.15;
    } else if (stats.z_score_50 <= -2.0) {
        probability += 0.10;
    }
    
    // 7. VWAP Deviation
    if (vwap.deviation_z_score < -1.5) {
        probability += 0.10;
    }
    
    return std::clamp(probability, 0.0, 0.95);
}

double MeanReversionStrategy::estimateReversionTarget(
    double current_price,
    const BollingerBands& bb,
    const VWAPAnalysis& vwap,
    const KalmanFilterState& kalman) const
{
    // ?щ윭 ?됯퇏??媛以??됯퇏
    std::vector<double> targets;
    std::vector<double> weights;
    
    // Bollinger Middle
    targets.push_back(bb.middle);
    weights.push_back(0.3);
    
    // VWAP
    targets.push_back(vwap.vwap);
    weights.push_back(0.3);
    
    // Kalman Estimated Mean
    targets.push_back(kalman.estimated_mean);
    weights.push_back(0.4);
    
    double weighted_target = 0.0;
    double sum_weights = 0.0;
    
    for (size_t i = 0; i < targets.size(); i++) {
        weighted_target += targets[i] * weights[i];
        sum_weights += weights[i];
    }
    
    double target_price = weighted_target / sum_weights;
    
    return (target_price - current_price) / current_price;
}

double MeanReversionStrategy::estimateReversionTime(
    double half_life,
    double current_deviation) const
{
    (void)current_deviation;  // current_deviation ?뚮씪誘명꽣 誘몄궗??
    
    if (half_life <= 0 || half_life > 100) {
        return 120.0;  // Default 2 hours
    }
    
    // Exponential decay: deviation(t) = deviation(0) * exp(-lambda * t)
    // lambda = ln(2) / half_life
    
    double lambda = std::log(2.0) / half_life;
    
    // Time to reach 10% of current deviation
    double target_deviation = 0.1;
    double time = -std::log(target_deviation) / lambda;
    
    // Convert to minutes (assuming 1-minute candles)
    return std::clamp(time, 10.0, 240.0);
}

double MeanReversionStrategy::calculateSignalStrength(
    const MeanReversionSignalMetrics& metrics,
    const StatisticalMetrics& stats,
    const analytics::CoinMetrics& coin_metrics) const
{
    double strength = 0.0;
    
    // 1. Reversion Probability (40%)
    strength += metrics.reversion_probability * 0.40;
    
    // 2. Confidence (20%)
    strength += metrics.confidence * 0.20;
    
    // 3. Expected Return (15%)
    double normalized_return = std::min(std::abs(metrics.expected_reversion_pct) / 0.03, 1.0);
    strength += normalized_return * 0.15;
    
    // 4. Hurst Exponent (10%)
    double hurst_score = std::max(0.0, (0.5 - stats.hurst_exponent) / 0.5);
    strength += hurst_score * 0.10;
    
    // 5. Liquidity (10%)
    double liquidity_score = std::min(coin_metrics.liquidity_score / 100.0, 1.0);
    strength += liquidity_score * 0.10;
    
    // 6. Time Factor (5%)
    double time_score = std::clamp(120.0 / metrics.time_to_revert, 0.0, 1.0);
    strength += time_score * 0.05;

    double spread_penalty = 0.0;
    if (coin_metrics.orderbook_snapshot.valid) {
        double normalized_spread = std::clamp(coin_metrics.orderbook_snapshot.spread_pct / 0.003, 0.0, 1.0);
        spread_penalty = normalized_spread * 0.10;
    }
    strength -= spread_penalty;
    
    return std::clamp(strength, 0.0, 1.0);
}

// ===== Position Sizing =====

double MeanReversionStrategy::calculateKellyFraction(
    double win_rate,
    double avg_win,
    double avg_loss) const
{
    if (avg_loss == 0) return 0.0;
    
    double b = avg_win / avg_loss;
    double kelly = (win_rate * b - (1 - win_rate)) / b;
    
    return std::clamp(kelly, 0.0, 1.0);
}

double MeanReversionStrategy::adjustForVolatility(
    double kelly_size,
    double volatility) const
{
    double vol_factor = 1.0 - std::min(volatility / 0.05, 0.5);
    return kelly_size * vol_factor;
}

double MeanReversionStrategy::adjustForConfidence(
    double base_size,
    double confidence) const
{
    // Confidence媛 ?믪쓣?섎줉 ?ъ???利앷?
    double conf_multiplier = 0.5 + (confidence * 0.5);
    return base_size * conf_multiplier;
}

// ===== Risk Management =====

double MeanReversionStrategy::calculateMeanReversionCVaR(
    double position_size,
    double volatility) const
{
    double z_score = 1.645;  // 95% confidence
    return position_size * volatility * z_score;
}

bool MeanReversionStrategy::isWorthTrading(
    const MeanReversionSignalMetrics& signal,
    double expected_return) const
{
    // Expected value > 0
    double ev = signal.reversion_probability * expected_return - 
                (1 - signal.reversion_probability) * BASE_STOP_LOSS;
    
    return ev > 0.005;  // Minimum 0.5% expected value
}

// ===== Signal Validation =====

bool MeanReversionStrategy::shouldGenerateMeanReversionSignal(
    const MeanReversionSignalMetrics& metrics) const
{
    const auto strategy_cfg = Config::getInstance().getMeanReversionConfig();
    if (!metrics.is_valid) {
        return false;
    }

    double required_strength = std::max(0.30, strategy_cfg.min_signal_strength);
    if (metrics.regime == MRMarketRegime::RANDOM_WALK) {
        required_strength = std::max(0.30, required_strength - 0.06);
    } else if (metrics.regime == MRMarketRegime::TRENDING) {
        required_strength = std::max(0.36, required_strength + 0.02);
    }
    if (metrics.strength < required_strength) {
        return false;
    }
    
    if (metrics.reversion_probability < kRelaxedMinReversionProb) {
        return false;
    }
    
    if (metrics.confidence < kRelaxedMinConfidence) {
        return false;
    }
    
    const bool regime_ok =
        (metrics.regime == MRMarketRegime::MEAN_REVERTING) ||
        (metrics.regime == MRMarketRegime::RANDOM_WALK && metrics.confidence >= 0.58) ||
        (metrics.regime == MRMarketRegime::TRENDING &&
         metrics.confidence >= 0.66 &&
         metrics.reversion_probability >= 0.66 &&
         metrics.expected_reversion_pct >= 0.004);
    if (!regime_ok) {
        return false;
    }
    
    return true;
}

// ===== Helpers =====

double MeanReversionStrategy::calculateMean(const std::vector<double>& values) const
{
    if (values.empty()) return 0.0;
    return std::accumulate(values.begin(), values.end(), 0.0) / values.size();
}

double MeanReversionStrategy::calculateStdDev(
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

double MeanReversionStrategy::calculateVolatility(const std::vector<Candle>& candles) const {
    if (candles.size() < 21) return 0.02;
    
    std::vector<double> returns;
    // [?섏젙] ?곗씠?곗쓽 留????꾩옱) 20媛?援ш컙???섏씡瑜?怨꾩궛
    for (size_t i = candles.size() - 20; i < candles.size(); ++i) {
        double ret = (candles[i].close - candles[i-1].close) / candles[i-1].close;
        returns.push_back(ret);
    }
    
    return calculateStdDev(returns, calculateMean(returns));
}

std::vector<double> MeanReversionStrategy::extractPrices(
    const std::vector<Candle>& candles,
    const std::string& type) const
{
    std::vector<double> prices;
    
    for (const auto& candle : candles) {
        if (type == "close") {
            prices.push_back(candle.close);
        } else if (type == "open") {
            prices.push_back(candle.open);
        } else if (type == "high") {
            prices.push_back(candle.high);
        } else if (type == "low") {
            prices.push_back(candle.low);
        } else {
            prices.push_back(candle.close);
        }
    }
    
    return prices;
}

long long MeanReversionStrategy::getCurrentTimestamp() const
{
    if (latest_market_timestamp_ms_ > 0) {
        return latest_market_timestamp_ms_;
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

std::vector<Candle> MeanReversionStrategy::parseCandlesFromJson(
    const nlohmann::json& json_data) const
{
    return analytics::TechnicalIndicators::jsonToCandles(json_data);
}

std::vector<Candle> MeanReversionStrategy::resampleTo5m(const std::vector<Candle>& candles_1m) const {
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

} // namespace strategy
} // namespace autolife
