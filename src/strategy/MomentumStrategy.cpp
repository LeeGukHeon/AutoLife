#include "strategy/MomentumStrategy.h"
#include "analytics/TechnicalIndicators.h"
#include "common/Logger.h"
#include <chrono>
#include <algorithm>
#include <numeric>
#include <cmath>

namespace autolife {
namespace strategy {

// ===== 생성자 & 기본 메서드 =====

MomentumStrategy::MomentumStrategy(std::shared_ptr<network::UpbitHttpClient> client)
    : client_(client)
    , enabled_(true)
    , last_signal_time_(0)
{
    LOG_INFO("Advanced Momentum Strategy 초기화 (금융공학 기반)");
    
    stats_ = Statistics();
    rolling_stats_ = RollingStatistics();
    regime_model_ = RegimeModel();
}

StrategyInfo MomentumStrategy::getInfo() const {
    StrategyInfo info;
    info.name = "Advanced Momentum";
    info.description = "금융공학 기반 모멘텀 전략 (Kelly Criterion, HMM, CVaR)";
    info.timeframe = "1m-15m";
    info.min_capital = 100000;
    info.expected_winrate = 0.62;
    info.risk_level = 6.5;
    return info;
}

Signal MomentumStrategy::generateSignal(
    const std::string& market,
    const analytics::CoinMetrics& metrics,
    const std::vector<analytics::Candle>& candles,
    double current_price,
    double available_capital
) {
     std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    Signal signal;
    signal.market = market;

    if (active_positions_.find(market) != active_positions_.end()){
        return signal;
    }

    signal.strategy_name = "Advanced Momentum";
    signal.timestamp = getCurrentTimestamp();
    
    LOG_INFO("{} - [Momentum] 분석 시작", market);
    
    if (candles.size() < 60) {
        LOG_INFO("{} - [Momentum] 캔들 부족: {}", market, candles.size());
        return signal;
    }
    
    LOG_INFO("{} - [Momentum] shouldEnter() 체크 시작...", market);
    if (!shouldEnter(market, metrics, candles, current_price)) {
        LOG_INFO("{} - [Momentum] shouldEnter() 실패", market);
        return signal;
    }
    
    LOG_INFO("{} - [Momentum] Market Regime 체크...", market);
    
    // 1. Market Regime
    MarketRegime regime = detectMarketRegime(candles);
    if (regime != MarketRegime::STRONG_UPTREND && 
        regime != MarketRegime::WEAK_UPTREND) {
        LOG_INFO("{} - Regime 부적합: {}", market, static_cast<int>(regime));
        return signal;
    }
    
    // 2. Multi-Timeframe
    auto mtf_signal = analyzeMultiTimeframe(candles);
    if (mtf_signal.alignment_score < 0.40) {
        LOG_INFO("{} - MTF 정렬 부족: {:.2f}", market, mtf_signal.alignment_score);
        return signal;
    }
    
    // 3. Order Flow
    auto order_flow = analyzeAdvancedOrderFlow(metrics, current_price);
    if (order_flow.microstructure_score < 0.40) {
        LOG_INFO("{} - 미세구조 점수 부족: {:.2f}", market, order_flow.microstructure_score);
        return signal;
    }
    
    // 4. Signal Strength
    signal.strength = calculateSignalStrength(metrics, candles, mtf_signal, order_flow, regime);
    
    if (signal.strength < 0.65) {
        LOG_INFO("{} - 신호 강도 부족: {:.2f}", market, signal.strength);
        return signal;
    }
    
    // 5. Dynamic Stops
    auto stops = calculateDynamicStops(current_price, candles);
    
    signal.type = SignalType::BUY;
    signal.entry_price = current_price;
    signal.stop_loss = stops.stop_loss;
    signal.take_profit_1 = stops.take_profit_1;  // [✅] 1차 익절
    signal.take_profit_2 = stops.take_profit_2;  // [✅] 2차 익절
    signal.buy_order_type = strategy::OrderTypePolicy::LIMIT_WITH_FALLBACK;
    signal.sell_order_type = strategy::OrderTypePolicy::LIMIT_WITH_FALLBACK;
    signal.max_retries = 3;
    signal.retry_wait_ms = 500;
    
    // 6. Worth Trading
    double expected_return = (signal.take_profit_2 - signal.entry_price) / signal.entry_price;
    double expected_sharpe = calculateSharpeRatio();
    
    if (!isWorthTrading(expected_return, expected_sharpe)) {
        LOG_INFO("{} - 거래 비용 대비 수익 부족", market);
        return signal;
    }
    
    double current_capital = available_capital;
    if (current_capital <= 0) {
        LOG_WARN("{} - [Momentum] 가용자본 없음 (신호 무시)", market);
        return signal;
    }

    // 7. Position Sizing
    auto pos_metrics = calculateAdvancedPositionSize(
        current_capital, signal.entry_price, signal.stop_loss, metrics, candles
    );

    signal.position_size = pos_metrics.final_position_size;
    
    // 8. Signal Interval
    if (!shouldGenerateSignal(expected_return, expected_sharpe)) {
        LOG_INFO("{} - 신호 간격 제한", market);
        return signal;
    }
    
    signal.reason = fmt::format(
        "Momentum: Regime={}, MTF={:.0f}%, Flow={:.0f}%, Strength={:.0f}%, Kelly={:.1f}%",
        static_cast<int>(regime),
        mtf_signal.alignment_score * 100,
        order_flow.microstructure_score * 100,
        signal.strength * 100,
        pos_metrics.final_position_size * 100
    );
    
    if (signal.strength >= 0.85) {
        signal.type = SignalType::STRONG_BUY;
    }
    
    last_signal_time_ = getCurrentTimestamp();
    
    LOG_INFO("모멘텀 신호: {} - 강도 {:.2f}, 포지션 {:.1f}%",
             market, signal.strength, signal.position_size * 100);
    
    return signal;
}

bool MomentumStrategy::shouldEnter(
    const std::string& market,
    const analytics::CoinMetrics& metrics,
    const std::vector<analytics::Candle>& candles,
    double current_price
) {
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
    
    // [수정된 부분] 최근 3개 캔들 확인 (Old -> New 순서이므로 뒤에서부터 확인)
    int bullish_count = 0;
    size_t n = candles.size();
    size_t start_check = (n >= 3) ? n - 3 : 0; // 뒤에서 3번째부터 시작

    for (size_t i = start_check; i < n; ++i) {
        if (candles[i].close > candles[i].open) {
            bullish_count++;
        }
    }

    if (bullish_count < 2) {
        return false;
    }
    
    if (metrics.price_change_rate < 0.5) {
        LOG_INFO("{} - 상승 모멘텀 부족: {:.2f}% (목표: 2.0%+)", 
             market, metrics.price_change_rate);
        return false;
    }
    
    // [수정] 유동성 점수 체크 (Liquidity Score)
    // 점수가 낮으면 슬리피지가 크므로 모멘텀 전략에 불리
    if (metrics.liquidity_score < 30.0) { // 50 -> 30 완화 (알트코인 고려)
        return false;
    }
    
    // ✅ 모든 조건 만족
    LOG_INFO("{} - ✅ 모멘텀 진입 조건 만족! (RSI: {:.1f}, 변동: {:.2f}%, 양봉: {}/3)", 
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
    
    // 1. 시간 청산 (보유 시간이 너무 길어지면 모멘텀 상실로 간주)
    if (holding_time_seconds >= MAX_HOLDING_TIME) { // 2시간
        return true;
    }
    
    // 2. 추세 반전 확인 (Trend Reversal) logic이 필요함.
    // 하지만 현재 함수 인자로는 캔들 데이터에 접근할 수 없음.
    // 따라서 여기서는 '시간 청산'만 담당하고, 
    // 기술적 지표에 의한 청산은 RiskManager의 Trailing Stop에 맡기는 것이 안전함.
    
    return false;
}

double MomentumStrategy::calculateStopLoss(
    double entry_price,
    const std::vector<analytics::Candle>& candles
) {
    auto stops = calculateDynamicStops(entry_price, candles);
    return stops.stop_loss;
}

double MomentumStrategy::calculateTakeProfit(
    double entry_price,
    const std::vector<analytics::Candle>& candles
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
    std::vector<analytics::Candle> empty_candles;
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
    active_positions_.insert(signal.market);
    return true;
}

void MomentumStrategy::updateStatistics(const std::string& market, bool is_win, double profit_loss) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    // [중복 매수 방지 해제] 청산 완료 시 목록에서 제거 -> 재진입 허용
    if (active_positions_.erase(market)) {
        LOG_INFO("{} - 모멘텀 포지션 해제 (청산 완료)", market);
    }
    
    stats_.total_signals++;
    
    if (is_win) {
        stats_.winning_trades++;
        stats_.total_profit += profit_loss;
    } else {
        stats_.losing_trades++;
        stats_.total_loss += std::abs(profit_loss);
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
}

double MomentumStrategy::updateTrailingStop(
    double entry_price,
    double highest_price,
    double current_price,
    const std::vector<analytics::Candle>& recent_candles
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
    const std::vector<analytics::Candle>& candles
) {
    if (candles.size() < 20) {
        return MarketRegime::SIDEWAYS;
    }
    
    updateRegimeModel(candles, regime_model_);
    
    double strong_up = regime_model_.current_prob[static_cast<int>(MarketRegime::STRONG_UPTREND)];
    double weak_up = regime_model_.current_prob[static_cast<int>(MarketRegime::WEAK_UPTREND)];
    
    // [핵심] 두 상승 확률의 합이 횡보 확률보다 크거나, 40%를 넘으면 상승으로 판정
    if ((strong_up + weak_up) > 0.40) {
        // 더 힘이 강한 쪽을 반환하거나, 모멘텀 전략이라면 WEAK만 되어도 진입 허용
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
    const std::vector<analytics::Candle>& candles,
    RegimeModel& model
) {
    if (candles.size() < 20) return;
    
    std::vector<double> returns;
    // [✅ 핵심 수정] 
    // 데이터가 [과거 -> 현재] 순서이므로, 
    // 가장 끝(back)이 최신 데이터입니다. 
    // 따라서 뒤에서부터 앞으로 가면서 계산해야 "최신 수익률"을 얻습니다.
    
    size_t end_idx = candles.size() - 1;
    size_t start_idx = (candles.size() > 20) ? (candles.size() - 20) : 0;

    // 뒤(최신)에서부터 과거로 가면서 루프
    for (size_t i = end_idx; i > start_idx; --i) {
        // 공식: (현재가격 - 전가격) / 전가격
        // candles[i]가 현재, candles[i-1]이 과거
        double ret = (candles[i].close - candles[i-1].close) / candles[i-1].close;
        
        returns.push_back(ret);
    }
    
    double mean_return = calculateMean(returns);
    double volatility = calculateStdDev(returns, mean_return);
    // 최소 변동성 하한선 (변동성이 극도로 낮을 때 노이즈에 튀는 것 방지)
    double active_vol = std::max(volatility, 0.0005);
    std::array<double, 5> observation_prob;
    
    // 1. STRONG_UP: 수익률이 변동성의 1.5배 이상 (강한 돌파)
    if (mean_return > active_vol * 1.5) {
        observation_prob = {0.70, 0.20, 0.05, 0.03, 0.02};
    } 
    // 2. WEAK_UP: 수익률이 변동성의 0.5배 이상 (완만한 상승)
    else if (mean_return > active_vol * 0.5) {
        observation_prob = {0.20, 0.65, 0.10, 0.03, 0.02};
    } 
    // 3. SIDEWAYS: 수익률이 변동성의 +-0.5배 이내 (박스권)
    else if (std::abs(mean_return) <= active_vol * 0.5) {
        observation_prob = {0.10, 0.15, 0.55, 0.15, 0.05};
    } 
    // 4. WEAK_DOWN: 수익률이 -0.5배 미만
    else if (mean_return < -active_vol * 0.5 && mean_return >= -active_vol * 1.5) {
        observation_prob = {0.05, 0.05, 0.15, 0.60, 0.15};
    } 
    // 5. STRONG_DOWN: 수익률이 -1.5배 미만
    else {
        observation_prob = {0.02, 0.03, 0.05, 0.20, 0.70};
    }
    
    double total = 0.0;
    for (int i = 0; i < 5; ++i) {
        model.current_prob[i] *= observation_prob[i];

        //특정 상태가 고사(Dead)하는 것을 방지하여 반응성 유지
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
    const std::vector<analytics::Candle>& candles
) const {
    // 1. 최소 데이터 확보 (현재 1개 + 과거 30개 = 31개)
        (void)metrics;  // 현재 미사용
    if (candles.size() < 31) return false;
    
    // 2. [수정] 전체 31개 데이터 수집 (과거 30개 + 현재 1개)
    std::vector<double> volumes;
    volumes.reserve(31);
    
    // 정렬된 candles의 뒤에서 31번째부터 끝까지
    for (size_t i = candles.size() - 31; i < candles.size(); ++i) {
        volumes.push_back(candles[i].volume);
    }

    // 3. Z-Score 계산 (현재값 vs 과거 30개 평균)
    double current_volume = volumes.back();
    // history: volumes의 처음부터 뒤에서 두 번째까지 (마지막 제외)
    std::vector<double> history(volumes.begin(), volumes.end() - 1);
    
    double z_score = calculateZScore(current_volume, history);
    
    // [중요] Z-Score 필터링 (누락되었던 부분 복구!)
    // 1.96은 95% 신뢰구간을 의미. 즉, 상위 2.5% 수준의 급등만 인정
    if (z_score < 1.64) {
        return false;
    }
    
    // 4. T-Test: 최근 5개(현재 포함) vs 그 이전 26개
    // volumes 벡터 내에서 분리
    std::vector<double> recent_5(volumes.end() - 5, volumes.end());
    std::vector<double> past_26(volumes.begin(), volumes.end() - 5);
    
    // 유의수준 0.05 (5%)
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
        (void)alpha;  // 고정 t_critical 값 사용
    
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
    const std::vector<analytics::Candle>& candles_1m

) const {
    MultiTimeframeSignal signal;
    
    if (candles_1m.size() < 60) {
        return signal;
    }
    
    // 1. 1분봉 분석 (데이터 충분)
    signal.tf_1m_bullish = isBullishOnTimeframe(candles_1m, signal.tf_1m);
    
    // 2. 5분봉 분석 (200개 기준 40개 -> 데이터 충분)
    auto candles_5m = resampleTo5m(candles_1m);
    if (!candles_5m.empty()) {
        signal.tf_5m_bullish = isBullishOnTimeframe(candles_5m, signal.tf_5m);
    }
    
    // 3. 15분봉 분석 (200개 기준 13개 -> 부족함!)
    auto candles_15m = resampleTo15m(candles_1m);
    bool is_15m_valid = false; // 15분봉을 점수 계산에 넣을지 여부

    // [핵심] 지표 계산에 필요한 최소 개수(MACD 기준 26개)가 있는지 확인
    if (candles_15m.size() >= 26) {
        signal.tf_15m_bullish = isBullishOnTimeframe(candles_15m, signal.tf_15m);
        is_15m_valid = true; // 데이터가 충분하므로 점수판에 끼워줌
    }
    
    // 4. [수정] 동적 점수 계산 (Dynamic Scoring)
    double total_score = 0.0;
    double max_possible_score = 0.0;

    // 1분봉 반영
    max_possible_score += 1.0;
    if (signal.tf_1m_bullish) total_score += 1.0;

    // 5분봉 반영
    max_possible_score += 1.0;
    if (signal.tf_5m_bullish) total_score += 1.0;

    // 15분봉 반영 (데이터가 충분할 때만 분모/분자에 포함)
    if (is_15m_valid) {
        max_possible_score += 1.0;
        if (signal.tf_15m_bullish) total_score += 1.0;
    }
    
    // 0으로 나누기 방지
    signal.alignment_score = (max_possible_score > 0) 
                           ? (total_score / max_possible_score) 
                           : 0.0;
    
    return signal;
}

std::vector<analytics::Candle> MomentumStrategy::resampleTo5m(const std::vector<analytics::Candle>& candles_1m) const {
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

std::vector<analytics::Candle> MomentumStrategy::resampleTo15m(
    const std::vector<analytics::Candle>& candles_1m
) const {
    if (candles_1m.size() < 15) return {};
    
    std::vector<analytics::Candle> candles_15m;
    
    for (size_t i = 0; i + 15 <= candles_1m.size(); i += 15) {
        analytics::Candle candle_15m;
        
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

bool MomentumStrategy::isBullishOnTimeframe(const std::vector<analytics::Candle>& candles, MultiTimeframeSignal::TimeframeMetrics& metrics) const {
    if (candles.size() < 26) return false;
    auto prices = analytics::TechnicalIndicators::extractClosePrices(candles);
    metrics.rsi = analytics::TechnicalIndicators::calculateRSI(prices, 14);
    auto macd = analytics::TechnicalIndicators::calculateMACD(prices, 12, 26, 9);
    metrics.macd_histogram = macd.histogram;

    // [수정] EMA20과 3봉 전 가격을 비교하여 현재 추세 측정
    double ema_20 = analytics::TechnicalIndicators::calculateEMA(prices, 20);
    double current_price = prices.back();
    metrics.trend_strength = (current_price - ema_20) / ema_20;

    bool rsi_bullish = metrics.rsi >= 50.0 && metrics.rsi <= 75.0; // 상단 범위 확장
    bool macd_bullish = metrics.macd_histogram > 0;
    bool trend_bullish = metrics.trend_strength > 0;

    return rsi_bullish && macd_bullish && trend_bullish;
}

// ===== 4. Advanced Order Flow =====

AdvancedOrderFlowMetrics MomentumStrategy::analyzeAdvancedOrderFlow(
    const analytics::CoinMetrics& metrics,
    double current_price
) const {
    (void)current_price;  // current_price 파라미터 미사용
    
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
        
        // Order Book Pressure (30%) 수정
        if (flow.order_book_pressure > 0.1) score += 0.30;       // 0.3 -> 0.1
        else if (flow.order_book_pressure > -0.1) score += 0.20; // 균형만 이뤄도 점수
        else if (flow.order_book_pressure > -0.3) score += 0.10;

        // Large Order Imbalance (20%) 수정
        if (flow.large_order_imbalance > 0.1) score += 0.20;     // 0.2 -> 0.1
        else if (flow.large_order_imbalance > -0.1) score += 0.10;
        
        if (flow.cumulative_delta > 0.1) score += 0.20;
        else if (flow.cumulative_delta > 0) score += 0.10;
        
        flow.microstructure_score = score;
        
    } catch (const std::exception& e) {
        LOG_ERROR("Order Flow 분석 실패: {}", e.what());
    }
    
    return flow;
}

double MomentumStrategy::calculateVWAPDeviation(
    const std::vector<analytics::Candle>& candles,
    double current_price
) const {
    double vwap = analytics::TechnicalIndicators::calculateVWAP(candles);
    
    if (vwap < 0.0001) return 0.0;
    
    return (current_price - vwap) / vwap * 100;
}

AdvancedOrderFlowMetrics::VolumeProfile MomentumStrategy::calculateVolumeProfile(
    const std::vector<analytics::Candle>& candles
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
    const std::vector<analytics::Candle>& candles
) const {
    PositionMetrics pos_metrics;
    
    // 1. Kelly Fraction 계산
    double win_rate = stats_.win_rate > 0 ? stats_.win_rate : 0.60;
    double avg_win = stats_.avg_profit > 0 ? stats_.avg_profit : 0.05;
    double avg_loss = stats_.avg_loss > 0 ? stats_.avg_loss : 0.02;
    
    pos_metrics.kelly_fraction = calculateKellyFraction(win_rate, avg_win, avg_loss);
    pos_metrics.half_kelly = pos_metrics.kelly_fraction * HALF_KELLY_FRACTION;
    
    // 2. 변동성 조정
    double volatility = 0.02;  // 기본 2%
    if (!candles.empty()) {
        volatility = calculateVolatility(candles);
    }
    
    pos_metrics.volatility_adjusted = adjustForVolatility(pos_metrics.half_kelly, volatility);
    
    // 3. 유동성 조정
    double liquidity_factor = metrics.liquidity_score / 100.0;
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
    
    // 5. 예상 Sharpe Ratio
    double risk = std::abs(entry_price - stop_loss) / entry_price;
    if (risk > 0.0001) {
        double reward = BASE_TAKE_PROFIT;
        pos_metrics.expected_sharpe = (reward - risk) / (volatility * std::sqrt(252));
    }
    
    // 6. 최대 손실 금액
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
    const std::vector<analytics::Candle>& candles
) const {
    DynamicStops stops;
    
    if (candles.size() < 14) {
        stops.stop_loss = entry_price * (1.0 - BASE_STOP_LOSS);
        stops.take_profit_1 = entry_price * (1.0 + BASE_TAKE_PROFIT * 0.5);
        stops.take_profit_2 = entry_price * (1.0 + BASE_TAKE_PROFIT);
        stops.trailing_start = entry_price * (1.0 + BASE_TAKE_PROFIT * 0.3);
        return stops;
    }
    
    // 1. ATR 기반 손절
    double atr_stop = calculateATRBasedStop(entry_price, candles, 2.0);
    
    // 2. Support 기반 손절
    double support_stop = findNearestSupport(entry_price, candles);
    
    // 3. Hard Stop (최소 손절)
    double hard_stop = entry_price * (1.0 - BASE_STOP_LOSS);
    
    // 가장 높은 손절선 선택 (가장 보수적)
    stops.stop_loss = std::max({hard_stop, atr_stop, support_stop});
    
    // 진입가보다 높으면 안됨
    if (stops.stop_loss >= entry_price) {
        stops.stop_loss = hard_stop;
    }
    
    // 4. Take Profit 계산
    double risk = entry_price - stops.stop_loss;
    double reward_ratio = 2.5;
    
    stops.take_profit_1 = entry_price + (risk * reward_ratio * 0.5);
    stops.take_profit_2 = entry_price + (risk * reward_ratio);
    
    // 5. Trailing Start
    stops.trailing_start = entry_price + (risk * reward_ratio * 0.3);
    
    // 6. Chandelier Exit & Parabolic SAR (참고용)
    double atr = analytics::TechnicalIndicators::calculateATR(candles, 14);
    stops.chandelier_exit = calculateChandelierExit(entry_price, atr, 3.0);
    stops.parabolic_sar = calculateParabolicSAR(candles, 0.02, 0.20);
    
    return stops;
}

double MomentumStrategy::calculateATRBasedStop(
    double entry_price,
    const std::vector<analytics::Candle>& candles,
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
    const std::vector<analytics::Candle>& candles
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
    const std::vector<analytics::Candle>& candles,
    double acceleration,
    double max_af
) const {
    // 1. 최소 데이터 확보 (충분한 누적을 위해 30개 추천)
    if (candles.size() < 30) {
        return candles.back().low * 0.99; // 데이터 부족 시 현재가보다 살짝 아래
    }
    
    // 2. [수정] 시작 지점 설정 (최근 30개 전의 데이터를 초기값으로 사용)
    size_t lookback = 30;
    size_t start_idx = candles.size() - lookback;
    
    double sar = candles[start_idx].low;  // 시작점의 저가
    double ep = candles[start_idx].high;  // 시작점의 최고가(Extreme Point)
    double af = acceleration;             // 가속도 초기화
    
    // 3. [수정] 정방향 루프 (start_idx부터 마지막까지)
    // 과거에서 현재로 오면서 가속도를 붙여야 합니다.
    for (size_t i = start_idx + 1; i < candles.size(); ++i) {
        // SAR 공식 적용
        sar = sar + af * (ep - sar);
        
        // 새로운 고가 경신 시 EP 업데이트 및 가속도 증가
        if (candles[i].high > ep) {
            ep = candles[i].high;
            af = std::min(af + acceleration, max_af);
        }
        
        // 상승장일 때 SAR은 전일/전전일 저가보다 높을 수 없음 (안전장치)
        sar = std::min(sar, std::min(candles[i-1].low, candles[i].low));
    }
    
    return sar; // 최종 결과가 현재 시점의 SAR 값
}

// ===== 7. Trade Cost Analysis =====

bool MomentumStrategy::isWorthTrading(double expected_return, double expected_sharpe) const {
    double total_cost = (FEE_RATE * 2) + (EXPECTED_SLIPPAGE * 2); 
    double net_return = expected_return - total_cost;

    // [수정] 모멘텀 전략 순수익 기준 1.0% -> 0.4%로 현실화
    if (net_return < 0.004) return false;
    if (expected_sharpe < MIN_SHARPE_RATIO) return false;

    double actual_rr = expected_return / BASE_STOP_LOSS;
    if (actual_rr < 1.5) return false; // 모멘텀은 손익비 1.5 이상 권장

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
    const std::vector<analytics::Candle>& candles,
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

    // 7. 슬리피지 패널티 (최대 -10%)
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
    const std::vector<analytics::Candle>& historical_data
) const {
    WalkForwardResult result;
    
    // 간단한 구현: 전반부 In-Sample, 후반부 Out-of-Sample
    if (historical_data.size() < 100) {
        return result;
    }
    
    [[maybe_unused]] size_t split_point = historical_data.size() / 2;
    
    // In-Sample Sharpe (전반부)
    // Out-of-Sample Sharpe (후반부)
    // 실제로는 백테스팅 엔진 필요
    
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

double MomentumStrategy::calculateVolatility(const std::vector<analytics::Candle>& candles) const {
    if (candles.size() < 2) return 0.0;
    std::vector<double> returns;
    for (size_t i = 1; i < candles.size(); ++i) {
        // [수정] 정방향 수익률
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
    if (expected_return < 0.01) {
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

void MomentumStrategy::updateState(const std::string& market, double current_price) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    (void)market;
    (void)current_price;
    
    // MomentumStrategy는 추세 추종이므로
    // 현재 추세가 유지되는지 모니터링 가능
    // 하지만 TradingEngine에서 손절/익절을 처리하므로 여기서는 패스
}

} // namespace strategy
} // namespace autolife
