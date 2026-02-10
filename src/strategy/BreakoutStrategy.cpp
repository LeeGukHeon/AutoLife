#include "strategy/BreakoutStrategy.h"
#include "analytics/TechnicalIndicators.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <chrono>
#include <spdlog/spdlog.h>

namespace autolife {
namespace strategy {

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
    const std::vector<analytics::Candle>& candles,
    double current_price,
    double available_capital)
{
    std::lock_guard<std::mutex> lock(mutex_);
    
    Signal signal;
    signal.type = SignalType::NONE;
    signal.market = market;
    signal.strategy_name = "Breakout Strategy"; // 전략명 명시
    
    // [수정] 중복 진입 방지: 이미 진입한 종목이면 신호 생성 안 함
    if (active_positions_.find(market) != active_positions_.end()) {
        return signal;
    }
    
    std::vector<analytics::Candle> candles_5m = resampleTo5m(candles);

    // ===== 1. 기본 검증 =====
    if (candles_5m.size() < 30) {
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
    
    // ===== 5. 돌파 분석 =====
    BreakoutSignalMetrics breakout = analyzeBreakout(market, metrics, candles_5m, current_price);
    
    if (!shouldGenerateBreakoutSignal(breakout)) {
        return signal;
    }
    
    // ===== 6. 매수 신호 생성 =====
    signal.type = SignalType::BUY;
    signal.strength = breakout.strength;
    signal.entry_price = current_price;
    
    // ===== 7. 손절/익절 계산 =====
    signal.stop_loss = calculateStopLoss(current_price, candles_5m);
    double base_tp = calculateTakeProfit(current_price, candles_5m);
    double risk = current_price - signal.stop_loss;
    signal.take_profit_1 = current_price + (risk * 2.0 * 0.5);
    signal.take_profit_2 = base_tp;
    signal.buy_order_type = strategy::OrderTypePolicy::LIMIT_WITH_FALLBACK;
    signal.sell_order_type = strategy::OrderTypePolicy::LIMIT_WITH_FALLBACK;
    signal.max_retries = 3;
    signal.retry_wait_ms = 500;
    
    // ===== 8. 포지션 사이징 =====
    double capital = available_capital;
    if (capital <= 0) {
        spdlog::warn("[Breakout] 가용자본 없음 (신호 무시) - {}", market);
        signal.type = SignalType::NONE;
        return signal;
    }
    signal.position_size = calculatePositionSize(capital, current_price, signal.stop_loss, metrics);
    
    // ===== 9. 최소 주문 금액 확인 =====
    double order_amount = capital * signal.position_size;
    if (order_amount < MIN_ORDER_AMOUNT_KRW) {
        signal.type = SignalType::NONE; // HOLD -> NONE으로 명확화
        return signal;
    }
    
    // ===== 10. 리스크/리워드 비율 =====
    double expected_return = (signal.take_profit_2 - current_price) / current_price;
    // risk는 이미 L120에서 정의됨
    
    if (risk <= 0) {
        signal.type = SignalType::NONE;
        return signal;
    }
    
    double risk_reward = expected_return / risk;
    if (risk_reward < 2.0) {
        signal.type = SignalType::NONE;
        return signal;
    }
    
    // ===== 11. 로깅 =====
    spdlog::info("[Breakout] BUY Signal - {} | Strength: {:.3f} | Size: {:.2f}% | R/R: {:.2f}",
                 market, signal.strength, signal.position_size * 100, risk_reward);
    
    last_signal_time_ = now;
    rolling_stats_.total_breakouts_detected++;
    
    return signal;
}

// ===== Entry Decision =====

bool BreakoutStrategy::shouldEnter(
    const std::string& market,
    const analytics::CoinMetrics& metrics,
    const std::vector<analytics::Candle>& candles,
    double current_price)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (active_positions_.find(market) != active_positions_.end()) {
        return false;
    }

    std::vector<analytics::Candle> candles_5m = resampleTo5m(candles);

    if (candles_5m.size() < 30) {
        return false;
    }

    if (metrics.liquidity_score < MIN_LIQUIDITY_SCORE) {
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
    if ((now - last_signal_time_) < MIN_SIGNAL_INTERVAL_SEC * 1000) {
        return false;
    }

    BreakoutSignalMetrics breakout = analyzeBreakout(market, metrics, candles_5m, current_price);
    return shouldGenerateBreakoutSignal(breakout);
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
    const std::vector<analytics::Candle>& candles)
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
    const std::vector<analytics::Candle>& candles)
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

std::vector<analytics::Candle> BreakoutStrategy::getCachedCandles(
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
        return std::vector<analytics::Candle>();
    }
}

// ===== Trade Frequency Management =====

bool BreakoutStrategy::canTradeNow()
{
    resetDailyCounters();
    resetHourlyCounters();
    
    if (daily_trades_count_ >= MAX_DAILY_BREAKOUT_TRADES) {
        return false;
    }
    
    if (hourly_trades_count_ >= MAX_HOURLY_BREAKOUT_TRADES) {
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
    long long now = getCurrentTimestamp();
    
    if (circuit_breaker_active_ && now >= circuit_breaker_until_) {
        circuit_breaker_active_ = false;
        spdlog::info("[Breakout] Circuit breaker deactivated");
    }
    
    if (consecutive_losses_ >= MAX_CONSECUTIVE_LOSSES && !circuit_breaker_active_) {
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
    const std::vector<analytics::Candle>& candles,
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
    
    if (signal.atr_multiple < MIN_ATR_MULTIPLE) {
        return signal;
    }
    
    // 3. 거래량 확인
    signal.volume_confirmation = calculateVolumeConfirmation(candles);
    if (signal.volume_confirmation < 0.5) {
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
    signal.is_valid = (signal.strength >= MIN_SIGNAL_STRENGTH);
    
    return signal;
}

// ===== Calculate Donchian Channel =====

DonchianChannel BreakoutStrategy::calculateDonchianChannel(const std::vector<analytics::Candle>& candles, int period) const {
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
    const std::vector<analytics::Candle>& candles) const
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
    const std::vector<analytics::Candle>& candles) const
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
    const std::vector<analytics::Candle>& candles) const
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
    const std::vector<analytics::Candle>& candles) const
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
    const std::vector<analytics::Candle>& candles) const
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
    const std::vector<analytics::Candle>& candles,
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
    const std::vector<analytics::Candle>& candles) const
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
    const std::vector<analytics::Candle>& candles) const
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

double BreakoutStrategy::calculateVolumeConfirmation(const std::vector<analytics::Candle>& candles) const {
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
    const std::vector<analytics::Candle>& candles,
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

double BreakoutStrategy::calculateVolatility(const std::vector<analytics::Candle>& candles) const {
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
    
    if (metrics.strength < MIN_SIGNAL_STRENGTH) {
        return false;
    }
    
    if (metrics.false_breakout_probability > FALSE_BREAKOUT_THRESHOLD) {
        return false;
    }
    
    if (metrics.volume_confirmation < 0.5) {
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
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

std::vector<analytics::Candle> BreakoutStrategy::parseCandlesFromJson(
    const nlohmann::json& json_data) const
{
    return analytics::TechnicalIndicators::jsonToCandles(json_data);
}

std::vector<analytics::Candle> BreakoutStrategy::resampleTo5m(const std::vector<analytics::Candle>& candles_1m) const {
    if (candles_1m.size() < 5) return {};
    std::vector<analytics::Candle> candles_5m;
    for (size_t i = 0; i + 5 <= candles_1m.size(); i += 5) {
        analytics::Candle candle_5m;
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