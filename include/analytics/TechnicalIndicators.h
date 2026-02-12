#pragma once

#include <vector>
#include <string>
#include <map>
#include <nlohmann/json.hpp>
#include "common/Types.h"

namespace autolife {
namespace analytics {

// Candle struct is now in common/Types.h

// Technical Indicators - 검증된 공식으로 구현
class TechnicalIndicators {
public:
    // RSI (Relative Strength Index) - 14일 기준
    // 70 이상: 과매수, 30 이하: 과매도
    static double calculateRSI(const std::vector<double>& prices, int period = 14);
    
    // MACD (Moving Average Convergence Divergence)
    struct MACDResult {
        double macd;        // MACD 선
        double signal;      // Signal 선
        double histogram;   // MACD - Signal
        
        MACDResult() : macd(0), signal(0), histogram(0) {}
    };
    static MACDResult calculateMACD(const std::vector<double>& prices,
                                     int fast = 12, int slow = 26, int signal_period = 9);
    
    // Bollinger Bands - 가격 밴드
    struct BollingerBands {
        double upper;       // 상단 밴드
        double middle;      // 중간선 (SMA)
        double lower;       // 하단 밴드
        double width;       // 밴드 폭 (변동성)
        double percent_b;   // %B (현재가가 밴드 내 어디에 위치하는지: 0~1)
        
        BollingerBands() : upper(0), middle(0), lower(0), width(0), percent_b(0) {}
    };
    static BollingerBands calculateBollingerBands(const std::vector<double>& prices,
                                                    double current_price,
                                                    int period = 20,
                                                    double std_dev_mult = 2.0);
    
    // ATR (Average True Range) - 변동성 측정, 손절라인 설정에 사용
    static double calculateATR(const std::vector<Candle>& candles, int period = 14);

    // ADX (Average Directional Index) - 추세 강도 측정
    // 25 이상: 추세장, 20 미만: 비추세(횡보)
    static double calculateADX(const std::vector<Candle>& candles, int period = 14);
    
    // EMA (Exponential Moving Average) - 최근 가격에 더 큰 가중치
    static double calculateEMA(const std::vector<double>& prices, int period);
    static std::vector<double> calculateEMAVector(const std::vector<double>& prices, int period);
    
    // SMA (Simple Moving Average) - 단순 이동평균
    static double calculateSMA(const std::vector<double>& prices, int period);
    
    // Stochastic Oscillator - 단기 과매수/과매도
    struct StochasticResult {
        double k;  // %K (Fast)
        double d;  // %D (Slow, Signal)
        
        StochasticResult() : k(0), d(0) {}
    };
    static StochasticResult calculateStochastic(const std::vector<Candle>& candles,
                                                 int k_period = 14,
                                                 int d_period = 3);
    
    // Volume Weighted Average Price (VWAP) - 거래량 가중 평균가
    static double calculateVWAP(const std::vector<Candle>& candles);
    
    // Trend Detection - 추세 판단
    enum class Trend {
        STRONG_UPTREND,     // 강한 상승
        UPTREND,            // 상승
        SIDEWAYS,           // 횡보
        DOWNTREND,          // 하락
        STRONG_DOWNTREND    // 강한 하락
    };
    static Trend detectTrend(const std::vector<double>& prices, int short_period = 20, int long_period = 50);
    
    // Support/Resistance Levels - 지지/저항선 자동 감지
    static std::vector<double> findSupportLevels(const std::vector<Candle>& candles, int lookback = 50);
    static std::vector<double> findResistanceLevels(const std::vector<Candle>& candles, int lookback = 50);
    
    // Fibonacci Retracement Levels - 피보나치 되돌림
    static std::vector<double> calculateFibonacciLevels(double high, double low);
    
    // Volatility Ratio - 현재 변동성 / 평균 변동성
    static double calculateVolatilityRatio(const std::vector<Candle>& candles, int period = 20);
    
    // Helper: JSON candles를 Candle 구조체로 변환
    static std::vector<Candle> jsonToCandles(const nlohmann::json& json_candles);
    
    // Helper: 가격 배열 추출
    static std::vector<double> extractClosePrices(const std::vector<Candle>& candles);
    
private:
    // 내부 헬퍼 함수들
    static double calculateStandardDeviation(const std::vector<double>& values, double mean);
    static double calculateMean(const std::vector<double>& values);
    static bool isLocalMinimum(const std::vector<Candle>& candles, size_t index, int lookback);
    static bool isLocalMaximum(const std::vector<Candle>& candles, size_t index, int lookback);
};

} // namespace analytics
} // namespace autolife
