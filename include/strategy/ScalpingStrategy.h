#pragma once

#include "strategy/IStrategy.h"
#include "network/UpbitHttpClient.h"
#include <memory>
#include <mutex>
#include <deque>
#include <array>
#include <map>
#include <set>
#include "engine/TradingEngine.h"

namespace autolife {
namespace strategy {

// ===== Market Microstate (초단타용) =====

enum class MarketMicrostate {
    OVERSOLD_BOUNCE,     // 과매도 반등
    MOMENTUM_SPIKE,      // 순간 급등
    BREAKOUT,            // 짧은 돌파
    CONSOLIDATION,       // 횡보
    DECLINE              // 하락
};

// HMM (Scalping용)
struct MicrostateModel {
    std::array<std::array<double, 5>, 5> transition_prob;
    std::array<double, 5> current_prob;
    
    MicrostateModel() {
        for (int i = 0; i < 5; ++i) {
            for (int j = 0; j < 5; ++j) {
                transition_prob[i][j] = (i == j) ? 0.6 : 0.1;
            }
            current_prob[i] = 0.2;
        }
    }
};

// ===== Ultra-Fast Order Flow =====

struct UltraFastOrderFlowMetrics {
    double bid_ask_spread;
    double instant_pressure;
    double order_flow_delta;
    double tape_reading_score;
    double micro_imbalance;
    double momentum_acceleration;
    double microstructure_score;
    
    UltraFastOrderFlowMetrics()
        : bid_ask_spread(0), instant_pressure(0), order_flow_delta(0)
        , tape_reading_score(0), micro_imbalance(0), momentum_acceleration(0)
        , microstructure_score(0)
    {}
};

// ===== Multi-Timeframe Signal (1m, 3m) =====

struct ScalpingMultiTimeframeSignal {
    bool tf_1m_oversold;
    bool tf_3m_oversold;
    double alignment_score;
    
    struct ScalpingTimeframeMetrics {
        double rsi;
        double stoch_rsi;
        double instant_momentum;
    };
    
    ScalpingTimeframeMetrics tf_1m;
    ScalpingTimeframeMetrics tf_3m;
    
    ScalpingMultiTimeframeSignal()
        : tf_1m_oversold(false), tf_3m_oversold(false), alignment_score(0)
    {}
};

// ===== Position Metrics =====

struct ScalpingPositionMetrics {
    double kelly_fraction;
    double half_kelly;
    double volatility_adjusted;
    double final_position_size;
    double expected_sharpe;
    double max_loss_amount;
    
    ScalpingPositionMetrics()
        : kelly_fraction(0), half_kelly(0), volatility_adjusted(0)
        , final_position_size(0), expected_sharpe(0), max_loss_amount(0)
    {}
};

// ===== Dynamic Stops =====

struct ScalpingDynamicStops {
    double stop_loss;
    double take_profit_1;
    double take_profit_2;
    double breakeven_trigger;
    double trailing_start;
    
    ScalpingDynamicStops()
        : stop_loss(0), take_profit_1(0), take_profit_2(0)
        , breakeven_trigger(0), trailing_start(0)
    {}
};

// ===== Rolling Statistics =====

struct ScalpingRollingStatistics {
    double rolling_sharpe_1h;
    double rolling_sharpe_24h;
    double rolling_sortino_1h;
    double rolling_win_rate_50;
    double rolling_profit_factor;
    double avg_holding_time_seconds;
    
    ScalpingRollingStatistics()
        : rolling_sharpe_1h(0), rolling_sharpe_24h(0), rolling_sortino_1h(0)
        , rolling_win_rate_50(0), rolling_profit_factor(0)
        , avg_holding_time_seconds(0)
    {}
};

// ===== Advanced Scalping Strategy =====

class ScalpingStrategy : public IStrategy {
public:
    ScalpingStrategy(std::shared_ptr<network::UpbitHttpClient> client);
    
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
    
    ScalpingRollingStatistics getRollingStatistics() const;
    
private:
    std::shared_ptr<network::UpbitHttpClient> client_;
    bool enabled_;
    Statistics stats_;
    mutable std::recursive_mutex mutex_;
    
    // 이력 (500개)
    std::deque<double> recent_returns_;
    std::deque<double> recent_holding_times_;
    std::deque<long long> trade_timestamps_;
    
    ScalpingRollingStatistics rolling_stats_;
    MicrostateModel microstate_model_;
    long long last_signal_time_;
    
    // [신규 추가] 중복 진입 방지를 위한 활성 포지션 목록
    std::set<std::string> active_positions_;

    // ===== 업비트 API 호출 제어 =====
    
    mutable long long last_orderbook_fetch_time_;
    mutable nlohmann::json cached_orderbook_;
    static constexpr int ORDERBOOK_CACHE_MS = 500;  // 0.5초 캐시
    
    mutable std::map<std::string, long long> candle_cache_time_;
    mutable std::map<std::string, std::vector<analytics::Candle>> candle_cache_;
    static constexpr int CANDLE_CACHE_MS = 2000;    // 2초 캐시
    
    mutable std::deque<long long> api_call_timestamps_;
    static constexpr int MAX_ORDERBOOK_CALLS_PER_SEC = 8;   // 초당 8회 (안전 마진 20%)
    static constexpr int MAX_CANDLE_CALLS_PER_SEC = 8;      // 초당 8회
    
    // ===== 거래 빈도 제어 =====
    
    int daily_trades_count_;
    int hourly_trades_count_;
    long long current_day_start_;
    long long current_hour_start_;
    
    static constexpr int MAX_DAILY_SCALPING_TRADES = 15;
    static constexpr int MAX_HOURLY_SCALPING_TRADES = 5;
    
    // ===== 연속 손실 제어 (서킷 브레이커) =====
    
    int consecutive_losses_;
    bool circuit_breaker_active_;
    long long circuit_breaker_until_;
    
    static constexpr int MAX_CONSECUTIVE_LOSSES = 5;
    static constexpr long long CIRCUIT_BREAKER_COOLDOWN_MS = 3600000;  // 1시간
    
    // ===== 파라미터 (업비트 기준) =====
    
    static constexpr double UPBIT_FEE_RATE = 0.0005;        // 0.05% (공식)
    static constexpr double EXPECTED_SLIPPAGE = 0.0003;     // 0.03%
    static constexpr double MIN_ORDER_AMOUNT_KRW = 5000.0;  // 5,000원 (공식)
    
    static constexpr double BASE_TAKE_PROFIT = 0.02;        // 2%
    static constexpr double BASE_STOP_LOSS = 0.01;          // 1%
    static constexpr double MAX_HOLDING_TIME = 300.0;       // 5분
    static constexpr double CONFIDENCE_LEVEL = 0.95;
    static constexpr double MIN_SHARPE_RATIO = 0.8;
    static constexpr double MAX_POSITION_SIZE = 0.20;       // 5%
    static constexpr double HALF_KELLY_FRACTION = 0.5;
    static constexpr double MIN_LIQUIDITY = 60.0;
    static constexpr int MIN_SIGNAL_INTERVAL_SEC = 120;     // 2분
    static constexpr double MIN_RISK_REWARD_RATIO = 1.8;
    static constexpr double MIN_EXPECTED_SHARPE = 1.0;
    static constexpr double BREAKEVEN_TRIGGER = 0.01;       // 1%
    
    // ===== API 호출 관리 =====
    
    bool canMakeOrderBookAPICall() const;
    bool canMakeCandleAPICall() const;
    void recordAPICall() const;
    
    nlohmann::json getCachedOrderBook(const std::string& market);
    
    std::vector<analytics::Candle> getCachedCandles(
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
    
    // ===== 1. Market Microstate Detection =====
    
    MarketMicrostate detectMarketMicrostate(
        const std::vector<analytics::Candle>& candles
    );
    
    void updateMicrostateModel(
        const std::vector<analytics::Candle>& candles,
        MicrostateModel& model
    );
    
    // ===== 2. Statistical Significance =====
    
    bool isVolumeSpikeSignificant(
        const analytics::CoinMetrics& metrics,
        const std::vector<analytics::Candle>& candles
    ) const;
    
    double calculateZScore(
        double value,
        const std::vector<double>& history
    ) const;
    
    bool isTTestSignificant(
        const std::vector<double>& sample1,
        const std::vector<double>& sample2,
        double alpha = 0.05
    ) const;
    
    // ===== 3. Multi-Timeframe (1m, 3m) =====
    
    ScalpingMultiTimeframeSignal analyzeScalpingTimeframes(
        const std::vector<analytics::Candle>& candles_1m
    ) const;
    
    std::vector<analytics::Candle> resampleTo3m(
        const std::vector<analytics::Candle>& candles_1m
    ) const;
    
    bool isOversoldOnTimeframe(
        const std::vector<analytics::Candle>& candles,
        ScalpingMultiTimeframeSignal::ScalpingTimeframeMetrics& metrics
    ) const;
    
    // ===== 4. Ultra-Fast Order Flow =====
    
    UltraFastOrderFlowMetrics analyzeUltraFastOrderFlow(
        const std::string& market,
        double current_price
    );
    
    double calculateTapeReadingScore(
        const nlohmann::json& orderbook
    ) const;
    
    double calculateMomentumAcceleration(
        const std::vector<analytics::Candle>& candles
    ) const;
    
    // ===== 5. Position Sizing =====
    
    ScalpingPositionMetrics calculateScalpingPositionSize(
        double capital,
        double entry_price,
        double stop_loss,
        const analytics::CoinMetrics& metrics,
        const std::vector<analytics::Candle>& candles
    ) const;
    
    double calculateKellyFraction(
        double win_rate,
        double avg_win,
        double avg_loss
    ) const;
    
    double adjustForUltraShortVolatility(
        double kelly_size,
        double volatility
    ) const;
    
    // ===== 6. Dynamic Stops =====
    
    ScalpingDynamicStops calculateScalpingDynamicStops(
        double entry_price,
        const std::vector<analytics::Candle>& candles
    ) const;
    
    double calculateMicroATRBasedStop(
        double entry_price,
        const std::vector<analytics::Candle>& candles
    ) const;
    
    // ===== 7. Trade Cost Analysis =====
    
    bool isWorthScalping(
        double expected_return,
        double expected_sharpe
    ) const;
    
    // ===== 8. Signal Strength =====
    
    double calculateScalpingSignalStrength(
        const analytics::CoinMetrics& metrics,
        const std::vector<analytics::Candle>& candles,
        const ScalpingMultiTimeframeSignal& mtf_signal,
        const UltraFastOrderFlowMetrics& order_flow,
        MarketMicrostate microstate
    ) const;
    
    // ===== 9. Risk Management =====
    
    double calculateScalpingCVaR(
        double position_size,
        double volatility
    ) const;
    
    // ===== 10. Performance Metrics =====
    
    double calculateScalpingSharpeRatio(
        int periods_per_year = 105120
    ) const;
    
    double calculateScalpingSortinoRatio(
        int periods_per_year = 105120
    ) const;
    
    void updateScalpingRollingStatistics();
    
    // ===== 11. Helpers =====
    
    double calculateMean(const std::vector<double>& values) const;
    double calculateStdDev(const std::vector<double>& values, double mean) const;
    double calculateUltraShortVolatility(const std::vector<analytics::Candle>& candles) const;
    
    long long getCurrentTimestamp() const;
    engine::EngineConfig engine_config_;
    bool shouldGenerateScalpingSignal(
        double expected_return,
        double expected_sharpe
    ) const;
};

} // namespace strategy
} // namespace autolife
