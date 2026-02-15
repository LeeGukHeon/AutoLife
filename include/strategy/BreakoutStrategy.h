#pragma once

#include "strategy/IStrategy.h"
#include "network/UpbitHttpClient.h"
#include <memory>
#include <mutex>
#include <deque>
#include <array>
#include <map>
#include <set> // [필수] 중복 방지용

namespace autolife {
namespace strategy {

// ===== Breakout Types =====

enum class BreakoutType {
    NONE,
    DONCHIAN_BREAK,       // 도치안 채널 돌파
    RESISTANCE_BREAK,     // 저항선 돌파
    CONSOLIDATION_BREAK,  // 횡보 이탈
    VOLUME_BREAKOUT       // 거래량 급증 돌파
};

// ===== Donchian Channel =====

struct DonchianChannel {
    double upper;
    double lower;
    double middle;
    double width_percentile;
    
    DonchianChannel()
        : upper(0), lower(0), middle(0), width_percentile(0)
    {}
};

// ===== Support/Resistance =====

struct SupportResistanceLevels {
    double pivot_point;
    double r1, r2, r3;
    double s1, s2, s3;
    std::vector<double> fibonacci_levels;
    
    SupportResistanceLevels()
        : pivot_point(0), r1(0), r2(0), r3(0)
        , s1(0), s2(0), s3(0)
    {}
};

// ===== Volume Profile =====

struct VolumeProfileData {
    double poc;                    // Point of Control
    double value_area_high;
    double value_area_low;
    double volume_at_price_score;
    
    VolumeProfileData()
        : poc(0), value_area_high(0), value_area_low(0)
        , volume_at_price_score(0)
    {}
};

// ===== Market Structure =====

struct MarketStructureAnalysis {
    bool uptrend;
    bool downtrend;
    bool ranging;
    double swing_strength;
    int consolidation_bars;
    double consolidation_range_pct;
    
    MarketStructureAnalysis()
        : uptrend(false), downtrend(false), ranging(false)
        , swing_strength(0), consolidation_bars(0)
        , consolidation_range_pct(0)
    {}
};

// ===== Breakout Signal =====

struct BreakoutSignalMetrics {
    BreakoutType type;
    double strength;                    // 0-1
    double volume_confirmation;         // 0-1
    double false_breakout_probability;  // 0-1
    double atr_multiple;
    bool is_valid;
    
    BreakoutSignalMetrics()
        : type(BreakoutType::NONE)
        , strength(0), volume_confirmation(0)
        , false_breakout_probability(1.0)
        , atr_multiple(0), is_valid(false)
    {}
};

// ===== Position Tracking =====

struct BreakoutPositionData {
    std::string market;
    double entry_price;
    double highest_price;
    double trailing_stop;
    long long entry_timestamp;
    bool tp1_hit;
    bool tp2_hit;
    
    BreakoutPositionData()
        : entry_price(0), highest_price(0), trailing_stop(0)
        , entry_timestamp(0), tp1_hit(false), tp2_hit(false)
    {}
};

// ===== Rolling Statistics =====

struct BreakoutRollingStatistics {
    double rolling_win_rate;
    double avg_holding_time_minutes;
    double rolling_profit_factor;
    int total_breakouts_detected;
    int successful_breakouts;
    
    BreakoutRollingStatistics()
        : rolling_win_rate(0), avg_holding_time_minutes(0)
        , rolling_profit_factor(0), total_breakouts_detected(0)
        , successful_breakouts(0)
    {}
};

// ===== Breakout Strategy =====

class BreakoutStrategy : public IStrategy {
public:
    BreakoutStrategy(std::shared_ptr<network::UpbitHttpClient> client);
    
    // IStrategy 구현
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
    
    BreakoutRollingStatistics getRollingStatistics() const;

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
    
    BreakoutRollingStatistics rolling_stats_;
    long long last_signal_time_;
    
    std::vector<Candle> resampleTo5m(
    const std::vector<Candle>& candles_1m
    ) const;

    // 포지션 추적 (이건 내부 로직용이고, 중복 방지는 active_positions_가 담당)
    std::map<std::string, BreakoutPositionData> position_data_; // active_positions_와 이름 겹치지 않게 주의
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
    
    // ===== API 호출 제어 =====
    
    mutable long long last_orderbook_fetch_time_;
    mutable nlohmann::json cached_orderbook_;
    static constexpr int ORDERBOOK_CACHE_MS = 2000;
    
    mutable std::map<std::string, long long> candle_cache_time_;
    mutable std::map<std::string, std::vector<Candle>> candle_cache_;
    static constexpr int CANDLE_CACHE_MS = 5000;
    
    mutable std::deque<long long> api_call_timestamps_;
    static constexpr int MAX_ORDERBOOK_CALLS_PER_SEC = 8;
    static constexpr int MAX_CANDLE_CALLS_PER_SEC = 8;
    
    // ===== 거래 빈도 제어 =====
    
    int daily_trades_count_;
    int hourly_trades_count_;
    long long current_day_start_;
    long long current_hour_start_;
    mutable long long latest_market_timestamp_ms_ = 0;
    
    static constexpr int MAX_DAILY_BREAKOUT_TRADES = 10;
    static constexpr int MAX_HOURLY_BREAKOUT_TRADES = 3;
    
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
    
    static constexpr int DONCHIAN_PERIOD = 20;
    static constexpr double MIN_ATR_MULTIPLE = 1.5;
    static constexpr double BASE_STOP_LOSS = 0.018;         // 1.8%
    static constexpr double BASE_TAKE_PROFIT_1 = 0.035;     // 3.5%
    static constexpr double BASE_TAKE_PROFIT_2 = 0.06;      // 6%
    static constexpr double TRAILING_ACTIVATION = 0.04;     // 4%
    static constexpr double TRAILING_DISTANCE = 0.02;       // 2%
    static constexpr double MAX_HOLDING_TIME_MINUTES = 300.0;  // 5시간
    static constexpr double MIN_LIQUIDITY_SCORE = 50.0;
    static constexpr double MIN_SIGNAL_STRENGTH = 0.65;
    static constexpr double FALSE_BREAKOUT_THRESHOLD = 0.25;
    static constexpr double MAX_POSITION_SIZE = 0.20;       // 20%
    static constexpr double BREAKEVEN_TRIGGER = 0.025;      // 2.5%
    static constexpr int MIN_SIGNAL_INTERVAL_SEC = 720;     // 12분
    static constexpr int MIN_CONSOLIDATION_BARS = 15;
    
    // ===== API 호출 관리 =====
    
    bool canMakeOrderBookAPICall() const;
    bool canMakeCandleAPICall() const;
    void recordAPICall() const;
    
    nlohmann::json getCachedOrderBook(const std::string& market);
    
    std::vector<Candle> getCachedCandles(
        const std::string& market,
        int count
    );
    
    // ===== 거래 빈도 관리 =====
    
    bool canTradeNow();
    void recordTrade();
    void resetDailyCounters();
    void resetHourlyCounters();
    
    // ===== 서킷 브레이커 =====
    
    void checkCircuitBreaker();
    void activateCircuitBreaker();
    bool isCircuitBreakerActive() const;
    double getAdaptiveEntryBias(int entry_key) const;
    void recordAdaptiveEntryOutcome(int entry_key, bool is_win, double profit_loss);
    void loadAdaptiveEntryStats();
    void saveAdaptiveEntryStats() const;
    
    // ===== 1. Donchian Channel =====
    
    DonchianChannel calculateDonchianChannel(
        const std::vector<Candle>& candles,
        int period
    ) const;
    
    // ===== 2. Support/Resistance =====
    
    SupportResistanceLevels calculateSupportResistance(
        const std::vector<Candle>& candles
    ) const;
    
    std::vector<double> findSwingHighs(
        const std::vector<Candle>& candles
    ) const;
    
    std::vector<double> findSwingLows(
        const std::vector<Candle>& candles
    ) const;
    
    // ===== 3. Volume Profile =====
    
    VolumeProfileData calculateVolumeProfile(
        const std::vector<Candle>& candles
    ) const;
    
    // ===== 4. Market Structure =====
    
    MarketStructureAnalysis analyzeMarketStructure(
        const std::vector<Candle>& candles
    ) const;
    
    bool isConsolidating(
        const std::vector<Candle>& candles,
        double& range_pct
    ) const;
    
    // ===== 5. Breakout Detection =====
    
    BreakoutSignalMetrics analyzeBreakout(
        const std::string& market,
        const analytics::CoinMetrics& metrics,
        const std::vector<Candle>& candles,
        double current_price
    );
    
    bool isFalseBreakout(
        double current_price,
        double breakout_level,
        const std::vector<Candle>& candles
    ) const;
    
    double calculateBreakoutStrength(
        double current_price,
        double breakout_level,
        const DonchianChannel& channel,
        const VolumeProfileData& volume_profile
    ) const;
    
    // ===== 6. Volume Analysis =====
    
    bool isVolumeSpikeSignificant(
        const analytics::CoinMetrics& metrics,
        const std::vector<Candle>& candles
    ) const;
    
    double calculateVolumeConfirmation(
        const std::vector<Candle>& candles
    ) const;
    
    // ===== 7. ATR Calculation =====
    
    double calculateATR(
        const std::vector<Candle>& candles,
        int period
    ) const;
    
    // ===== 8. Order Flow =====
    
    double analyzeOrderFlowImbalance(
        const analytics::CoinMetrics& metrics
    );
    
    // ===== 9. Signal Strength =====
    
    double calculateSignalStrength(
        const BreakoutSignalMetrics& metrics,
        const MarketStructureAnalysis& structure,
        const analytics::CoinMetrics& coin_metrics
    ) const;
    
    // ===== 10. Risk Management =====
    
    double calculateBreakoutCVaR(
        double position_size,
        double volatility
    ) const;
    
    // ===== 11. Performance Tracking =====
    
    void updateRollingStatistics();
    
    // ===== 12. Helpers =====
    
      double calculateKellyFraction(
        double win_rate,
        double avg_win,
        double avg_loss
    ) const;
    
    double adjustForVolatility(
        double kelly_size,
        double volatility
    ) const;
    
    double calculateMean(const std::vector<double>& values) const;
    double calculateStdDev(const std::vector<double>& values, double mean) const;
    double calculateVolatility(const std::vector<Candle>& candles) const;
    
    long long getCurrentTimestamp() const;
    
    bool shouldGenerateBreakoutSignal(
        const BreakoutSignalMetrics& metrics
    ) const;
    
    std::vector<Candle> parseCandlesFromJson(
        const nlohmann::json& json_data
    ) const;
};

} // namespace strategy
} // namespace autolife
