#pragma once

#include "strategy/IStrategy.h"
#include "network/UpbitHttpClient.h"
#include <memory>
#include <mutex>
#include <deque>
#include <array>
#include <map>
#include <set>

namespace autolife {
namespace strategy {

// ===== Mean Reversion Types =====

enum class MeanReversionType {
    NONE,
    BOLLINGER_OVERSOLD,      // BB 하단 과매도
    RSI_OVERSOLD,            // RSI 과매도
    Z_SCORE_EXTREME,         // Z-Score 극단값
    KALMAN_DEVIATION,        // Kalman Filter 괴리
    VWAP_DEVIATION           // VWAP 괴리
};

// ✅ 독립적인 enum (이름 변경)
enum class MRMarketRegime {
    MEAN_REVERTING,
    TRENDING,
    RANDOM_WALK,
    UNKNOWN
};

// ===== Statistical Metrics =====

struct StatisticalMetrics {
    double z_score_20;
    double z_score_50;
    double z_score_100;
    double hurst_exponent;           // 0.5=랜덤, <0.5=평균회귀, >0.5=추세
    double half_life;                // 회귀 반감기 (캔들 수)
    double adf_statistic;            // ADF 검정 통계량
    bool is_stationary;              // 정상성 여부
    double autocorrelation;          // 자기상관
    
    StatisticalMetrics()
        : z_score_20(0), z_score_50(0), z_score_100(0)
        , hurst_exponent(0.5), half_life(0), adf_statistic(0)
        , is_stationary(false), autocorrelation(0)
    {}
};

// ===== Kalman Filter State =====

struct KalmanFilterState {
    double estimated_mean;
    double estimated_variance;
    double prediction_error;
    double kalman_gain;
    double process_noise;
    double measurement_noise;
    
    KalmanFilterState()
        : estimated_mean(0), estimated_variance(1.0)
        , prediction_error(0), kalman_gain(0)
        , process_noise(0.001), measurement_noise(0.1)
    {}
};

// ===== Bollinger Bands =====

struct BollingerBands {
    double upper;
    double middle;
    double lower;
    double bandwidth;                // (상단-하단) / 중간
    double percent_b;                // 현재가 위치 (0-1)
    bool squeeze;                    // BB Squeeze 여부
    
    BollingerBands()
        : upper(0), middle(0), lower(0)
        , bandwidth(0), percent_b(0.5), squeeze(false)
    {}
};

// ===== Multi-Period RSI =====

struct MultiPeriodRSI {
    double rsi_7;
    double rsi_14;
    double rsi_21;
    double rsi_composite;            // 가중평균
    bool oversold_7;
    bool oversold_14;
    bool oversold_21;
    int oversold_count;
    
    MultiPeriodRSI()
        : rsi_7(50), rsi_14(50), rsi_21(50)
        , rsi_composite(50)
        , oversold_7(false), oversold_14(false), oversold_21(false)
        , oversold_count(0)
    {}
};

// ===== VWAP Analysis =====

struct VWAPAnalysis {
    double vwap;
    double vwap_upper;               // VWAP + 1 std
    double vwap_lower;               // VWAP - 1 std
    double current_deviation_pct;   // 현재가 괴리율
    double deviation_z_score;        // 괴리의 Z-Score
    
    VWAPAnalysis()
        : vwap(0), vwap_upper(0), vwap_lower(0)
        , current_deviation_pct(0), deviation_z_score(0)
    {}
};

// ===== Reversion Signal =====

struct MeanReversionSignalMetrics {
    MeanReversionType type;
    MRMarketRegime regime;
    double strength;                 // 0-1
    double confidence;               // 통계적 신뢰도
    double expected_reversion_pct;   // 예상 회귀 폭
    double reversion_probability;    // 회귀 확률
    double time_to_revert;           // 예상 회귀 시간 (분)
    bool is_valid;
    
    MeanReversionSignalMetrics()
        : type(MeanReversionType::NONE)
        , regime(MRMarketRegime::UNKNOWN)
        , strength(0), confidence(0)
        , expected_reversion_pct(0), reversion_probability(0)
        , time_to_revert(0), is_valid(false)
    {}
};

// ===== Position Tracking =====

struct MeanReversionPositionData {
    std::string market;
    double entry_price;
    double target_mean;              // 목표 평균가
    double initial_deviation;        // 진입시 괴리도
    double highest_price;
    double trailing_stop;
    long long entry_timestamp;
    bool tp1_hit;
    bool tp2_hit;
    
    MeanReversionPositionData()
        : entry_price(0), target_mean(0), initial_deviation(0)
        , highest_price(0), trailing_stop(0)
        , entry_timestamp(0), tp1_hit(false), tp2_hit(false)
    {}
};

// ===== Rolling Statistics =====

struct MeanReversionRollingStatistics {
    double rolling_win_rate;
    double avg_holding_time_minutes;
    double rolling_profit_factor;
    double avg_reversion_time;       // 평균 회귀 소요 시간
    int total_reversions_detected;
    int successful_reversions;
    double avg_reversion_accuracy;   // 예측 정확도
    
    MeanReversionRollingStatistics()
        : rolling_win_rate(0), avg_holding_time_minutes(0)
        , rolling_profit_factor(0), avg_reversion_time(0)
        , total_reversions_detected(0), successful_reversions(0)
        , avg_reversion_accuracy(0)
    {}
};

// ===== Mean Reversion Strategy =====

class MeanReversionStrategy : public IStrategy {
public:
    MeanReversionStrategy(std::shared_ptr<network::UpbitHttpClient> client);
    
    // IStrategy 구현
    StrategyInfo getInfo() const override;
    
    Signal generateSignal(
        const std::string& market,
        const analytics::CoinMetrics& metrics,
        const std::vector<analytics::Candle>& candles,
        double current_price
    ) override;
    
    bool shouldEnter(
        const std::string& market,
        const analytics::CoinMetrics& metrics,
        const std::vector<analytics::Candle>& candles,
        double current_price
    ) override;
    
    bool shouldExit(
        const std::string& market,
        double entry_price,
        double current_price,
        double holding_time_seconds
    ) override;
    
    double calculateStopLoss(
        double entry_price,
        const std::vector<analytics::Candle>& candles
    ) override;
    
    double calculateTakeProfit(
        double entry_price,
        const std::vector<analytics::Candle>& candles
    ) override;
    
    double calculatePositionSize(
        double capital,
        double entry_price,
        double stop_loss,
        const analytics::CoinMetrics& metrics
    ) override;
    
    void setEnabled(bool enabled) override;
    bool isEnabled() const override;
    
    Statistics getStatistics() const override;
    void updateStatistics(const std::string& market, bool is_win, double profit_loss) override;
    
    // === 추가 기능 ===
    
    double updateTrailingStop(
        double entry_price,
        double highest_price,
        double current_price
    );
    
    bool shouldMoveToBreakeven(
        double entry_price,
        double current_price
    );
    
    MeanReversionRollingStatistics getRollingStatistics() const;

private:
    std::shared_ptr<network::UpbitHttpClient> client_;
    bool enabled_;
    Statistics stats_;
    mutable std::mutex mutex_;
    
    // [신규 추가] 중복 진입 방지용
    std::set<std::string> active_positions_;

    // 이력
    std::deque<double> recent_returns_;
    std::deque<double> recent_holding_times_;
    std::deque<long long> trade_timestamps_;
    std::deque<double> reversion_time_history_;
    
    MeanReversionRollingStatistics rolling_stats_;
    long long last_signal_time_;
    
    // 포지션 추적 (내부 로직용)
    std::map<std::string, MeanReversionPositionData> position_data_;
    
    // Kalman Filter States (마켓별)
    std::map<std::string, KalmanFilterState> kalman_states_;
    
    // ===== API 호출 제어 =====
    
    mutable long long last_orderbook_fetch_time_;
    mutable nlohmann::json cached_orderbook_;
    static constexpr int ORDERBOOK_CACHE_MS = 2000;
    
    mutable std::map<std::string, long long> candle_cache_time_;
    mutable std::map<std::string, std::vector<analytics::Candle>> candle_cache_;
    static constexpr int CANDLE_CACHE_MS = 5000;
    
    mutable std::deque<long long> api_call_timestamps_;
    static constexpr int MAX_ORDERBOOK_CALLS_PER_SEC = 8;
    static constexpr int MAX_CANDLE_CALLS_PER_SEC = 8;
    
    // ===== 거래 빈도 제어 =====
    
    int daily_trades_count_;
    int hourly_trades_count_;
    long long current_day_start_;
    long long current_hour_start_;
    
    static constexpr int MAX_DAILY_REVERSION_TRADES = 12;
    static constexpr int MAX_HOURLY_REVERSION_TRADES = 4;
    
    // ===== 서킷 브레이커 =====
    
    int consecutive_losses_;
    bool circuit_breaker_active_;
    long long circuit_breaker_until_;
    
    static constexpr int MAX_CONSECUTIVE_LOSSES = 4;
    static constexpr long long CIRCUIT_BREAKER_COOLDOWN_MS = 7200000;  // 2시간
    
    // ===== 파라미터 =====
    
    static constexpr double UPBIT_FEE_RATE = 0.0005;
    static constexpr double EXPECTED_SLIPPAGE = 0.0003;
    static constexpr double MIN_ORDER_AMOUNT_KRW = 5000.0;
    
    // Z-Score 임계값
    static constexpr double Z_SCORE_EXTREME = -2.0;      // -2 이하 = 과매도
    static constexpr double Z_SCORE_EXIT = -0.5;         // -0.5 회복시 청산
    
    // RSI 임계값
    static constexpr double RSI_OVERSOLD = 30.0;
    static constexpr double RSI_EXIT = 50.0;
    
    // Bollinger Bands
    static constexpr double BB_PERIOD = 20;
    static constexpr double BB_STD_DEV = 2.0;
    static constexpr double BB_SQUEEZE_THRESHOLD = 0.05;  // 5% 이하
    
    // Hurst Exponent (평균회귀 판별)
    static constexpr double HURST_MEAN_REVERTING = 0.45;  // < 0.45 = 강한 평균회귀
    
    // 손절/익절
    static constexpr double BASE_STOP_LOSS = 0.025;       // 2.5%
    static constexpr double BASE_TAKE_PROFIT_1 = 0.02;    // 2%
    static constexpr double BASE_TAKE_PROFIT_2 = 0.04;    // 4%
    static constexpr double TRAILING_ACTIVATION = 0.025;  // 2.5%
    static constexpr double TRAILING_DISTANCE = 0.015;    // 1.5%
    
    static constexpr double MAX_HOLDING_TIME_MINUTES = 240.0;  // 4시간
    static constexpr double MIN_LIQUIDITY_SCORE = 50.0;
    static constexpr double MIN_SIGNAL_STRENGTH = 0.65;
    static constexpr double MIN_REVERSION_PROBABILITY = 0.70;  // 70% 이상
    static constexpr double MAX_POSITION_SIZE = 0.15;          // 15%
    static constexpr double BREAKEVEN_TRIGGER = 0.015;         // 1.5%
    static constexpr int MIN_SIGNAL_INTERVAL_SEC = 600;        // 10분
    
    // ===== API 호출 관리 =====
    
    bool canMakeOrderBookAPICall() const;
    bool canMakeCandleAPICall() const;
    void recordAPICall() const;
    
    nlohmann::json getCachedOrderBook(const std::string& market);
    std::vector<analytics::Candle> getCachedCandles(const std::string& market, int count);
    
    // ===== 거래 빈도 관리 =====
    
    bool canTradeNow();
    void recordTrade();
    void resetDailyCounters();
    void resetHourlyCounters();
    
    // ===== 서킷 브레이커 =====
    
    void checkCircuitBreaker();
    void activateCircuitBreaker();
    bool isCircuitBreakerActive() const;
    
    // ===== 1. Statistical Analysis =====
    
    StatisticalMetrics calculateStatisticalMetrics(
        const std::vector<analytics::Candle>& candles
    ) const;
    
    double calculateZScore(
        double value,
        const std::vector<double>& history
    ) const;
    
    double calculateHurstExponent(
        const std::vector<analytics::Candle>& candles
    ) const;
    
    double calculateHalfLife(
        const std::vector<analytics::Candle>& candles
    ) const;
    
    double calculateADFStatistic(
        const std::vector<double>& prices
    ) const;
    
    double calculateAutocorrelation(
        const std::vector<double>& prices,
        int lag = 1
    ) const;
    
    // ===== 2. Kalman Filter =====
    
    void updateKalmanFilter(
        const std::string& market,
        double observed_price
    );
    
    KalmanFilterState& getKalmanState(const std::string& market);
    
    // ===== 3. Bollinger Bands =====
    
    BollingerBands calculateBollingerBands(
        const std::vector<analytics::Candle>& candles,
        int period,
        double std_dev_mult
    ) const;
    
    bool isBBSqueeze(const BollingerBands& bb) const;
    
    // ===== 4. RSI Analysis =====
    
    MultiPeriodRSI calculateMultiPeriodRSI(
        const std::vector<analytics::Candle>& candles
    ) const;
    
    double calculateRSI(
        const std::vector<analytics::Candle>& candles,
        int period
    ) const;
    
    // ===== 5. VWAP Analysis =====
    
    VWAPAnalysis calculateVWAPAnalysis(
        const std::vector<analytics::Candle>& candles,
        double current_price
    ) const;
    
    // ===== 6. Market Regime Detection =====
    
    MRMarketRegime detectMarketRegime(
        const StatisticalMetrics& stats
    ) const;
    
    // ===== 7. Mean Reversion Detection =====
    
    MeanReversionSignalMetrics analyzeMeanReversion(
        const std::string& market,
        const analytics::CoinMetrics& metrics,
        const std::vector<analytics::Candle>& candles,
        double current_price
    );
    
    double calculateReversionProbability(
        const StatisticalMetrics& stats,
        const BollingerBands& bb,
        const MultiPeriodRSI& rsi,
        const VWAPAnalysis& vwap
    ) const;
    
    double estimateReversionTarget(
        double current_price,
        const BollingerBands& bb,
        const VWAPAnalysis& vwap,
        const KalmanFilterState& kalman
    ) const;
    
    double estimateReversionTime(
        double half_life,
        double current_deviation
    ) const;
    
    // ===== 8. Signal Strength =====
    
    double calculateSignalStrength(
        const MeanReversionSignalMetrics& metrics,
        const StatisticalMetrics& stats,
        const analytics::CoinMetrics& coin_metrics
    ) const;
    
    // ===== 9. Position Sizing =====
    
    double calculateKellyFraction(
        double win_rate,
        double avg_win,
        double avg_loss
    ) const;
    
    double adjustForVolatility(
        double kelly_size,
        double volatility
    ) const;
    
    double adjustForConfidence(
        double base_size,
        double confidence
    ) const;
    
    // ===== 10. Risk Management =====
    
    double calculateMeanReversionCVaR(
        double position_size,
        double volatility
    ) const;
    
    bool isWorthTrading(
        const MeanReversionSignalMetrics& signal,
        double expected_return
    ) const;
    
    // ===== 11. Performance Tracking =====
    
    void updateRollingStatistics();
    
    // ===== 12. Helpers =====
    
    double calculateMean(const std::vector<double>& values) const;
    double calculateStdDev(const std::vector<double>& values, double mean) const;
    double calculateVolatility(const std::vector<analytics::Candle>& candles) const;
    
    std::vector<double> extractPrices(
        const std::vector<analytics::Candle>& candles,
        const std::string& type = "close"
    ) const;
    
    long long getCurrentTimestamp() const;
    
    bool shouldGenerateMeanReversionSignal(
        const MeanReversionSignalMetrics& metrics
    ) const;
    
    std::vector<analytics::Candle> parseCandlesFromJson(
        const nlohmann::json& json_data
    ) const;
};

} // namespace strategy
} // namespace autolife
