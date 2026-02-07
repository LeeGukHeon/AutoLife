#include "analytics/TechnicalIndicators.h"
#include "common/Logger.h"
#include <cmath>
#include <algorithm>
#include <numeric>

namespace autolife {
namespace analytics {

// RSI 계산 (Wilder's Smoothing 방식)
double TechnicalIndicators::calculateRSI(const std::vector<double>& prices, int period) {
    if (prices.size() < static_cast<size_t>(period + 1)) {
        return 50.0; // 데이터 부족 시 중립
    }
    
    std::vector<double> gains, losses;
    
    // 가격 변화 계산 (최신이 앞)
    for (size_t i = 1; i < prices.size() && i <= static_cast<size_t>(period); ++i) {
        double change = prices[i-1] - prices[i]; // 최신 - 이전
        
        if (change > 0) {
            gains.push_back(change);
            losses.push_back(0);
        } else {
            gains.push_back(0);
            losses.push_back(std::abs(change));
        }
    }
    
    if (gains.empty()) return 50.0;
    
    // Wilder's Smoothing 평균
    double avg_gain = std::accumulate(gains.begin(), gains.end(), 0.0) / gains.size();
    double avg_loss = std::accumulate(losses.begin(), losses.end(), 0.0) / losses.size();
    
    if (avg_loss < 0.0000001) return 100.0; // 손실 없음 = 강한 상승
    
    double rs = avg_gain / avg_loss;
    double rsi = 100.0 - (100.0 / (1.0 + rs));
    
    return rsi;
}

// MACD 계산
TechnicalIndicators::MACDResult TechnicalIndicators::calculateMACD(
    const std::vector<double>& prices,
    int fast,
    int slow,
    int signal_period  // <- 여기
) {
    MACDResult result;
    
    if (prices.size() < static_cast<size_t>(slow)) {
        return result;
    }
    
    double fast_ema = calculateEMA(prices, fast);
    double slow_ema = calculateEMA(prices, slow);
    
    result.macd = fast_ema - slow_ema;
    
    // Signal Line 계산 추가 (warning 제거)
    std::vector<double> macd_values = {result.macd};
    if (macd_values.size() >= static_cast<size_t>(signal_period)) {
        result.signal = calculateEMA(macd_values, signal_period);
    } else {
        result.signal = result.macd;
    }
    
    result.histogram = result.macd - result.signal;
    
    return result;
}

// Bollinger Bands 계산
TechnicalIndicators::BollingerBands TechnicalIndicators::calculateBollingerBands(
    const std::vector<double>& prices,
    double current_price,
    int period,
    double std_dev_mult
) {
    BollingerBands result;
    
    if (prices.size() < static_cast<size_t>(period)) {
        return result;
    }
    
    // Middle Band (SMA)
    result.middle = calculateSMA(prices, period);
    
    // Standard Deviation
    double std_dev = calculateStandardDeviation(prices, result.middle);
    
    // Upper & Lower Bands
    result.upper = result.middle + (std_dev * std_dev_mult);
    result.lower = result.middle - (std_dev * std_dev_mult);
    
    // Band Width (변동성)
    result.width = result.upper - result.lower;
    
    // %B (현재가 위치: 0~1)
    if (result.width > 0.0001) {
        result.percent_b = (current_price - result.lower) / result.width;
    } else {
        result.percent_b = 0.5;
    }
    
    return result;
}

// ATR 계산 (Average True Range)
double TechnicalIndicators::calculateATR(const std::vector<Candle>& candles, int period) {
    if (candles.size() < static_cast<size_t>(period + 1)) {
        return 0.0;
    }
    
    std::vector<double> true_ranges;
    
    for (size_t i = 1; i < candles.size() && i <= static_cast<size_t>(period); ++i) {
        const auto& current = candles[i-1]; // 최신
        const auto& previous = candles[i];  // 이전
        
        // True Range = max(high-low, |high-prev_close|, |low-prev_close|)
        double tr1 = current.high - current.low;
        double tr2 = std::abs(current.high - previous.close);
        double tr3 = std::abs(current.low - previous.close);
        
        double true_range = std::max({tr1, tr2, tr3});
        true_ranges.push_back(true_range);
    }
    
    if (true_ranges.empty()) return 0.0;
    
    // ATR = 평균 True Range
    return std::accumulate(true_ranges.begin(), true_ranges.end(), 0.0) / true_ranges.size();
}

// EMA 계산 (Exponential Moving Average)
double TechnicalIndicators::calculateEMA(const std::vector<double>& prices, int period) {
    if (prices.empty() || prices.size() < static_cast<size_t>(period)) {
        return prices.empty() ? 0.0 : prices[0];
    }
    
    double multiplier = 2.0 / (period + 1.0);
    
    // 초기값: SMA
    double ema = calculateSMA(
        std::vector<double>(prices.end() - period, prices.end()),
        period
    );
    
    // EMA 계산 (최신 데이터부터)
    for (int i = period - 2; i >= 0; --i) {
        ema = (prices[i] - ema) * multiplier + ema;
    }
    
    return ema;
}

std::vector<double> TechnicalIndicators::calculateEMAVector(
    const std::vector<double>& prices,
    int period
) {
    std::vector<double> ema_values;
    
    if (prices.size() < static_cast<size_t>(period)) {
        return ema_values;
    }
    
    double multiplier = 2.0 / (period + 1.0);
    
    // 초기 SMA
    double ema = 0.0;
    for (int i = 0; i < period; ++i) {
        ema += prices[prices.size() - period + i];
    }
    ema /= period;
    ema_values.push_back(ema);
    
    // 나머지 EMA
    for (size_t i = prices.size() - period + 1; i < prices.size(); ++i) {
        ema = (prices[i] - ema) * multiplier + ema;
        ema_values.push_back(ema);
    }
    
    return ema_values;
}

// SMA 계산 (Simple Moving Average)
double TechnicalIndicators::calculateSMA(const std::vector<double>& prices, int period) {
    if (prices.empty() || prices.size() < static_cast<size_t>(period)) {
        return prices.empty() ? 0.0 : prices[0];
    }
    
    double sum = 0.0;
    for (int i = 0; i < period; ++i) {
        sum += prices[i];
    }
    
    return sum / period;
}

// Stochastic Oscillator 계산
TechnicalIndicators::StochasticResult TechnicalIndicators::calculateStochastic(
    const std::vector<Candle>& candles,
    int k_period,
    int d_period  // <- 여기
) {
    StochasticResult result;
    
    if (candles.size() < static_cast<size_t>(k_period)) {
        return result;
    }
    
    // 1. [핵심 수정] 최신 k_period 기간의 고가/저가 찾기
    // 인덱스를 뒤에서부터 k_period 개만큼 훑어야 합니다.
    size_t start_idx = candles.size() - k_period;
    double lowest = candles[start_idx].low;
    double highest = candles[start_idx].high;
    
    for (size_t i = start_idx; i < candles.size(); ++i) {
        lowest = std::min(lowest, candles[i].low);
        highest = std::max(highest, candles[i].high);
    }
    
    // 2. [핵심 수정] 현재가는 가장 마지막(back) 데이터입니다.
    double current_close = candles.back().close;
    
    if (highest - lowest > 0.0001) {
        result.k = ((current_close - lowest) / (highest - lowest)) * 100.0;
    } else {
        result.k = 50.0;
    }
    
    // %D: %K의 SMA (d_period 사용)
    std::vector<double> k_values = {result.k};
    if (k_values.size() >= static_cast<size_t>(d_period)) {
        result.d = calculateSMA(k_values, d_period);
    } else {
        result.d = result.k;
    }
    
    return result;
}

// VWAP 계산 (Volume Weighted Average Price)
double TechnicalIndicators::calculateVWAP(const std::vector<Candle>& candles) {
    if (candles.empty()) return 0.0;
    
    double cumulative_tpv = 0.0;  // Typical Price * Volume
    double cumulative_volume = 0.0;
    
    for (const auto& candle : candles) {
        double typical_price = (candle.high + candle.low + candle.close) / 3.0;
        cumulative_tpv += typical_price * candle.volume;
        cumulative_volume += candle.volume;
    }
    
    if (cumulative_volume < 0.0001) return 0.0;
    
    return cumulative_tpv / cumulative_volume;
}

// Trend Detection (추세 감지)
TechnicalIndicators::Trend TechnicalIndicators::detectTrend(
    const std::vector<double>& prices,
    int short_period,
    int long_period
) {
    if (prices.size() < static_cast<size_t>(long_period)) {
        return Trend::SIDEWAYS;
    }
    
    double short_ma = calculateSMA(prices, short_period);
    double long_ma = calculateSMA(prices, long_period);
    
    double diff_percent = ((short_ma - long_ma) / long_ma) * 100.0;
    
    if (diff_percent > 5.0) return Trend::STRONG_UPTREND;
    if (diff_percent > 2.0) return Trend::UPTREND;
    if (diff_percent < -5.0) return Trend::STRONG_DOWNTREND;
    if (diff_percent < -2.0) return Trend::DOWNTREND;
    
    return Trend::SIDEWAYS;
}

// Support Levels 찾기 (지지선)
std::vector<double> TechnicalIndicators::findSupportLevels(
    const std::vector<Candle>& candles,
    int lookback
) {
    std::vector<double> supports;
    
    if (candles.size() < static_cast<size_t>(lookback)) {
        return supports;
    }
    
    // Local Minimum 찾기
    for (size_t i = lookback; i < candles.size() - lookback; ++i) {
        if (isLocalMinimum(candles, i, lookback)) {
            supports.push_back(candles[i].low);
        }
    }
    
    // 중복 제거 및 정렬
    std::sort(supports.begin(), supports.end());
    supports.erase(std::unique(supports.begin(), supports.end()), supports.end());
    
    return supports;
}

// Resistance Levels 찾기 (저항선)
std::vector<double> TechnicalIndicators::findResistanceLevels(
    const std::vector<Candle>& candles,
    int lookback
) {
    std::vector<double> resistances;
    
    if (candles.size() < static_cast<size_t>(lookback)) {
        return resistances;
    }
    
    // Local Maximum 찾기
    for (size_t i = lookback; i < candles.size() - lookback; ++i) {
        if (isLocalMaximum(candles, i, lookback)) {
            resistances.push_back(candles[i].high);
        }
    }
    
    std::sort(resistances.begin(), resistances.end(), std::greater<double>());
    resistances.erase(std::unique(resistances.begin(), resistances.end()), resistances.end());
    
    return resistances;
}

// Fibonacci Retracement Levels
std::vector<double> TechnicalIndicators::calculateFibonacciLevels(double high, double low) {
    std::vector<double> levels;
    
    double diff = high - low;
    
    levels.push_back(high);                       // 100%
    levels.push_back(high - diff * 0.236);        // 76.4%
    levels.push_back(high - diff * 0.382);        // 61.8%
    levels.push_back(high - diff * 0.5);          // 50%
    levels.push_back(high - diff * 0.618);        // 38.2%
    levels.push_back(high - diff * 0.786);        // 21.4%
    levels.push_back(low);                        // 0%
    
    return levels;
}

// Volatility Ratio
double TechnicalIndicators::calculateVolatilityRatio(
    const std::vector<Candle>& candles,
    int period
) {
    if (candles.size() < static_cast<size_t>(period + 1)) {
        return 1.0;
    }
    
    double current_atr = calculateATR(
        std::vector<Candle>(candles.begin(), candles.begin() + 5),
        5
    );
    
    double avg_atr = calculateATR(candles, period);
    
    if (avg_atr < 0.0001) return 1.0;
    
    return current_atr / avg_atr;
}

// JSON → Candle 변환
std::vector<Candle> TechnicalIndicators::jsonToCandles(const nlohmann::json& json_candles) {
    std::vector<Candle> candles;
    if (!json_candles.is_array()) return candles;

    for (const auto& jc : json_candles) {
        Candle c;
        c.open = jc["opening_price"].get<double>();
        c.high = jc["high_price"].get<double>();
        c.low = jc["low_price"].get<double>();
        c.close = jc["trade_price"].get<double>();
        c.volume = jc["candle_acc_trade_volume"].get<double>();
        c.timestamp = jc["timestamp"].get<long long>();
        
        candles.push_back(c);
    }
    
    // [핵심 수정] 시간순(과거 -> 현재)으로 정렬
    // 업비트는 최신순으로 주므로, 배열을 뒤집거나 timestamp 기준으로 정렬해야 합니다.
    std::sort(candles.begin(), candles.end(), [](const Candle& a, const Candle& b) {
        return a.timestamp < b.timestamp;
    });
    
    return candles;
}

// Close 가격만 추출
std::vector<double> TechnicalIndicators::extractClosePrices(const std::vector<Candle>& candles) {
    std::vector<double> prices;
    prices.reserve(candles.size());
    
    for (const auto& candle : candles) {
        prices.push_back(candle.close);
    }
    
    return prices;
}

// ========== Private 헬퍼 함수들 ==========

double TechnicalIndicators::calculateStandardDeviation(
    const std::vector<double>& values,
    double mean
) {
    if (values.empty()) return 0.0;
    
    double sum_sq_diff = 0.0;
    
    for (const auto& value : values) {
        double diff = value - mean;
        sum_sq_diff += diff * diff;
    }
    
    return std::sqrt(sum_sq_diff / values.size());
}

double TechnicalIndicators::calculateMean(const std::vector<double>& values) {
    if (values.empty()) return 0.0;
    
    return std::accumulate(values.begin(), values.end(), 0.0) / values.size();
}

bool TechnicalIndicators::isLocalMinimum(
    const std::vector<Candle>& candles,
    size_t index,
    int lookback
) {
    double value = candles[index].low;
    
    for (int i = 1; i <= lookback; ++i) {
        if (index + i >= candles.size()) break;
        if (candles[index + i].low < value) return false;
        
        if (index >= static_cast<size_t>(i)) {
            if (candles[index - i].low < value) return false;
        }
    }
    
    return true;
}

bool TechnicalIndicators::isLocalMaximum(
    const std::vector<Candle>& candles,
    size_t index,
    int lookback
) {
    double value = candles[index].high;
    
    for (int i = 1; i <= lookback; ++i) {
        if (index + i >= candles.size()) break;
        if (candles[index + i].high > value) return false;
        
        if (index >= static_cast<size_t>(i)) {
            if (candles[index - i].high > value) return false;
        }
    }
    
    return true;
}

} // namespace analytics
} // namespace autolife
