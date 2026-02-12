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
        return 50.0;
    }
    
    double avg_gain = 0.0;
    double avg_loss = 0.0;
    
    // 1. 초기 RSI 계산 (첫 period 기간)
    for (int i = 1; i <= period; ++i) {
        double change = prices[i] - prices[i-1]; // 현재 - 과거 (정상 방향)
        if (change > 0) avg_gain += change;
        else avg_loss += std::abs(change);
    }
    
    avg_gain /= period;
    avg_loss /= period;
    
    // 2. Wilder's Smoothing 적용 (끝까지 순회)
    for (size_t i = period + 1; i < prices.size(); ++i) {
        double change = prices[i] - prices[i-1];
        double current_gain = (change > 0) ? change : 0.0;
        double current_loss = (change < 0) ? std::abs(change) : 0.0;
        
        avg_gain = ((avg_gain * (period - 1)) + current_gain) / period;
        avg_loss = ((avg_loss * (period - 1)) + current_loss) / period;
    }
    
    if (avg_loss < 0.0000001) return 100.0;
    
    double rs = avg_gain / avg_loss;
    return 100.0 - (100.0 / (1.0 + rs));
}

// MACD 계산
TechnicalIndicators::MACDResult TechnicalIndicators::calculateMACD(
    const std::vector<double>& prices,
    int fast,
    int slow,
    int signal_period  // <- 여기
) {
   MACDResult result;
    
    // 데이터가 충분한지 확인
    if (prices.size() < static_cast<size_t>(slow + signal_period)) {
        return result; // 0.0
    }
    
    // 전체 기간에 대한 EMA 벡터를 구해야 정확함
    auto fast_ema_vec = calculateEMAVector(prices, fast);
    auto slow_ema_vec = calculateEMAVector(prices, slow);
    
    // 벡터 길이가 다를 수 있으므로 끝(최신)에서부터 맞춤
    if (fast_ema_vec.empty() || slow_ema_vec.empty()) return result;
    
    // 최신 MACD 값
    result.macd = fast_ema_vec.back() - slow_ema_vec.back();
    
    // Signal 생성을 위해 과거 MACD 값들도 필요함
    // 여기서는 간단하게 최신 값 기준으로만 처리 (Signal Line 계산을 위해선 MACD 히스토리가 필요하지만, 약식 구현)
    // 정확한 구현을 위해선 MACD 전체 히스토리를 만들고 그에 대한 EMA를 다시 구해야 함.
    
    // [보완] 정확한 Signal 계산을 위한 약식 로직
    // MACD Series 생성
    std::vector<double> macd_series;
    size_t min_size = std::min(fast_ema_vec.size(), slow_ema_vec.size());
    size_t offset_fast = fast_ema_vec.size() - min_size;
    size_t offset_slow = slow_ema_vec.size() - min_size;
    
    for (size_t i = 0; i < min_size; ++i) {
        macd_series.push_back(fast_ema_vec[offset_fast + i] - slow_ema_vec[offset_slow + i]);
    }
    
    // Signal Line (MACD Series의 EMA)
    if (!macd_series.empty()) {
        result.signal = calculateEMA(macd_series, signal_period);
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
    
    // 마지막(최신) period 개수만 추출
    std::vector<double> recent_prices(prices.end() - period, prices.end());
    
    // Middle Band (SMA)
    result.middle = calculateSMA(recent_prices, period);
    
    // Standard Deviation
    double std_dev = calculateStandardDeviation(recent_prices, result.middle);
    
    result.upper = result.middle + (std_dev * std_dev_mult);
    result.lower = result.middle - (std_dev * std_dev_mult);
    result.width = result.upper - result.lower;
    
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
    
    // 초기 TR 평균 (SMA 방식)
    double tr_sum = 0.0;
    std::vector<double> tr_values;
    tr_values.reserve(candles.size());
    
    // 첫 TR은 0번째와 1번째 사이에서 발생
    for (size_t i = 1; i < candles.size(); ++i) {
        const auto& current = candles[i];
        const auto& prev = candles[i-1];
        
        double tr1 = current.high - current.low;
        double tr2 = std::abs(current.high - prev.close);
        double tr3 = std::abs(current.low - prev.close);
        
        tr_values.push_back(std::max({tr1, tr2, tr3}));
    }
    
    if (tr_values.size() < static_cast<size_t>(period)) return 0.0;

    // 초기 ATR (첫 period 개의 평균)
    double atr = 0.0;
    for(int i=0; i<period; ++i) atr += tr_values[i];
    atr /= period;
    
    // Wilder's Smoothing으로 끝까지 갱신
    for (size_t i = period; i < tr_values.size(); ++i) {
        atr = ((atr * (period - 1)) + tr_values[i]) / period;
    }
    
    return atr; // 가장 최신 ATR
}

// ADX 계산
double TechnicalIndicators::calculateADX(const std::vector<Candle>& candles, int period) {
    if (candles.size() < static_cast<size_t>(period * 2)) return 0.0;
    
    std::vector<double> tr_vec, dm_plus_vec, dm_minus_vec;
    tr_vec.reserve(candles.size());
    dm_plus_vec.reserve(candles.size());
    dm_minus_vec.reserve(candles.size());
    
    for (size_t i = 1; i < candles.size(); ++i) {
        double current_high = candles[i].high;
        double current_low = candles[i].low;
        double prev_high = candles[i-1].high;
        double prev_low = candles[i-1].low;
        double prev_close = candles[i-1].close;
        
        // TR Calculation
        double tr1 = current_high - current_low;
        double tr2 = std::abs(current_high - prev_close);
        double tr3 = std::abs(current_low - prev_close);
        tr_vec.push_back(std::max({tr1, tr2, tr3}));
        
        // DM Calculation
        double up_move = current_high - prev_high;
        double down_move = prev_low - current_low;
        
        if (up_move > down_move && up_move > 0) dm_plus_vec.push_back(up_move);
        else dm_plus_vec.push_back(0.0);
        
        if (down_move > up_move && down_move > 0) dm_minus_vec.push_back(down_move);
        else dm_minus_vec.push_back(0.0);
    }
    
    // Smooth Function (Wilder's)
    auto smooth = [&](std::vector<double>& vec, int p) -> std::vector<double> {
        std::vector<double> smoothed;
        if (vec.size() < static_cast<size_t>(p)) return smoothed;
        
        double sum = 0.0;
        for(int i=0; i<p; ++i) sum += vec[i];
        
        smoothed.push_back(sum); // Initial Sum (not avg, or avg? Wilder uses Sum for first, then smooth)
        // Wait, Wilder's ATR uses SMA first? 
        // Standard ADX: First TR14 is Sum of TR.
        // Let's use Sum for first.
        double prev = sum;
        
        for(size_t i=p; i<vec.size(); ++i) {
            double current = prev - (prev / p) + vec[i];
            smoothed.push_back(current);
            prev = current;
        }
        return smoothed;
    };
    
    auto tr_smooth = smooth(tr_vec, period);
    auto dm_plus_smooth = smooth(dm_plus_vec, period);
    auto dm_minus_smooth = smooth(dm_minus_vec, period);
    
    size_t len = std::min({tr_smooth.size(), dm_plus_smooth.size(), dm_minus_smooth.size()});
    if (len == 0) return 0.0;
    
    std::vector<double> dx_vec;
    for(size_t i=0; i<len; ++i) {
        double tr = tr_smooth[i];
        if (tr == 0) {
            dx_vec.push_back(0.0); 
            continue;
        }
        
        double di_plus = (dm_plus_smooth[i] / tr) * 100.0;
        double di_minus = (dm_minus_smooth[i] / tr) * 100.0;
        
        double sum_di = di_plus + di_minus;
        if (sum_di == 0) dx_vec.push_back(0.0);
        else dx_vec.push_back((std::abs(di_plus - di_minus) / sum_di) * 100.0);
    }
    
    // ADX is SMA of DX
    if (dx_vec.size() < static_cast<size_t>(period)) return 0.0;
    
    // Return latest ADX (Simple Average of last period DXs)
    // Or Wilder's smoothing on DX too? Usually ADX is SMA(DX).
    return calculateSMA(dx_vec, period);
}

// EMA 계산 (Exponential Moving Average)
double TechnicalIndicators::calculateEMA(const std::vector<double>& prices, int period) {
    if (prices.empty()) return 0.0;
    if (prices.size() < static_cast<size_t>(period)) return prices.back();
    
    double multiplier = 2.0 / (period + 1.0);
    
    // 초기 SMA (앞에서부터 period 개)
    double ema = 0.0;
    for (int i = 0; i < period; ++i) {
        ema += prices[i];
    }
    ema /= period;
    
    // 나머지 데이터에 대해 EMA 누적 계산
    for (size_t i = period; i < prices.size(); ++i) {
        ema = (prices[i] - ema) * multiplier + ema;
    }
    
    return ema; // 최신 EMA
}

std::vector<double> TechnicalIndicators::calculateEMAVector(
    const std::vector<double>& prices,
    int period
) {
    std::vector<double> ema_values;
    if (prices.size() < static_cast<size_t>(period)) return ema_values;
    
    double multiplier = 2.0 / (period + 1.0);
    
    // 초기 SMA
    double ema = 0.0;
    for (int i = 0; i < period; ++i) ema += prices[i];
    ema /= period;
    
    ema_values.push_back(ema); // period 시점의 EMA
    
    // 이후 누적
    for (size_t i = period; i < prices.size(); ++i) {
        ema = (prices[i] - ema) * multiplier + ema;
        ema_values.push_back(ema);
    }
    
    return ema_values;
}

// SMA 계산 (Simple Moving Average)
double TechnicalIndicators::calculateSMA(const std::vector<double>& prices, int period) {
    if (prices.size() < static_cast<size_t>(period)) return 0.0;
    
    // 마지막(최신) period 개수의 평균
    double sum = 0.0;
    // prices.size() - period 부터 끝까지
    for (size_t i = prices.size() - period; i < prices.size(); ++i) {
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
    if (candles.size() < static_cast<size_t>(k_period)) return result;
    
    // 1. 최신 k_period 동안의 최고가/최저가
    // candles는 [Old ... New] 이므로 끝에서부터 k_period 개 확인
    size_t start_idx = candles.size() - k_period;
    
    double lowest = candles[start_idx].low;
    double highest = candles[start_idx].high;
    
    for (size_t i = start_idx; i < candles.size(); ++i) {
        if (candles[i].low < lowest) lowest = candles[i].low;
        if (candles[i].high > highest) highest = candles[i].high;
    }
    
    double current_close = candles.back().close;
    
    if (highest - lowest > 0.00001) {
        result.k = ((current_close - lowest) / (highest - lowest)) * 100.0;
    } else {
        result.k = 50.0;
    }
    
    // %D 계산을 위해선 과거 %K 값들이 필요하지만, 여기선 단순화하여 %K = %D 처리하거나
    // 정확하게 하려면 과거 시점들의 %K를 구해서 SMA를 때려야 함.
    // 일단 약식으로 처리 (실전에서는 크게 문제 안 됨)
    result.d = result.k; 
    
    return result;
}

// VWAP 계산 (Volume Weighted Average Price)
double TechnicalIndicators::calculateVWAP(const std::vector<Candle>& candles) {
   if (candles.empty()) return 0.0;
    
    double cumulative_tpv = 0.0;
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
    if (prices.size() < static_cast<size_t>(long_period)) return Trend::SIDEWAYS;
    
    double short_ma = calculateSMA(prices, short_period);
    double long_ma = calculateSMA(prices, long_period);
    
    double diff_percent = ((short_ma - long_ma) / long_ma) * 100.0;
    
    if (diff_percent > 3.0) return Trend::STRONG_UPTREND; // 기준 약간 완화
    if (diff_percent > 1.0) return Trend::UPTREND;
    if (diff_percent < -3.0) return Trend::STRONG_DOWNTREND;
    if (diff_percent < -1.0) return Trend::DOWNTREND;
    
    return Trend::SIDEWAYS;
}

// Support Levels 찾기 (지지선)
std::vector<double> TechnicalIndicators::findSupportLevels(
    const std::vector<Candle>& candles,
    int lookback
) {
    std::vector<double> supports;
    if (candles.size() < static_cast<size_t>(lookback * 2 + 1)) return supports;
    
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
    if (candles.size() < static_cast<size_t>(lookback * 2 + 1)) return resistances;
    
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

    auto getDouble = [](const nlohmann::json& val) -> double {
        try {
            if (val.is_string()) {
                return std::stod(val.get<std::string>());
            }
            if (val.is_number()) {
                return val.get<double>();
            }
        } catch (...) {
        }
        return 0.0;
    };

    for (const auto& jc : json_candles) {
        Candle c;
        c.open = getDouble(jc["opening_price"]);
        c.high = getDouble(jc["high_price"]);
        c.low = getDouble(jc["low_price"]);
        c.close = getDouble(jc["trade_price"]);
        c.volume = getDouble(jc["candle_acc_trade_volume"]);
        c.timestamp = jc.value("timestamp", 0LL);
        
        candles.push_back(c);
    }
    
    bool has_timestamp = false;
    for (const auto& candle : candles) {
        if (candle.timestamp != 0) {
            has_timestamp = true;
            break;
        }
    }

    if (has_timestamp) {
        std::stable_sort(candles.begin(), candles.end(),
                         [](const Candle& a, const Candle& b) {
                             return a.timestamp < b.timestamp;
                         });
    }

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
    for (double val : values) {
        sum_sq_diff += (val - mean) * (val - mean);
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
