#pragma once

#include "v2/strategy/IStrategy.h"
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
    BOLLINGER_OVERSOLD,      // BB ?섎떒 怨쇰ℓ??
    RSI_OVERSOLD,            // RSI 怨쇰ℓ??
    Z_SCORE_EXTREME,         // Z-Score 洹밸떒媛?
    KALMAN_DEVIATION,        // Kalman Filter 愿대━
    VWAP_DEVIATION           // VWAP 愿대━
};

// ???낅┰?곸씤 enum (?대쫫 蹂寃?
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
    double hurst_exponent;           // 0.5=?쒕뜡, <0.5=?됯퇏?뚭?, >0.5=異붿꽭
    double half_life;                // ?뚭? 諛섍컧湲?(罹붾뱾 ??
    double adf_statistic;            // ADF 寃???듦퀎??
    bool is_stationary;              // ?뺤긽???щ?
    double autocorrelation;          // ?먭린?곴?
    
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
    double bandwidth;                // (?곷떒-?섎떒) / 以묎컙
    double percent_b;                // ?꾩옱媛 ?꾩튂 (0-1)
    bool squeeze;                    // BB Squeeze ?щ?
    
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
    double rsi_composite;            // 媛以묓룊洹?
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
    double current_deviation_pct;   // ?꾩옱媛 愿대━??
    double deviation_z_score;        // 愿대━??Z-Score
    
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
    double confidence;               // ?듦퀎???좊ː??
    double expected_reversion_pct;   // ?덉긽 ?뚭? ??
    double reversion_probability;    // ?뚭? ?뺣쪧
    double time_to_revert;           // ?덉긽 ?뚭? ?쒓컙 (遺?
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
    double target_mean;              // 紐⑺몴 ?됯퇏媛
    double initial_deviation;        // 吏꾩엯??愿대━??
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
    double avg_reversion_time;       // ?됯퇏 ?뚭? ?뚯슂 ?쒓컙
    int total_reversions_detected;
    int successful_reversions;
    double avg_reversion_accuracy;   // ?덉륫 ?뺥솗??
    
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
    
    // IStrategy 援ы쁽
    StrategyInfo getInfo() const override;
    
    Signal generateSignal(
        const std::string& market,
        const analytics::CoinMetrics& metrics,
        const std::vector<Candle>& candles,
        double current_price,
        double available_capital,
        const analytics::RegimeAnalysis& regime
    ) override;
    
    bool shouldEnter(
        const std::string& market,
        const analytics::CoinMetrics& metrics,
        const std::vector<Candle>& candles,
        double current_price,
        const analytics::RegimeAnalysis& regime
    ) override;
    
    bool shouldExit(
        const std::string& market,
        double entry_price,
        double current_price,
        double holding_time_seconds
    ) override;
    
    double calculateStopLoss(
        double entry_price,
        const std::vector<Candle>& candles
    ) override;
    
    double calculateTakeProfit(
        double entry_price,
        const std::vector<Candle>& candles
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
    void setStatistics(const Statistics& stats) override;
    bool onSignalAccepted(const Signal& signal, double allocated_capital) override;
    
    // === 異붽? 湲곕뒫 ===
    
    double updateTrailingStop(
        double entry_price,
        double highest_price,
        double current_price
    );
    
    bool shouldMoveToBreakeven(
        double entry_price,
        double current_price
    );

private:
    std::shared_ptr<network::UpbitHttpClient> client_;
    bool enabled_;
    Statistics stats_;
    mutable std::recursive_mutex mutex_;
    
    // [?좉퇋 異붽?] 以묐났 吏꾩엯 諛⑹???
    std::set<std::string> active_positions_;

    // ?대젰
    std::deque<double> recent_returns_;
    std::deque<double> recent_holding_times_;
    std::deque<long long> trade_timestamps_;
    std::deque<double> reversion_time_history_;
    
    MeanReversionRollingStatistics rolling_stats_;
    long long last_signal_time_;
    
    // ?ъ???異붿쟻 (?대? 濡쒖쭅??
    std::map<std::string, MeanReversionPositionData> position_data_;
    struct AdaptiveEntryStats {
        int trades = 0;
        int wins = 0;
        double pnl_sum = 0.0;
        double pnl_ema = 0.0;
    };
    std::map<int, AdaptiveEntryStats> adaptive_entry_stats_;
    std::map<std::string, int> pending_entry_keys_;
    std::map<std::string, int> active_entry_keys_;
    static constexpr int ADAPTIVE_ENTRY_MIN_TRADES = 6;
    
    // Kalman Filter States (留덉폆蹂?
    std::map<std::string, KalmanFilterState> kalman_states_;
    
    // ===== API ?몄텧 ?쒖뼱 =====
    
    mutable long long last_orderbook_fetch_time_;
    mutable nlohmann::json cached_orderbook_;
    static constexpr int ORDERBOOK_CACHE_MS = 2000;
    
    mutable std::map<std::string, long long> candle_cache_time_;
    mutable std::map<std::string, std::vector<Candle>> candle_cache_;
    static constexpr int CANDLE_CACHE_MS = 5000;
    
    mutable std::deque<long long> api_call_timestamps_;
    static constexpr int MAX_ORDERBOOK_CALLS_PER_SEC = 8;
    static constexpr int MAX_CANDLE_CALLS_PER_SEC = 8;
    
        std::vector<Candle> resampleTo5m(
        const std::vector<Candle>& candles_1m
    ) const;
    
    // ===== 嫄곕옒 鍮덈룄 ?쒖뼱 =====
    
    int daily_trades_count_;
    int hourly_trades_count_;
    long long current_day_start_;
    long long current_hour_start_;
    mutable long long latest_market_timestamp_ms_ = 0;
    
    static constexpr int MAX_DAILY_REVERSION_TRADES = 12;
    static constexpr int MAX_HOURLY_REVERSION_TRADES = 4;
    
    // ===== ?쒗궥 釉뚮젅?댁빱 =====
    
    int consecutive_losses_;
    bool circuit_breaker_active_;
    long long circuit_breaker_until_;
    
    static constexpr int MAX_CONSECUTIVE_LOSSES = 4;
    static constexpr long long CIRCUIT_BREAKER_COOLDOWN_MS = 7200000;  // 2?쒓컙
    
    // ===== ?뚮씪誘명꽣 =====
    
    static constexpr double UPBIT_FEE_RATE = 0.0005;
    static constexpr double EXPECTED_SLIPPAGE = 0.0003;
    static constexpr double MIN_ORDER_AMOUNT_KRW = 5000.0;
    
    // Z-Score ?꾧퀎媛?
    static constexpr double Z_SCORE_EXTREME = -2.0;      // -2 ?댄븯 = 怨쇰ℓ??
    static constexpr double Z_SCORE_EXIT = -0.5;         // -0.5 ?뚮났??泥?궛
    
    // RSI ?꾧퀎媛?
    static constexpr double RSI_OVERSOLD = 30.0;
    static constexpr double RSI_EXIT = 50.0;
    
    // Bollinger Bands
    static constexpr double BB_PERIOD = 20;
    static constexpr double BB_STD_DEV = 2.0;
    static constexpr double BB_SQUEEZE_THRESHOLD = 0.05;  // 5% ?댄븯
    
    // Hurst Exponent (?됯퇏?뚭? ?먮퀎)
    static constexpr double HURST_MEAN_REVERTING = 0.45;  // < 0.45 = 媛뺥븳 ?됯퇏?뚭?
    
    // ?먯젅/?듭젅
    static constexpr double BASE_STOP_LOSS = 0.025;       // 2.5%
    static constexpr double BASE_TAKE_PROFIT_1 = 0.02;    // 2%
    static constexpr double BASE_TAKE_PROFIT_2 = 0.04;    // 4%
    static constexpr double TRAILING_ACTIVATION = 0.025;  // 2.5%
    static constexpr double TRAILING_DISTANCE = 0.015;    // 1.5%
    
    static constexpr double MAX_HOLDING_TIME_MINUTES = 240.0;  // 4?쒓컙
    static constexpr double MIN_LIQUIDITY_SCORE = 50.0;
    static constexpr double MIN_SIGNAL_STRENGTH = 0.65;
    static constexpr double MIN_REVERSION_PROBABILITY = 0.70;  // 70% ?댁긽
    static constexpr double MAX_POSITION_SIZE = 0.15;          // 15%
    static constexpr double BREAKEVEN_TRIGGER = 0.015;         // 1.5%
    static constexpr int MIN_SIGNAL_INTERVAL_SEC = 600;        // 10遺?
    
    // ===== API ?몄텧 愿由?=====
    
    bool canMakeOrderBookAPICall() const;
    bool canMakeCandleAPICall() const;
    void recordAPICall() const;
    
    nlohmann::json getCachedOrderBook(const std::string& market);
    std::vector<Candle> getCachedCandles(const std::string& market, int count);
    
    // ===== 嫄곕옒 鍮덈룄 愿由?=====
    
    bool canTradeNow();
    void recordTrade();
    void resetDailyCounters();
    void resetHourlyCounters();
    
    // ===== ?쒗궥 釉뚮젅?댁빱 =====
    
    void checkCircuitBreaker();
    void activateCircuitBreaker();
    bool isCircuitBreakerActive() const;
    double getAdaptiveEntryBias(int entry_key) const;
    void recordAdaptiveEntryOutcome(int entry_key, bool is_win, double profit_loss);
    void loadAdaptiveEntryStats();
    void saveAdaptiveEntryStats() const;
    
    // ===== 1. Statistical Analysis =====
    
    StatisticalMetrics calculateStatisticalMetrics(
        const std::vector<Candle>& candles
    ) const;
    
    double calculateZScore(
        double value,
        const std::vector<double>& history
    ) const;
    
    double calculateHurstExponent(
        const std::vector<Candle>& candles
    ) const;
    
    double calculateHalfLife(
        const std::vector<Candle>& candles
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
        const std::vector<Candle>& candles,
        int period,
        double std_dev_mult
    ) const;
    
    bool isBBSqueeze(const BollingerBands& bb) const;
    
    // ===== 4. RSI Analysis =====
    
    MultiPeriodRSI calculateMultiPeriodRSI(
        const std::vector<Candle>& candles
    ) const;
    
    double calculateRSI(
        const std::vector<Candle>& candles,
        int period
    ) const;
    
    // ===== 5. VWAP Analysis =====
    
    VWAPAnalysis calculateVWAPAnalysis(
        const std::vector<Candle>& candles,
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
        const std::vector<Candle>& candles,
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
    double calculateVolatility(const std::vector<Candle>& candles) const;
    
    std::vector<double> extractPrices(
        const std::vector<Candle>& candles,
        const std::string& type = "close"
    ) const;
    
    long long getCurrentTimestamp() const;
    
    bool shouldGenerateMeanReversionSignal(
        const MeanReversionSignalMetrics& metrics
    ) const;
    
    std::vector<Candle> parseCandlesFromJson(
        const nlohmann::json& json_data
    ) const;
};

} // namespace strategy
} // namespace autolife

