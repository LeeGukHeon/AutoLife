#include "strategy/MeanReversionStrategy.h"
#include "analytics/TechnicalIndicators.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <chrono>
#include <spdlog/spdlog.h>

namespace autolife {
namespace strategy {

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
    const std::vector<analytics::Candle>& candles,
    double current_price)
{
    std::lock_guard<std::mutex> lock(mutex_);
    
    Signal signal;
    signal.type = SignalType::NONE;
    signal.market = market;
    signal.strategy_name = "Mean Reversion";
    
    // [수정] 중복 진입 방지: 이미 진입한 종목이면 빈 신호 리턴
    if (active_positions_.find(market) != active_positions_.end()) {
        return signal;
    }
    
    // ===== 1. 기본 검증 =====
    if (candles.size() < 150) {
        return signal;
    }
    
    if (metrics.liquidity_score < MIN_LIQUIDITY_SCORE) {
        return signal;
    }
    
    // ===== 2. 거래 가능 확인 =====
    if (!canTradeNow()) {
        return signal;
    }
    
    // ===== 3. 서킷 브레이커 =====
    checkCircuitBreaker();
    if (isCircuitBreakerActive()) {
        return signal;
    }
    
    // ===== 4. 신호 간격 =====
    long long now = getCurrentTimestamp();
    if ((now - last_signal_time_) < MIN_SIGNAL_INTERVAL_SEC * 1000) {
        return signal;
    }
    
    // ===== 5. Kalman Filter 업데이트 & 분석 =====
    // (const 함수가 아니므로 내부 상태 업데이트 가능)
    updateKalmanFilter(market, current_price);
    
    MeanReversionSignalMetrics reversion = analyzeMeanReversion(market, metrics, candles, current_price);
    
    if (!shouldGenerateMeanReversionSignal(reversion)) {
        return signal;
    }
    
    // ===== 6. 매수 신호 생성 =====
    signal.type = SignalType::BUY;
    signal.strength = reversion.strength;
    signal.entry_price = current_price;
    
    // ===== 7. 손절/익절 계산 =====
    signal.stop_loss = calculateStopLoss(current_price, candles);
    signal.take_profit = calculateTakeProfit(current_price, candles);
    
    // ===== 8. 포지션 사이징 =====
    double capital = 1000000.0; // 엔진에서 실제 자본금에 맞춰 조정됨
    signal.position_size = calculatePositionSize(capital, current_price, signal.stop_loss, metrics);
    
    // ===== 9. 최소 주문 금액 확인 =====
    double order_amount = capital * signal.position_size;
    if (order_amount < MIN_ORDER_AMOUNT_KRW) {
        signal.type = SignalType::NONE;
        return signal;
    }
    
    // ===== 10. 리스크/리워드 비율 =====
    double expected_return = (signal.take_profit - current_price) / current_price;
    double risk = (current_price - signal.stop_loss) / current_price;
    
    if (risk <= 0) return signal;
    
    double risk_reward = expected_return / risk;
    if (risk_reward < 1.5) {
        signal.type = SignalType::NONE;
        return signal;
    }
    
    // ===== 11. 로깅 =====
    spdlog::info("[MeanReversion] BUY Signal - {} | Strength: {:.3f} | Prob: {:.1f}% | Expected: {:.2f}% | Time: {:.0f}m",
                 market, signal.strength, reversion.reversion_probability * 100,
                 reversion.expected_reversion_pct * 100, reversion.time_to_revert);
    
    last_signal_time_ = now;
    rolling_stats_.total_reversions_detected++;
    
    return signal;
}

// ===== Entry Decision =====

bool MeanReversionStrategy::shouldEnter(
    const std::string& market,
    const analytics::CoinMetrics& metrics,
    const std::vector<analytics::Candle>& candles,
    double current_price)
{
    // generateSignal 호출 (내부 락 있음)
    Signal signal = generateSignal(market, metrics, candles, current_price);
    
    if (signal.type == SignalType::BUY && signal.strength >= MIN_SIGNAL_STRENGTH) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // [수정] 1. 중복 방지 목록 등록
        active_positions_.insert(market);
        
        // Kalman Filter 상태 가져오기
        auto& kalman = getKalmanState(market);
        
        // [수정] 2. 포지션 데이터 초기화 (position_data_ 사용)
        MeanReversionPositionData pos_data;
        pos_data.market = market;
        pos_data.entry_price = current_price;
        pos_data.target_mean = kalman.estimated_mean;
        // 0으로 나누기 방지
        if (kalman.estimated_mean != 0) {
            pos_data.initial_deviation = (current_price - kalman.estimated_mean) / kalman.estimated_mean;
        } else {
            pos_data.initial_deviation = 0.0;
        }
        pos_data.highest_price = current_price;
        pos_data.trailing_stop = calculateStopLoss(current_price, candles); // 초기 스탑
        pos_data.entry_timestamp = getCurrentTimestamp();
        pos_data.tp1_hit = false;
        pos_data.tp2_hit = false;
        
        position_data_[market] = pos_data;
        
        recordTrade();
        
        spdlog::info("[MeanReversion] Position Registered - {} | Entry: {:.2f} | Target: {:.2f}",
                     market, current_price, kalman.estimated_mean);
        
        return true;
    }
    
    return false;
}

// ===== Exit Decision =====

bool MeanReversionStrategy::shouldExit(
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
    
    if (current_price > pos_data.highest_price) {
        pos_data.highest_price = current_price;
    }
    
    // ===== 1. 손절 (Stop Loss) =====
    // pos_data.trailing_stop은 진입 시 초기 손절가로 설정됨
    if (current_price <= pos_data.trailing_stop) {
        spdlog::info("[MeanReversion] Stop Loss / TS - {} | Price: {:.2f} <= {:.2f}", 
                     market, current_price, pos_data.trailing_stop);
        return true; // [중요] 여기서 erase 하지 않음 (엔진이 updateStatistics 호출 시 처리)
    }
    
    // ===== 2. Mean 도달 체크 (목표 평균가 회귀) =====
    double deviation_from_mean = 0.0;
    if (pos_data.target_mean != 0) {
        deviation_from_mean = std::abs(current_price - pos_data.target_mean) / pos_data.target_mean;
    }
    
    // 평균가 근접(0.5% 오차)하고 수익 상태일 때
    if (deviation_from_mean < 0.005 && profit_pct > 0.01) {
        spdlog::info("[MeanReversion] Mean Reached - {} | Profit: {:.2f}% | Deviation: {:.2f}%",
                     market, profit_pct * 100, deviation_from_mean * 100);
        return true;
    }
    
    // ===== 3. 트레일링 스탑 업데이트 =====
    if (profit_pct >= TRAILING_ACTIVATION) {
        double new_trailing = pos_data.highest_price * (1.0 - TRAILING_DISTANCE);
        if (new_trailing > pos_data.trailing_stop) {
            pos_data.trailing_stop = new_trailing;
        }
    }
    
    // ===== 4. 익절 2 (4%) - 전량 청산 =====
    if (!pos_data.tp2_hit && profit_pct >= BASE_TAKE_PROFIT_2) {
        spdlog::info("[MeanReversion] Take Profit 2 - {} | Profit: {:.2f}%", market, profit_pct * 100);
        return true;
    }
    
    // ===== 5. 익절 1 (2%) - 부분 청산 기록 (전량 청산은 아님) =====
    if (!pos_data.tp1_hit && profit_pct >= BASE_TAKE_PROFIT_1 && holding_minutes > 30.0) {
        pos_data.tp1_hit = true;
        spdlog::info("[MeanReversion] Take Profit 1 Hit - {}", market);
        // 여기서 true 리턴 시 전량 청산되므로 false 유지 (엔진이 부분청산 처리)
    }
    
    // ===== 6. 시간 제한 (4시간) =====
    if (holding_minutes >= MAX_HOLDING_TIME_MINUTES) {
        spdlog::info("[MeanReversion] Max Holding Time - {} | Profit: {:.2f}%", market, profit_pct * 100);
        return true;
    }
    
    // ===== 7. Breakeven 이동 =====
    if (shouldMoveToBreakeven(entry_price, current_price)) {
        double breakeven_price = entry_price * 1.002; // 수수료 포함
        if (pos_data.trailing_stop < breakeven_price) {
            pos_data.trailing_stop = breakeven_price;
        }
    }
    
    return false;
}

// ===== Calculate Stop Loss =====

double MeanReversionStrategy::calculateStopLoss(double entry_price, const std::vector<analytics::Candle>& candles) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (candles.size() < 16) return entry_price * (1.0 - BASE_STOP_LOSS);

    std::vector<double> true_ranges;
    // [수정] 가장 마지막(현재) 15개 캔들을 뒤에서부터 훑음
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
    const std::vector<analytics::Candle>& candles)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return entry_price * (1.0 + BASE_TAKE_PROFIT_1);
}

// ===== Calculate Position Size =====

double MeanReversionStrategy::calculatePositionSize(
    double capital,
    double entry_price,
    double stop_loss,
    const analytics::CoinMetrics& metrics)
{
    std::lock_guard<std::mutex> lock(mutex_);
    
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
    
    // Confidence adjustment (통계적 신뢰도 반영)
    double confidence = 0.75;  // 기본값
    double conf_adj = adjustForConfidence(vol_adj, confidence);
    
    double position_size = std::clamp(conf_adj, 0.05, MAX_POSITION_SIZE);
    
    return position_size;
}

// ===== Enabled =====

void MeanReversionStrategy::setEnabled(bool enabled)
{
    std::lock_guard<std::mutex> lock(mutex_);
    enabled_ = enabled;
    spdlog::info("[MeanReversionStrategy] Enabled: {}", enabled);
}

bool MeanReversionStrategy::isEnabled() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return enabled_;
}

// ===== Statistics =====

IStrategy::Statistics MeanReversionStrategy::getStatistics() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

// [수정] market 인자 추가 및 포지션 목록 삭제 로직
void MeanReversionStrategy::updateStatistics(const std::string& market, bool is_win, double profit_loss)
{
    std::lock_guard<std::mutex> lock(mutex_);
    
    // [중요] 포지션 목록에서 제거 (재진입 허용)
    if (active_positions_.erase(market)) {
        // active_positions_에 있었다면 position_data_에도 있을 것임
        position_data_.erase(market);
        spdlog::info("[MeanReversion] Position Cleared - {} (Ready for next trade)", market);
    } else {
        // 혹시 모르니 map에서도 확인 사살
        position_data_.erase(market);
    }
    
    // --- 통계 업데이트 (기존 로직 유지) ---
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

// ===== Trailing Stop =====

double MeanReversionStrategy::updateTrailingStop(
    double entry_price,
    double highest_price,
    double current_price)
{
    std::lock_guard<std::mutex> lock(mutex_);
    
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
    std::lock_guard<std::mutex> lock(mutex_);
    return rolling_stats_;
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

std::vector<analytics::Candle> MeanReversionStrategy::getCachedCandles(
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
        return std::vector<analytics::Candle>();
    }
    
    try {
        nlohmann::json json_data = client_->getCandles(market, "minutes/15", count);
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
        return std::vector<analytics::Candle>();
    }
}

// ===== Trade Frequency Management =====

bool MeanReversionStrategy::canTradeNow()
{
    resetDailyCounters();
    resetHourlyCounters();
    
    if (daily_trades_count_ >= MAX_DAILY_REVERSION_TRADES) {
        return false;
    }
    
    if (hourly_trades_count_ >= MAX_HOURLY_REVERSION_TRADES) {
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
    long long now = getCurrentTimestamp();
    
    if (circuit_breaker_active_ && now >= circuit_breaker_until_) {
        circuit_breaker_active_ = false;
        spdlog::info("[MeanReversion] Circuit breaker deactivated");
    }
    
    if (consecutive_losses_ >= MAX_CONSECUTIVE_LOSSES && !circuit_breaker_active_) {
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
    const std::vector<analytics::Candle>& candles) const
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
    
    // 2. Hurst Exponent (평균회귀 vs 추세 판별)
    stats.hurst_exponent = calculateHurstExponent(candles);
    
    // 3. Half-Life (회귀 속도)
    stats.half_life = calculateHalfLife(candles);
    
    // 4. ADF Test (정상성 검정)
    stats.adf_statistic = calculateADFStatistic(prices);
    stats.is_stationary = (stats.adf_statistic < -2.86);  // 5% significance level
    
    // 5. Autocorrelation (자기상관)
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
    const std::vector<analytics::Candle>& candles) const
{
    if (candles.size() < 100) return 0.5;
    
    std::vector<double> prices = extractPrices(candles, "close");
    
    // Rescaled Range (R/S) Analysis
    std::vector<int> lags = {10, 20, 30, 50, 75, 100};
    std::vector<double> log_rs;
    std::vector<double> log_n;
    
    for (int lag : lags) {
        if (prices.size() < static_cast<size_t>(lag * 2)) continue;
        
        int num_subseries = prices.size() / lag;
        std::vector<double> rs_values;
        
        for (int i = 0; i < num_subseries; i++) {
            std::vector<double> subseries(prices.begin() + i * lag, 
                                          prices.begin() + (i + 1) * lag);
            
            double mean = calculateMean(subseries);
            
            // Cumulative deviations
            std::vector<double> deviations;
            double cumsum = 0.0;
            for (double val : subseries) {
                cumsum += (val - mean);
                deviations.push_back(cumsum);
            }
            
            // Range
            double max_dev = *std::max_element(deviations.begin(), deviations.end());
            double min_dev = *std::min_element(deviations.begin(), deviations.end());
            double range = max_dev - min_dev;
            
            // Standard deviation
            double std_dev = calculateStdDev(subseries, mean);
            
            if (std_dev > 0) {
                rs_values.push_back(range / std_dev);
            }
        }
        
        if (!rs_values.empty()) {
            double avg_rs = calculateMean(rs_values);
            log_rs.push_back(std::log(avg_rs));
            log_n.push_back(std::log(static_cast<double>(lag)));
        }
    }
    
    // Linear regression: log(R/S) = H * log(n) + c
    if (log_rs.size() < 3) return 0.5;
    
    double mean_x = calculateMean(log_n);
    double mean_y = calculateMean(log_rs);
    
    double numerator = 0.0;
    double denominator = 0.0;
    
    for (size_t i = 0; i < log_n.size(); i++) {
        numerator += (log_n[i] - mean_x) * (log_rs[i] - mean_y);
        denominator += (log_n[i] - mean_x) * (log_n[i] - mean_x);
    }
    
    double hurst = (denominator != 0) ? numerator / denominator : 0.5;
    
    return std::clamp(hurst, 0.0, 1.0);
}

double MeanReversionStrategy::calculateHalfLife(
    const std::vector<analytics::Candle>& candles) const
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
    // Simplified version: Δy_t = α + β*y_{t-1} + ε_t
    
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

BollingerBands MeanReversionStrategy::calculateBollingerBands(const std::vector<analytics::Candle>& candles, int period, double std_dev_mult) const {
    BollingerBands bb;
    if (candles.size() < static_cast<size_t>(period)) return bb;

    std::vector<double> prices;
    // [수정] 가장 최근 'period'만큼의 종가를 추출
    for (size_t i = candles.size() - period; i < candles.size(); ++i) {
        prices.push_back(candles[i].close);
    }

    bb.middle = calculateMean(prices);
    double std_dev = calculateStdDev(prices, bb.middle);
    bb.upper = bb.middle + (std_dev_mult * std_dev);
    bb.lower = bb.middle - (std_dev_mult * std_dev);
    
    if (bb.middle > 0) bb.bandwidth = (bb.upper - bb.lower) / bb.middle;
    
    // [수정] 현재가(가장 최신) 기준으로 위치 파악
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
    const std::vector<analytics::Candle>& candles) const
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
    const std::vector<analytics::Candle>& candles,
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
    const std::vector<analytics::Candle>& candles,
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
    // Hurst Exponent 기반 판별
    if (stats.hurst_exponent < HURST_MEAN_REVERTING) {
        return MRMarketRegime::MEAN_REVERTING;
    } else if (stats.hurst_exponent > 0.55) {
        return MRMarketRegime::TRENDING;
    } else if (stats.hurst_exponent >= 0.45 && stats.hurst_exponent <= 0.55) {
        return MRMarketRegime::RANDOM_WALK;
    }
    
    return MRMarketRegime::UNKNOWN;
}

// ===== Mean Reversion Analysis (핵심) =====

MeanReversionSignalMetrics MeanReversionStrategy::analyzeMeanReversion(
    const std::string& market,
    const analytics::CoinMetrics& metrics,
    const std::vector<analytics::Candle>& candles,
    double current_price)
{
    MeanReversionSignalMetrics signal;
    
    // 1. Statistical Metrics 계산
    StatisticalMetrics stats = calculateStatisticalMetrics(candles);
    
    // 2. Market Regime 판별
    signal.regime = detectMarketRegime(stats);
    
    // Mean Reverting 상태가 아니면 거래 안함
    if (signal.regime != MRMarketRegime::MEAN_REVERTING) {
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
    
    // 7. 과매도 확인 (여러 조건 중 하나라도 만족)
    bool is_bb_oversold = (current_price <= bb.lower);
    bool is_rsi_oversold = (rsi.oversold_count >= 2);  // 최소 2개 기간에서 과매도
    bool is_z_score_extreme = (stats.z_score_20 <= Z_SCORE_EXTREME || 
                               stats.z_score_50 <= Z_SCORE_EXTREME);
    bool is_vwap_oversold = (current_price < vwap.vwap_lower);
    bool is_kalman_oversold = (current_price < kalman.estimated_mean * 0.98);
    
    int oversold_signals = (is_bb_oversold ? 1 : 0) + 
                          (is_rsi_oversold ? 1 : 0) + 
                          (is_z_score_extreme ? 1 : 0) + 
                          (is_vwap_oversold ? 1 : 0) + 
                          (is_kalman_oversold ? 1 : 0);
    
    // 최소 3개 이상의 과매도 신호 필요
    if (oversold_signals < 3) {
        return signal;
    }
    
    // 8. Signal Type 결정
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
    
    // 9. Reversion Probability 계산
    signal.reversion_probability = calculateReversionProbability(stats, bb, rsi, vwap);
    
    if (signal.reversion_probability < MIN_REVERSION_PROBABILITY) {
        return signal;
    }
    
    // 10. Expected Reversion Target
    signal.expected_reversion_pct = estimateReversionTarget(current_price, bb, vwap, kalman);
    
    // 11. Time to Revert
    signal.time_to_revert = estimateReversionTime(stats.half_life, 
                                                   std::abs(current_price - kalman.estimated_mean) / kalman.estimated_mean);
    
    // 12. Confidence (통계적 신뢰도)
    signal.confidence = 0.0;
    signal.confidence += (stats.is_stationary ? 0.2 : 0.0);
    signal.confidence += (stats.hurst_exponent < 0.4 ? 0.2 : 0.1);
    signal.confidence += (stats.half_life > 0 && stats.half_life < 50 ? 0.2 : 0.0);
    signal.confidence += (std::abs(stats.autocorrelation) > 0.3 ? 0.2 : 0.0);
    signal.confidence += (oversold_signals >= 4 ? 0.2 : 0.1);
    
    // 13. Signal Strength 계산
    signal.strength = calculateSignalStrength(signal, stats, metrics);
    
    // 14. Validation
    signal.is_valid = (signal.strength >= MIN_SIGNAL_STRENGTH && 
                      signal.reversion_probability >= MIN_REVERSION_PROBABILITY &&
                      signal.confidence >= 0.6);
    
    return signal;
}

double MeanReversionStrategy::calculateReversionProbability(
    const StatisticalMetrics& stats,
    const BollingerBands& bb,
    const MultiPeriodRSI& rsi,
    const VWAPAnalysis& vwap) const
{
    double probability = 0.5;  // Base 50%
    
    // 1. Hurst Exponent (강한 평균회귀일수록 확률 증가)
    if (stats.hurst_exponent < 0.40) {
        probability += 0.20;
    } else if (stats.hurst_exponent < 0.45) {
        probability += 0.10;
    }
    
    // 2. Stationarity (정상성)
    if (stats.is_stationary) {
        probability += 0.10;
    }
    
    // 3. Half-Life (적정 범위)
    if (stats.half_life > 5 && stats.half_life < 50) {
        probability += 0.10;
    }
    
    // 4. Bollinger Bands (하단 돌파)
    if (bb.percent_b < 0.1) {
        probability += 0.15;
    }
    
    // 5. RSI (다중 과매도)
    if (rsi.oversold_count >= 3) {
        probability += 0.15;
    } else if (rsi.oversold_count >= 2) {
        probability += 0.10;
    }
    
    // 6. Z-Score (극단적 과매도)
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
    // 여러 평균의 가중 평균
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
    // Confidence가 높을수록 포지션 증가
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
    if (!metrics.is_valid) {
        return false;
    }
    
    if (metrics.strength < MIN_SIGNAL_STRENGTH) {
        return false;
    }
    
    if (metrics.reversion_probability < MIN_REVERSION_PROBABILITY) {
        return false;
    }
    
    if (metrics.confidence < 0.6) {
        return false;
    }
    
    if (metrics.regime != MRMarketRegime::MEAN_REVERTING) {
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

double MeanReversionStrategy::calculateVolatility(const std::vector<analytics::Candle>& candles) const {
    if (candles.size() < 21) return 0.02;
    
    std::vector<double> returns;
    // [수정] 데이터의 맨 뒤(현재) 20개 구간의 수익률 계산
    for (size_t i = candles.size() - 20; i < candles.size(); ++i) {
        double ret = (candles[i].close - candles[i-1].close) / candles[i-1].close;
        returns.push_back(ret);
    }
    
    return calculateStdDev(returns, calculateMean(returns));
}

std::vector<double> MeanReversionStrategy::extractPrices(
    const std::vector<analytics::Candle>& candles,
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
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

std::vector<analytics::Candle> MeanReversionStrategy::parseCandlesFromJson(
    const nlohmann::json& json_data) const
{
    std::vector<analytics::Candle> candles;
    
    if (!json_data.is_array()) {
        return candles;
    }
    
    for (const auto& item : json_data) {
        analytics::Candle candle;
        
        candle.timestamp = item.value("timestamp", 0LL);
        candle.open = item.value("opening_price", 0.0);
        candle.high = item.value("high_price", 0.0);
        candle.low = item.value("low_price", 0.0);
        candle.close = item.value("trade_price", 0.0);
        candle.volume = item.value("candle_acc_trade_volume", 0.0);
        
        candles.push_back(candle);
    }
    
    std::sort(candles.begin(), candles.end(), 
              [](const analytics::Candle& a, const analytics::Candle& b) {
                  return a.timestamp < b.timestamp;
              });
    
    return candles;
}

} // namespace strategy
} // namespace autolife
