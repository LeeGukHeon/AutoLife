#include "strategy/ScalpingStrategy.h"
#include "analytics/TechnicalIndicators.h"
#include "common/Logger.h"
#include <chrono>
#include <algorithm>
#include <numeric>
#include <cmath>

namespace autolife {
namespace strategy {

// ===== 생성자 & 기본 메서드 =====

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
    LOG_INFO("Advanced Scalping Strategy 초기화 (업비트 API 최적화)");
    
    stats_ = Statistics();
    rolling_stats_ = ScalpingRollingStatistics();
    microstate_model_ = MicrostateModel();
    
    // 시간 카운터 초기화
    current_day_start_ = getCurrentTimestamp();
    current_hour_start_ = getCurrentTimestamp();
}

StrategyInfo ScalpingStrategy::getInfo() const {
    StrategyInfo info;
    info.name = "Advanced Scalping";
    info.description = "금융공학 기반 초단타 전략 (Kelly, HMM, API 최적화)";
    info.timeframe = "1m-3m";
    info.min_capital = 50000;
    info.expected_winrate = 0.65;
    info.risk_level = 7.0;
    return info;
}

Signal ScalpingStrategy::generateSignal(
    const std::string& market,
    const analytics::CoinMetrics& metrics,
    const std::vector<analytics::Candle>& candles,
    double current_price,
    double available_capital)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    // [추가] 1. 이미 진입한 상태면 매수 신호 금지
    if (active_positions_.find(market) != active_positions_.end()) {
        Signal signal;
        signal.market = market;
        // 이미 보유 중이므로 매수 신호는 안 됨. (매도 로직은 별도 shouldExit에서 처리하거나 여기서 처리)
        // 여기서는 빈 신호 리턴하여 엔진이 RiskManager를 통해 청산하게 유도
        return signal; 
    }

    Signal signal;
    signal.market = market;
    signal.strategy_name = "Advanced Scalping";
    signal.timestamp = getCurrentTimestamp();
    
    LOG_INFO("{} - [Scalping] 분석 시작", market);
    
    // 서킷 브레이커 체크
    checkCircuitBreaker();
    if (isCircuitBreakerActive()) {
        LOG_INFO("{} - [Scalping] 서킷 브레이커 활성화 중", market);
        return signal;
    }
    
    // 거래 빈도 체크
    if (!canTradeNow()) {
        LOG_INFO("{} - [Scalping] 거래 빈도 제한 (일일: {}/{}, 시간당: {}/{})", 
                 market, daily_trades_count_, MAX_DAILY_SCALPING_TRADES,
                 hourly_trades_count_, MAX_HOURLY_SCALPING_TRADES);
        return signal;
    }
    
    if (candles.size() < 30) {
        LOG_INFO("{} - [Scalping] 캔들 부족: {}", market, candles.size());
        return signal;
    }
    
    LOG_INFO("{} - [Scalping] shouldEnter() 체크 시작...", market);
    if (!shouldEnter(market, metrics, candles, current_price)) {
        LOG_INFO("{} - [Scalping] shouldEnter() 실패", market);
        return signal;
    }
    
    LOG_INFO("{} - [Scalping] Market Microstate 체크...", market);
    
    // 1. Market Microstate
    MarketMicrostate microstate = detectMarketMicrostate(candles);
    if (microstate != MarketMicrostate::OVERSOLD_BOUNCE && 
        microstate != MarketMicrostate::MOMENTUM_SPIKE &&
        microstate != MarketMicrostate::BREAKOUT &&
        microstate != MarketMicrostate::CONSOLIDATION) {
        LOG_INFO("{} - [Scalping] Microstate 부적합: {}", market, static_cast<int>(microstate));
        return signal;
    }
    
    LOG_INFO("{} - [Scalping] Multi-Timeframe 체크...", market);
    
    // 2. Multi-Timeframe
    auto mtf_signal = analyzeScalpingTimeframes(candles);
    if (mtf_signal.alignment_score < 0.4) {
        LOG_INFO("{} - [Scalping] MTF 정렬 부족: {:.2f}", market, mtf_signal.alignment_score);
        return signal;
    }
    
    LOG_INFO("{} - [Scalping] Order Flow 체크...", market);
    
    // 3. Ultra-Fast Order Flow
    auto order_flow = analyzeUltraFastOrderFlow(metrics, current_price);
    if (order_flow.microstructure_score < 0.4) {
        LOG_INFO("{} - [Scalping] 미세구조 점수 부족: {:.2f}", market, order_flow.microstructure_score);
        return signal;
    }
    
    // 4. Signal Strength
    signal.strength = calculateScalpingSignalStrength(
        metrics, candles, mtf_signal, order_flow, microstate
    );
    
    if (signal.strength < 0.65) {
        LOG_INFO("{} - 신호 강도 부족: {:.2f}", market, signal.strength);
        return signal;
    }
    
    // 5. Dynamic Stops
    auto stops = calculateScalpingDynamicStops(current_price, candles);
    
    signal.type = SignalType::BUY;
    signal.entry_price = current_price;
    signal.stop_loss = stops.stop_loss;
    signal.take_profit_1 = stops.take_profit_1;
    signal.take_profit_2 = stops.take_profit_2;
    signal.buy_order_type = strategy::OrderTypePolicy::LIMIT_WITH_FALLBACK;
    signal.sell_order_type = strategy::OrderTypePolicy::LIMIT_WITH_FALLBACK;
    signal.max_retries = 3;
    signal.retry_wait_ms = 500;
    
    // 6. Worth Scalping (거래 비용 체크)
    double expected_return = (signal.take_profit_2 - signal.entry_price) / signal.entry_price;
    double expected_sharpe = calculateScalpingSharpeRatio();
    
    if (!isWorthScalping(expected_return, expected_sharpe)) {
       LOG_INFO("{} - 진입 포기: 예상수익({:.4f}), R/R({:.2f}), Sharpe({:.2f})", 
             market, expected_return, (expected_return / BASE_STOP_LOSS), expected_sharpe);
    return signal;
    }
    
    // 7. Position Sizing - 전략은 엔진에서 전달된 실제 가용자본을 사용
    double current_capital = available_capital;
    if (current_capital <= 0) {
        LOG_WARN("{} - [Scalping] 가용자본 없음 (신호 무시)", market);
        return signal;
    }

    auto pos_metrics = calculateScalpingPositionSize(
        current_capital, signal.entry_price, signal.stop_loss, metrics, candles
    );
    
    // [✅ 근본적 해결 로직]
    // 1. 현재 엔진이 가진 진짜 돈과 전략이 믿는 가짜 돈의 비율을 구합니다.
    // (예: 100만 원 / 3.8만 원 = 약 25.7배)
    double gap_ratio = engine_config_.initial_capital / current_capital;
    (void)gap_ratio;  // 미래 사용을 위해 미리 계산

    // 2. 비중을 결정할 때, 업비트 최소 주문(5,200원)이 진짜 돈에서 얼마의 비중인지 계산합니다.
    // (예: 5,200 / 38,792 = 약 0.134)
    double real_min_ratio = 5200.0 / current_capital;

    // 3. 만약 전략이 준 비중이 실제 최소 주문 비중보다 작다면?
    if (pos_metrics.final_position_size < real_min_ratio) {
        // 강제로 실제 주문이 나갈 수 있는 비중으로 덮어씁니다.
        pos_metrics.final_position_size = real_min_ratio;
        LOG_INFO("{} - [소액 시드 보정] 비중 상향: {:.4f}", market, real_min_ratio);
    }

    signal.position_size = std::min(1.0, pos_metrics.final_position_size);
    
    // 최소 주문 금액 체크 (업비트: 5,000원)
    double order_amount = current_capital * signal.position_size;
    LOG_INFO("{} - [검증] 예상 주문금액: {:.0f}원 (자본: {:.0f}, 비중: {:.4f})", 
          market, order_amount, current_capital, signal.position_size);
          
    if (order_amount < MIN_ORDER_AMOUNT_KRW) {
        LOG_INFO("{} - 최소 주문 금액 미달: {:.0f}원 < 5,000원", market, order_amount);
        return signal;
    }
    
    // 8. Signal Interval
    if (!shouldGenerateScalpingSignal(expected_return, expected_sharpe)) {
        LOG_INFO("{} - 신호 간격 제한", market);
        return signal;
    }
    
    signal.reason = fmt::format(
        "Scalping: State={}, MTF={:.0f}%, Flow={:.0f}%, Str={:.0f}%, Size={:.1f}%",
        static_cast<int>(microstate),
        mtf_signal.alignment_score * 100,
        order_flow.microstructure_score * 100,
        signal.strength * 100,
        pos_metrics.final_position_size * 100
    );
    
    if (signal.strength >= 0.80) {
        signal.type = SignalType::STRONG_BUY;
    }
    
    last_signal_time_ = getCurrentTimestamp();
    
    LOG_INFO("스캘핑 신호: {} - 강도 {:.2f}, 포지션 {:.1f}%",
             market, signal.strength, signal.position_size * 100);
    
    return signal;
}

bool ScalpingStrategy::shouldEnter(
    const std::string& market,
    const analytics::CoinMetrics& metrics,
    const std::vector<analytics::Candle>& candles,
    double current_price
) {
    (void)current_price;
    
    if (candles.size() < 30) return false;
    
    double dynamic_liquidity_score = 50.0;
    // 1. 거래량 급증 체크 (Z-Score 로직 통과 여부)
    bool is_spike = isVolumeSpikeSignificant(metrics, candles);
    if (!is_spike && metrics.liquidity_score < 30.0) {
        LOG_INFO("{} - 1단계 탈락 (Spike: {}, LiqScore: {:.1f})", 
             market, is_spike, metrics.liquidity_score);
        // 거래량 폭발도 없고, 유동성도 낮으면 탈락
        return false;
    }
    
    // 2. RSI 체크 (정렬된 데이터 기준 마지막 가격들 추출)
    auto prices = analytics::TechnicalIndicators::extractClosePrices(candles);
    double rsi = analytics::TechnicalIndicators::calculateRSI(prices, 14);
    
    // [수정] 스캘핑/모멘텀을 위해 범위를 현실적으로 상향 (25~75)
    if (rsi < 20.0 || rsi > 75.0) {
        LOG_INFO("{} - RSI 범위 이탈: {:.1f} (목표: 25-75)", market, rsi);
        return false;
    }
    
    // 3. MACD 체크 (Histogram > 0)
    auto macd_current = analytics::TechnicalIndicators::calculateMACD(prices, 12, 26, 9);
    // 이전 MACD 계산: 마지막 가격을 제외한 sub-vector 생성
    std::vector<double> prev_prices(prices.begin(), prices.end() - 1);
    auto macd_prev = analytics::TechnicalIndicators::calculateMACD(prev_prices, 12, 26, 9);

    double current_hist = macd_current.histogram;
    double prev_hist = macd_prev.histogram;

    // 판정: 양수이거나, 음수더라도 전보다 '상승(덜 나빠짐)' 중이면 통과
    bool is_macd_positive = current_hist > 0;
    bool is_macd_rising = current_hist > prev_hist; 

    if (!is_macd_positive && !is_macd_rising) {
        LOG_INFO("{} - MACD 하락세 (Hist: {:.4f}, Prev: {:.4f})", market, current_hist, prev_hist);
        return false;
    }
    
    // 4. 가격 변동 체크
    double abs_change = std::abs(metrics.price_change_rate);
    if (abs_change < 0.05) { // 0.2도 높을 수 있으니 0.1로 완화
        LOG_INFO("{} - 변동성 미달 ({:.2f}%)", market, abs_change);
        return false;
    }
    
    // 5. [수정] 반등 및 지지 확인
    size_t n = candles.size();
    const auto& last_candle = candles.back();

    int bullish_count = 0;
    size_t start_check = (n >= 3) ? n - 3 : 0;
    for (size_t i = start_check; i < n; ++i) {
        if (candles[i].close > candles[i].open) bullish_count++;
    }

    // 디버깅 로그
    LOG_INFO("{} - 시가: {}, 종가: {}, 저가: {}, Bullish: {}/3", 
             market, last_candle.open, last_candle.close, last_candle.low, bullish_count);

    bool is_bullish = last_candle.close > last_candle.open;
    // [보정] 사실상 보합(flat) 범위를 0.05% (0.0005) -> 0.07% (0.0007)로 아주 살짝 상향
    bool is_flat = std::abs(last_candle.close - last_candle.open) <= (last_candle.open * 0.0007); 
    bool has_support = last_candle.low < std::min(last_candle.open, last_candle.close); 

    bool pass_rebound = false;

    // 1) 최근 3개 중 양봉이 1개라도 있는 경우 (기존 로직 유지 + 보합 범위 상향)
    if (bullish_count >= 1) {
        if (is_bullish || is_flat) pass_rebound = true;
        else if (has_support && (last_candle.close >= last_candle.open * 0.999)) pass_rebound = true; 
    } 
    // 2) [추가] 3개 다 음봉이었지만(0/3), 현재 강력한 '망치형 지지'가 나온 경우
    else if (has_support && is_flat) {
        // 하락을 멈추고 저가 매수세가 들어와 '도지'를 만들었다면 반등 신호로 간주
        pass_rebound = true;
    }

    if (!pass_rebound) {
        LOG_INFO("{} - 반등 확인 실패 (Bullish: {}/3, Flat: {}, Support: {})", 
                 market, bullish_count, is_flat, has_support);
        return false;
    }
    
    // 6. 유동성 체크
    if (metrics.liquidity_score < dynamic_liquidity_score) { // 기준을 조금 낮춰 유연하게 적용
        return false;
    }
    
    LOG_INFO("{} - ✅ 스캘핑 진입 조건 만족! (RSI: {:.1f}, 변동: {:.2f}%)", 
              market, rsi, metrics.price_change_rate);
    
    return true;
}

bool ScalpingStrategy::shouldExit(
    const std::string& market,
    double entry_price,
    double current_price,
    double holding_time_seconds
) {
    (void)market;
    
    // 익절
    if (current_price >= entry_price * 1.02) {
        return true;
    }
    
    // 손절
    if (current_price <= entry_price * 0.99) {
        return true;
    }
    
    // 시간 손절 (5분)
    if (holding_time_seconds >= MAX_HOLDING_TIME) {
        return true;
    }
    
    return false;
}

double ScalpingStrategy::calculateStopLoss(
    double entry_price,
    const std::vector<analytics::Candle>& candles
) {
    auto stops = calculateScalpingDynamicStops(entry_price, candles);
    return stops.stop_loss;
}

double ScalpingStrategy::calculateTakeProfit(
    double entry_price,
    const std::vector<analytics::Candle>& candles
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
    std::vector<analytics::Candle> empty_candles;
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
    
    // [핵심 추가] 거래가 종료되었으므로 '진입 중' 목록에서 해당 코인 삭제
    // 이제 이 코인은 다시 매수 신호가 발생하면 진입할 수 있게 됨
    if (active_positions_.erase(market)) {
        LOG_INFO("{} - 스캘핑 포지션 목록 해제 (청산 완료, 재진입 가능)", market);
    }
    
    // --- 아래는 기존 통계 로직 그대로 유지 ---
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

// ===== API 호출 관리 (업비트 Rate Limit 준수) =====

bool ScalpingStrategy::canMakeOrderBookAPICall() const {
    long long now = getCurrentTimestamp();
    
    // 1초 이내 호출 횟수
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
    
    // 10초 이상 된 기록 제거
    while (!api_call_timestamps_.empty() && 
           getCurrentTimestamp() - api_call_timestamps_.front() > 10000) {
        api_call_timestamps_.pop_front();
    }
}

nlohmann::json ScalpingStrategy::getCachedOrderBook(const std::string& market) {
    long long now = getCurrentTimestamp();
    
    // 캐시 유효성 체크
    if (now - last_orderbook_fetch_time_ < ORDERBOOK_CACHE_MS && 
        !cached_orderbook_.empty()) {
        return cached_orderbook_;
    }
    
    // Rate Limit 체크
    if (!canMakeOrderBookAPICall()) {
        LOG_WARN("OrderBook API Rate Limit 도달, 캐시 사용");
        return cached_orderbook_;
    }
    
    // 새로 조회
    try {
        recordAPICall();
        cached_orderbook_ = client_->getOrderBook(market);
        last_orderbook_fetch_time_ = now;
    } catch (const std::exception& e) {
        LOG_ERROR("OrderBook 조회 실패: {}", e.what());
    }
    
    return cached_orderbook_;
}

std::vector<analytics::Candle> ScalpingStrategy::getCachedCandles(const std::string& market, int count) {
    long long now = getCurrentTimestamp();
    if (candle_cache_.find(market) != candle_cache_.end() && now - candle_cache_time_[market] < CANDLE_CACHE_MS) {
        return candle_cache_[market];
    }

    if (!canMakeCandleAPICall()) return candle_cache_.count(market) ? candle_cache_[market] : std::vector<analytics::Candle>();

    try {
        recordAPICall();
        auto candles_json = client_->getCandles(market, "1", count);
        auto candles = analytics::TechnicalIndicators::jsonToCandles(candles_json);
        candle_cache_[market] = candles;
        candle_cache_time_[market] = now;
        return candles;
    } catch (const std::exception& e) {
        LOG_ERROR("Candle 조회 실패: {}", e.what());
        return candle_cache_.count(market) ? candle_cache_[market] : std::vector<analytics::Candle>();
    }
}

// ===== 거래 빈도 관리 =====

bool ScalpingStrategy::canTradeNow() {
    resetDailyCounters();
    resetHourlyCounters();
    
    if (daily_trades_count_ >= MAX_DAILY_SCALPING_TRADES) {
        return false;
    }
    
    if (hourly_trades_count_ >= MAX_HOURLY_SCALPING_TRADES) {
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
        LOG_INFO("일일 스캘핑 거래 카운터 리셋");
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

// ===== 서킷 브레이커 =====

void ScalpingStrategy::checkCircuitBreaker() {
    if (consecutive_losses_ >= MAX_CONSECUTIVE_LOSSES && !circuit_breaker_active_) {
        activateCircuitBreaker();
    }
    
    long long now = getCurrentTimestamp();
    if (circuit_breaker_active_ && now >= circuit_breaker_until_) {
        circuit_breaker_active_ = false;
        consecutive_losses_ = 0;
        LOG_INFO("서킷 브레이커 해제");
    }
}

void ScalpingStrategy::activateCircuitBreaker() {
    circuit_breaker_active_ = true;
    circuit_breaker_until_ = getCurrentTimestamp() + CIRCUIT_BREAKER_COOLDOWN_MS;
    LOG_WARN("서킷 브레이커 발동! {}연패, 1시간 거래 중지", consecutive_losses_);
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

MarketMicrostate ScalpingStrategy::detectMarketMicrostate(
    const std::vector<analytics::Candle>& candles
) {
    if (candles.size() < 14) { // RSI 14를 위해 최소 14개 권장
        return MarketMicrostate::CONSOLIDATION;
    }
    
    updateMicrostateModel(candles, microstate_model_);
    
    int max_idx = 0;
    double max_prob = microstate_model_.current_prob[0];
    
    for (int i = 1; i < 5; ++i) {
        if (microstate_model_.current_prob[i] > max_prob) {
            max_prob = microstate_model_.current_prob[i];
            max_idx = i;
        }
    }

    // [추가 수정] Index 4 (DECLINE) 보정 로직
    if (max_idx == static_cast<int>(MarketMicrostate::DECLINE)) {
        // 함수 내부에서 RSI만 가볍게 계산 (MACD보다 훨씬 가벼움)
        auto prices = analytics::TechnicalIndicators::extractClosePrices(candles);
        double rsi = analytics::TechnicalIndicators::calculateRSI(prices, 14);

        // RSI가 40 미만이면 "하락이 멈추고 반등하려는 횡보"로 간주하여 구출
        if (rsi < 40.0) {
            return MarketMicrostate::CONSOLIDATION; 
        }
    }
    
    // 기존 유동성 장벽 로직
    if (max_idx != static_cast<int>(MarketMicrostate::CONSOLIDATION) && max_prob < 0.4) {
        return MarketMicrostate::CONSOLIDATION; 
    }

    return static_cast<MarketMicrostate>(max_idx);
}

void ScalpingStrategy::updateMicrostateModel(
    const std::vector<analytics::Candle>& candles,
    MicrostateModel& model
) {
    // 1. 최소 데이터 확보 (최근 10개 분석)
    if (candles.size() < 11) return; 
    
    std::vector<double> returns;
    size_t start_idx = candles.size() - 10; // 최근 10개 캔들 구간 설정
    
    // 2. [수정] 정방향 수익률 계산 (과거에서 현재로)
    for (size_t i = start_idx + 1; i < candles.size(); ++i) {
        // (현재가 - 이전가) / 이전가 = 정상적인 수익률
        double ret = (candles[i].close - candles[i-1].close) / candles[i-1].close;
        returns.push_back(ret);
    }
    
    double mean_return = calculateMean(returns);
    double volatility = calculateStdDev(returns, mean_return);
    
    double min_vol = 0.0005; // 0.05%
    double active_vol = std::max(volatility, min_vol);

    std::array<double, 5> observation_prob;
    
    // 3. [수정] 조건문 인덱스 교정
    const auto& current = candles.back();          // 현재 캔들
    const auto& previous = candles[candles.size()-2]; // 직전 캔들
    
    // 1. 과매도 반등: 최근 하락폭이 변동성의 1.5배 이상이고, 현재 양봉인 경우
    if (mean_return < -(active_vol * 1.5) && current.close > previous.close) {
        observation_prob = {0.7, 0.15, 0.05, 0.05, 0.05}; // 반등 확률 상향
    }
    // 2. 모멘텀 급등: 평균 수익률이 변동성의 2배를 초과 (강한 돌파)
    else if (mean_return > (active_vol * 2.0)) {
        observation_prob = {0.1, 0.7, 0.1, 0.05, 0.05};
    }
    // 3. 안정적 돌파: 변동성 범위 내에서 꾸준히 상승 (계단식)
    else if (mean_return > (active_vol * 0.5) && volatility < active_vol * 1.2) {
        observation_prob = {0.1, 0.2, 0.6, 0.05, 0.05};
    }
    // 4. 횡보: 수익률이 변동성 절반도 안 되는 미미한 수준
    else if (std::abs(mean_return) < (active_vol * 0.3)) {
        observation_prob = {0.1, 0.1, 0.1, 0.6, 0.1};
    }
    // 5. 하락 추세
    else {
        observation_prob = {0.05, 0.05, 0.05, 0.15, 0.7};
    }
    
    // 4. 베이즈 업데이트 (Bayesian Update) 로직은 그대로 유지
    double total = 0.0;
    for (int i = 0; i < 5; ++i) {
        model.current_prob[i] *= observation_prob[i];
        total += model.current_prob[i];
    }
    
    if (total > 0) {
        for (int i = 0; i < 5; ++i) {
            model.current_prob[i] /= total;
        }
    }
}

bool ScalpingStrategy::isVolumeSpikeSignificant(
    const analytics::CoinMetrics& metrics,
    const std::vector<analytics::Candle>& candles
) const {
    (void)metrics;

    if (candles.size() < 21) return false;
    
    // 현재(T=0, 맨 뒤) 거래량
    double current_volume = candles.back().volume;
    
    // 히스토리: T-20 ~ T-1 (총 20개)
    std::vector<double> volumes;
    volumes.reserve(20);
    
    // candles.size() - 21 부터 candles.size() - 1 까지 (직전 캔들까지)
    for (size_t i = candles.size() - 21; i < candles.size() - 1; ++i) {
        volumes.push_back(candles[i].volume);
    }
    
    // volumes 벡터의 구성: [T-20, T-19, ..., T-2, T-1] (총 20개)
    
    // Z-Score (현재 거래량이 과거 20개 평균 대비 얼마나 튀었나)
    double z_score = calculateZScore(current_volume, volumes);
    
    if (z_score < 1.15) { 
        return false;
    }
    
    // T-Test: 최근 3분(T-2, T-1, T-0) vs 과거(T-20 ~ T-3)
    // T-0(현재)를 포함해야 '지금'의 추세를 알 수 있음
    
    std::vector<double> recent_3;
    recent_3.push_back(candles[candles.size()-3].volume);
    recent_3.push_back(candles[candles.size()-2].volume);
    recent_3.push_back(candles[candles.size()-1].volume); // 현재
    
    std::vector<double> past_17; // T-20 ~ T-4 (총 17개)
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
    (void)alpha;  // ✅ 추가

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
    const std::vector<analytics::Candle>& candles_1m
) const {
    ScalpingMultiTimeframeSignal signal;
    if (candles_1m.size() < 30) return signal;

    // [기존 과매도 체크 유지하되 용도 변경]
    signal.tf_1m_oversold = isOversoldOnTimeframe(candles_1m, signal.tf_1m);
    auto candles_3m = resampleTo3m(candles_1m);

    double score = 0.0;
    
    // 1. 1분봉 방향성 체크 (종가가 시가보다 높거나 RSI가 중립 이상)
    if (candles_1m.back().close >= candles_1m.back().open) score += 0.4;
    
    // 2. 3분봉 정렬 체크
    if (!candles_3m.empty()) {
        const auto& last_3m = candles_3m.back();
        // 3분봉이 양봉이거나, 최소한 하락을 멈췄다면 점수 부여
        if (last_3m.close >= last_3m.open) score += 0.4;
        
        // 3. (보너스) 상위 분봉에서 과매도였다가 반등 중이면 큰 가산점
        signal.tf_3m_oversold = isOversoldOnTimeframe(candles_3m, signal.tf_3m);
        if (signal.tf_3m_oversold) score += 0.2;
    }

    signal.alignment_score = score;
    return signal;
}

std::vector<analytics::Candle> ScalpingStrategy::resampleTo3m(
    const std::vector<analytics::Candle>& candles_1m
) const {
    if (candles_1m.size() < 3) return {};
    
    std::vector<analytics::Candle> candles_3m;
    
    size_t n = candles_1m.size();
    // 3의 배수로 맞추기 위해 앞부분을 버림 (최신 데이터를 살리기 위함)
    size_t start_idx = n % 3; 
    
    for (size_t i = start_idx; i + 3 <= n; i += 3) {
        analytics::Candle candle_3m;
        
        // [수정] 1분봉은 시간순(Asc)이므로
        // i: 시작(Open), i+2: 끝(Close)
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
        
        // 타임스탬프는 시작 시간 or 끝 시간? 보통 캔들 시작 시간을 씁니다.
        candle_3m.timestamp = candles_1m[i].timestamp;
        candles_3m.push_back(candle_3m);
    }
    
    return candles_3m;
}

bool ScalpingStrategy::isOversoldOnTimeframe(
    const std::vector<analytics::Candle>& candles,
    ScalpingMultiTimeframeSignal::ScalpingTimeframeMetrics& metrics
) const {
    if (candles.size() < 14) return false;
    
    auto prices = analytics::TechnicalIndicators::extractClosePrices(candles);
    
    // 1. RSI 계산 (가장 최신 prices.back()이 기준이 됨)
    metrics.rsi = analytics::TechnicalIndicators::calculateRSI(prices, 14);
    
    // 2. Stochastic RSI (우리가 고친 최신 데이터 기준 함수 호출)
    auto stoch_result = analytics::TechnicalIndicators::calculateStochastic(candles, 14, 3);
    metrics.stoch_rsi = stoch_result.k;
    
    // 3. [핵심 수정] Instant Momentum (진짜 최근 3개 캔들)
    if (candles.size() >= 4) { // 3개 전을 보려면 최소 4개가 안전함
        // 정렬된 데이터에서 가장 마지막이 '지금(now)', 뒤에서 4번째가 '3개 전'
        double price_now = candles.back().close;
        double price_3_ago = candles[candles.size() - 4].close;
        
        metrics.instant_momentum = (price_now - price_3_ago) / price_3_ago;
    }
    
    // 4. 과매도 조건 (스캘핑 특화)
    // RSI가 30~40 사이이면서, 스토캐스틱이 바닥이거나 '지금 당장' 가격이 고개를 들 때(momentum > 0)
    bool rsi_oversold = metrics.rsi >= 30.0 && metrics.rsi <= 45.0; // 40은 너무 빡빡해서 45로 살짝 완화
    bool stoch_oversold = metrics.stoch_rsi < 25.0; // 스토캐스틱은 더 확실한 바닥(25) 확인
    bool momentum_positive = metrics.instant_momentum > 0.0005; // 미세한 반등(0.05%) 확인
    
    return rsi_oversold && (stoch_oversold || momentum_positive);
}

// ===== 4. Ultra-Fast Order Flow =====

UltraFastOrderFlowMetrics ScalpingStrategy::analyzeUltraFastOrderFlow(
    const analytics::CoinMetrics& metrics,
    double current_price
) {
    (void)current_price;  // ✅ 추가

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
        
        // Instant Pressure (상위 3레벨)
        double top3_bid = 0, top3_ask = 0;
        for (size_t i = 0; i < std::min(size_t(3), units.size()); ++i) {
            top3_bid += units[i]["bid_size"].get<double>();
            top3_ask += units[i]["ask_size"].get<double>();
        }
        
        if (top3_bid + top3_ask > 0) {
            flow.instant_pressure = (top3_bid - top3_ask) / (top3_bid + top3_ask);
        }
        
        // Order Flow Delta (전체)
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
        
        // Micro Imbalance (호가 1-2레벨 집중도)
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
        
        // Microstructure Score 계산
        double score = 0.0;
        
        // Spread (25%)
        if (flow.bid_ask_spread < 0.03) score += 0.25;
        else if (flow.bid_ask_spread < 0.05) score += 0.18;
        else score += 0.10;
        
        // Instant Pressure (30%)
        if (flow.instant_pressure > 0.2) score += 0.30;       // 0.4 -> 0.2
        else if (flow.instant_pressure > 0.0) score += 0.20;  // 0.2 -> 0.0
        else if (flow.instant_pressure > -0.2) score += 0.10; // 하락 압력이 약해도 점수 부여
        
        // Order Flow Delta (20%) - 수정안
        if (flow.order_flow_delta > 0.1) score += 0.20;      // 0.2 -> 0.1
        else if (flow.order_flow_delta > -0.1) score += 0.12; // -0.1까지는 '균형'으로 인정
        
        // Tape Reading (15%)
        score += flow.tape_reading_score * 0.15;
        
        // Micro Imbalance (10%)
        if (flow.micro_imbalance > 0.05) score += 0.10; // 0.1 -> 0.05
        else if (flow.micro_imbalance > -0.05) score += 0.05;
        
        flow.microstructure_score = score;
        
    } catch (const std::exception& e) {
        LOG_ERROR("Order Flow 분석 실패: {}", e.what());
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
    
    // 상위 5레벨의 호가 균형도
    double score = 0.0;
    
    // 1. 매수 호가가 점진적으로 증가하는지 (계단식 지지)
    bool bid_support = true;
    for (size_t i = 1; i < std::min(size_t(5), units.size()); ++i) {
        double current_bid = units[i]["bid_size"].get<double>();
        double prev_bid = units[i-1]["bid_size"].get<double>();
        
        if (current_bid < prev_bid * 0.5) {  // 급격한 감소
            bid_support = false;
            break;
        }
    }
    
    if (bid_support) score += 0.5;
    
    // 2. 매도 호가가 빈약한지 (저항 약함)
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
    const std::vector<analytics::Candle>& candles
) const {
    // 1. 최소 데이터 확보 (최근 6개 캔들 필요)
    if (candles.size() < 6) return 0.0;
    
    // 2. [핵심 수정] 인덱스 기준을 '뒤에서부터'로 변경
    size_t n = candles.size();
    
    // 최근 모멘텀 (T-0 vs T-2) : 가장 최근 3개 캔들의 변화율
    // (현재가 - 2봉전가) / 2봉전가
    double recent_now = candles[n - 1].close;
    double recent_past = candles[n - 3].close;
    double recent_momentum = (recent_now - recent_past) / recent_past;
    
    // 이전 모멘텀 (T-3 vs T-5) : 그 직전 3개 캔들의 변화율
    // (3봉전가 - 5봉전가) / 5봉전가
    double prev_start = candles[n - 4].close;
    double prev_end = candles[n - 6].close;
    double prev_momentum = (prev_start - prev_end) / prev_end;
    
    // 3. 가속도 반환 (최근 힘 - 이전 힘)
    // 결과가 양수라면 상승 힘이 점점 더 세지고 있다는 뜻입니다.
    return recent_momentum - prev_momentum;
}
// ===== 5. Position Sizing (Kelly Criterion + Ultra-Short Vol) =====

ScalpingPositionMetrics ScalpingStrategy::calculateScalpingPositionSize(
    double capital,
    double entry_price,
    double stop_loss,
    const analytics::CoinMetrics& metrics,
    const std::vector<analytics::Candle>& candles
) const {
    ScalpingPositionMetrics pos_metrics;
    
    // 1. Kelly Fraction
    double win_rate = stats_.win_rate > 0 ? stats_.win_rate : 0.65;
    double avg_win = stats_.avg_profit > 0 ? stats_.avg_profit : 0.02;
    double avg_loss = stats_.avg_loss > 0 ? stats_.avg_loss : 0.01;
    
    pos_metrics.kelly_fraction = calculateKellyFraction(win_rate, avg_win, avg_loss);
    pos_metrics.half_kelly = pos_metrics.kelly_fraction * HALF_KELLY_FRACTION;
    
    // 2. 변동성 조정 (초단타용)
    double volatility = 0.015;  // 기본 1.5%
    if (!candles.empty()) {
        volatility = calculateUltraShortVolatility(candles);
    }
    
    pos_metrics.volatility_adjusted = adjustForUltraShortVolatility(
        pos_metrics.half_kelly, volatility
    );
    
    // 3. 유동성 조정
    double liquidity_factor = std::min(1.0, metrics.liquidity_score / 80.0);
    pos_metrics.final_position_size = pos_metrics.volatility_adjusted * liquidity_factor;
    
    // 4. 포지션 제한 및 최소 금액 보정 (최종 수정본)

    // (1) 일단 설정된 최대 비중 제한을 먼저 겁니다.
    pos_metrics.final_position_size = std::min(pos_metrics.final_position_size, MAX_POSITION_SIZE);
    
    // (2) [핵심] 업비트 최소 주문을 위한 비율 계산 (여유있게 6,000원)
    double min_required_size = 6000.0 / capital;
    
    // (3) [중요] 계산된 비중이 최소 주문 금액보다 작다면 '강제로' 상향
    // MAX_POSITION_SIZE(5%)보다 min_required_size(13%)가 크더라도, 
    // 주문이 나가는 게 우선이므로 여기서는 MAX 제한을 무시하고 올립니다.
    if (pos_metrics.final_position_size < min_required_size) {
        pos_metrics.final_position_size = min_required_size;
    }
    
    // (4) 내 전체 자산보다 많이 살 수는 없으므로 최종 방어선
    if (pos_metrics.final_position_size > 1.0) {
        pos_metrics.final_position_size = 1.0;
    }
    
    // (5) 만약 내 전재산이 6,000원도 안 된다면 주문 포기 (0.0 리턴)
    if (capital < 6000.0) {
        pos_metrics.final_position_size = 0.0;
    }
    
    // 5. 예상 Sharpe
    double risk = std::abs(entry_price - stop_loss) / entry_price;
    if (risk > 0.0001) {
        double reward = BASE_TAKE_PROFIT;
        pos_metrics.expected_sharpe = (reward - risk) / (volatility * std::sqrt(105120));
    }
    
    // 6. 최대 손실 금액
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
    kelly = std::min(0.15, kelly);  // 스캘핑: 최대 15%
    
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
    const std::vector<analytics::Candle>& candles
) const {
    ScalpingDynamicStops stops;
    
    if (candles.size() < 10) {
        stops.stop_loss = entry_price * (1.0 - BASE_STOP_LOSS);
        stops.take_profit_1 = entry_price * (1.0 + BASE_TAKE_PROFIT * 0.5);
        stops.take_profit_2 = entry_price * (1.0 + BASE_TAKE_PROFIT);
        stops.breakeven_trigger = entry_price * (1.0 + BREAKEVEN_TRIGGER);
        stops.trailing_start = entry_price * (1.0 + BASE_TAKE_PROFIT * 0.3);
        return stops;
    }
    
    // 1. Micro ATR 기반 손절
    double micro_atr_stop = calculateMicroATRBasedStop(entry_price, candles);
    
    // 2. Hard Stop
    double hard_stop = entry_price * (1.0 - BASE_STOP_LOSS);
    
    // 높은 손절선 선택
    stops.stop_loss = std::max(hard_stop, micro_atr_stop);
    
    if (stops.stop_loss >= entry_price) {
        stops.stop_loss = hard_stop;
    }
    
    // 3. Take Profit (고정 비율)
    double risk = entry_price - stops.stop_loss;
    double reward_ratio = 2.0;  // 스캘핑: 1:2
    
    stops.take_profit_1 = entry_price + (risk * reward_ratio * 0.5);  // 1% (50% 청산)
    stops.take_profit_2 = entry_price + (risk * reward_ratio);         // 2% (전체 청산)
    
    // 4. Breakeven Trigger (1% 수익시)
    stops.breakeven_trigger = entry_price * (1.0 + BREAKEVEN_TRIGGER);
    
    // 5. Trailing Start
    stops.trailing_start = entry_price * (1.0 + BASE_TAKE_PROFIT * 0.5);
    
    return stops;
}

double ScalpingStrategy::calculateMicroATRBasedStop(
    double entry_price,
    const std::vector<analytics::Candle>& candles
) const {
    // 초단타용 짧은 ATR (5기간)
    double atr = analytics::TechnicalIndicators::calculateATR(candles, 5);
    
    if (atr < 0.0001) {
        return entry_price * (1.0 - BASE_STOP_LOSS);
    }
    
    double atr_percent = (atr / entry_price) * 100;
    
    double multiplier = 1.2;  // 스캘핑: 더 타이트
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
    // 1. 최소 비용 계산 (수수료+슬리피지) - 이건 절대 타협 못하는 선입니다.
    double total_cost = (UPBIT_FEE_RATE * 2) + (EXPECTED_SLIPPAGE * 2); 
    double net_return = expected_return - total_cost;

    // 2. 순수익 기준: 0.05% (5bp)
    // 수수료 떼고 커피값이라도 남으면 일단 '기회'라고 봅니다.
    if (net_return < 0.0005) return false;

    // 3. 샤프지수 기준: 0.3 (완화)
    // 초단타(Scalping)는 캔들 몇 개만 보고 들어가기 때문에 샤프지수가 높게 나오기 어렵습니다.
    // 기존 0.5~1.0은 너무 가혹하니 0.3으로 낮춰보세요.
    if (expected_sharpe < 0.3) return false; 

    // 4. 손익비(R/R) 기준: 0.8 (완화)
    // 스캘핑은 승률로 먹고사는 거지, 손익비로 먹고사는 게 아닙니다.
    // '먹을 거 0.8 : 잃을 거 1' 정도만 되어도 승률이 65%면 무조건 이득입니다.
    double actual_rr = expected_return / BASE_STOP_LOSS;
    if (actual_rr < 0.8) return false; 

    return true;
}

// ===== 8. Signal Strength =====

double ScalpingStrategy::calculateScalpingSignalStrength(
    const analytics::CoinMetrics& metrics, const std::vector<analytics::Candle>& candles,
    const ScalpingMultiTimeframeSignal& mtf_signal, const UltraFastOrderFlowMetrics& order_flow,
    MarketMicrostate microstate) const 
{
    double strength = 0.0;
    // 1. 시장 상태 점수
    if (microstate == MarketMicrostate::OVERSOLD_BOUNCE) strength += 0.20;
    else if (microstate == MarketMicrostate::MOMENTUM_SPIKE) strength += 0.20;
    else if (microstate == MarketMicrostate::BREAKOUT) strength += 0.15; 
    else if (microstate == MarketMicrostate::CONSOLIDATION) strength += 0.10;

    strength += mtf_signal.alignment_score * 0.15;

    // 2. 오더플로우 보너스 문턱 완화
    double of_weight = 0.30;
    double effective_of_score = order_flow.microstructure_score;
    // [수정] 0.5 -> 0.3으로 완화하여 평시에도 가중치 부여
    if (effective_of_score > 0.3) { 
        effective_of_score = std::min(1.0, effective_of_score + 0.10);
    }
    strength += effective_of_score * of_weight;

    // 3. RSI 구간 하한선 확장 (핵심 수정)
    auto prices = analytics::TechnicalIndicators::extractClosePrices(candles);
    double rsi = analytics::TechnicalIndicators::calculateRSI(prices, 14);
    
    // [수정] 25(찐바닥)부터 점수를 주도록 범위 확장
    if (rsi >= 25 && rsi <= 45) strength += 0.15; 
    else if (rsi > 45 && rsi < 55) strength += 0.05;
    else if (rsi >= 55 && rsi <= 75) strength += 0.10;

    // 4. 거래량 점수
    if (metrics.volume_surge_ratio >= 150) strength += 0.15;
    else if (metrics.volume_surge_ratio >= 110) strength += 0.10; 
    else strength += 0.05;

    // 5. 유동성 보정
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
    
    int var_index = static_cast<int>(sorted_returns.size() * 0.05);  // 95% 신뢰구간
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
    
    // 평균 보유 시간 (초)
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

double ScalpingStrategy::calculateUltraShortVolatility(const std::vector<analytics::Candle>& candles) const {
    if (candles.size() < 10) return 0.0;
    
    std::vector<double> returns;
    size_t start = candles.size() - 10;
    for (size_t i = start + 1; i < candles.size(); ++i) {
        // [수정] (현재 - 이전) / 이전
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
    if (expected_return < 0.008) {  // 0.8% 미만
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

// ===== 포지션 상태 업데이트 (모니터링 중) =====

void ScalpingStrategy::updateState(const std::string& market, double current_price) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    // ScalpingStrategy는 초단타이므로 실시간 추적이 덜 중요
    // 하지만 필요시 마이크로스테이트 업데이트 가능
   
    // [선택사항] 가장 최근 가격으로 마이크로상태 재평가
    // 하지만 스캘핑이므로 홀딩 시간이 짧아 대부분 청산 전에 신호 재분석 안 함
}

} // namespace strategy
} // namespace autolife
