#pragma once

#include "v2/strategy/IStrategy.h"
#include "network/UpbitHttpClient.h"
#include "engine/EngineConfig.h"
#include <memory>
#include <mutex>
#include <deque>
#include <array>
#include <map>
#include <set>

namespace autolife {
namespace strategy {

// ===== Market Regime (HMM 湲곕컲) =====

enum class MarketRegime {
    STRONG_UPTREND,      // 媛뺥븳 ?곸듅
    WEAK_UPTREND,        // ?쏀븳 ?곸듅
    SIDEWAYS,            // ?〓낫
    WEAK_DOWNTREND,      // ?쏀븳 ?섎씫
    STRONG_DOWNTREND     // 媛뺥븳 ?섎씫
};

// Hidden Markov Model
struct RegimeModel {
    std::array<std::array<double, 5>, 5> transition_prob;  // ?곹깭 ?꾩씠 ?뺣쪧
    std::array<double, 5> current_prob;                     // ?꾩옱 ?곹깭 ?뺣쪧
    
    RegimeModel() {
        // 湲곕낯 ?꾩씠 ?뺣쪧 珥덇린??(?媛곸꽑 ?곗꽭 = ?곹깭 ?좎? 寃쏀뼢)
        for (int i = 0; i < 5; ++i) {
            for (int j = 0; j < 5; ++j) {
                transition_prob[i][j] = (i == j) ? 0.7 : 0.075;
            }
            current_prob[i] = 0.2;  // 洹좊벑 遺꾪룷濡??쒖옉
        }
    }
};

// ===== Advanced Order Flow Metrics =====

struct AdvancedOrderFlowMetrics {
    double bid_ask_spread;              // ?멸? ?ㅽ봽?덈뱶 (%)
    double order_book_pressure;         // ?멸? ?뺣젰 (-1 ~ +1)
    double large_order_imbalance;       // ??二쇰Ц 遺덇퇏??
    double vwap_deviation;              // VWAP ?鍮??꾩옱媛 愿대━??(%)
    double order_book_depth_ratio;      // ?멸?李?源딆씠 鍮꾩쑉
    double cumulative_delta;            // ?꾩쟻 ?명? (留ㅼ닔-留ㅻ룄 ?꾩쟻)
    double microstructure_score;        // 醫낇빀 誘몄꽭援ъ“ ?먯닔 (0-1)
    
    // Volume Profile
    struct VolumeProfile {
        double point_of_control;        // POC (理쒕? 嫄곕옒??媛寃?
        double value_area_high;         // Value Area ?곷떒
        double value_area_low;          // Value Area ?섎떒
    } volume_profile;
    
    AdvancedOrderFlowMetrics()
        : bid_ask_spread(0), order_book_pressure(0), large_order_imbalance(0)
        , vwap_deviation(0), order_book_depth_ratio(0), cumulative_delta(0)
        , microstructure_score(0)
    {
        volume_profile.point_of_control = 0;
        volume_profile.value_area_high = 0;
        volume_profile.value_area_low = 0;
    }
};

// ===== Multi-Timeframe Signal =====

struct MultiTimeframeSignal {
    bool tf_1m_bullish;                 // 1遺꾨큺 ?곸듅
    bool tf_5m_bullish;                 // 5遺꾨큺 ?곸듅
    bool tf_15m_bullish;                // 15遺꾨큺 ?곸듅
    double alignment_score;             // ?쒓컙?꾨젅???뺣젹 ?먯닔 (0-1)
    
    struct TimeframeMetrics {
        double rsi;
        double macd_histogram;
        double trend_strength;
    };
    
    TimeframeMetrics tf_1m;
    TimeframeMetrics tf_5m;
    TimeframeMetrics tf_15m;
    
    MultiTimeframeSignal()
        : tf_1m_bullish(false), tf_5m_bullish(false)
        , tf_15m_bullish(false), alignment_score(0)
    {}
};

// ===== Position Metrics (Kelly + Volatility) =====

struct PositionMetrics {
    double kelly_fraction;              // Full Kelly 鍮꾩쑉
    double half_kelly;                  // Half Kelly (?ㅼ젣 ?ъ슜)
    double volatility_adjusted;         // 蹂?숈꽦 議곗젙 ??
    double final_position_size;         // 理쒖쥌 ?ъ????ш린
    double expected_sharpe;             // ?덉긽 Sharpe Ratio
    double max_loss_amount;             // 理쒕? ?먯떎 湲덉븸
    
    PositionMetrics()
        : kelly_fraction(0), half_kelly(0), volatility_adjusted(0)
        , final_position_size(0), expected_sharpe(0), max_loss_amount(0)
    {}
};

// ===== Dynamic Stops =====

struct DynamicStops {
    double stop_loss;                   // ?먯젅媛
    double take_profit_1;               // 1李??듭젅 (50%)
    double take_profit_2;               // 2李??듭젅 (100%)
    double trailing_start;              // Trailing ?쒖옉媛
    double chandelier_exit;             // Chandelier Exit
    double parabolic_sar;               // Parabolic SAR
    
    DynamicStops()
        : stop_loss(0), take_profit_1(0), take_profit_2(0)
        , trailing_start(0), chandelier_exit(0), parabolic_sar(0)
    {}
};

// ===== Rolling Statistics =====

struct RollingStatistics {
    double rolling_sharpe_30d;          // 30??Sharpe
    double rolling_sharpe_90d;          // 90??Sharpe
    double rolling_sortino_30d;         // 30??Sortino
    double rolling_calmar;              // Calmar Ratio
    double rolling_max_dd_30d;          // 30??理쒕? ?숉룺
    double rolling_win_rate_100;        // 理쒓렐 100嫄곕옒 ?밸쪧
    double rolling_profit_factor;       // Profit Factor
    
    RollingStatistics()
        : rolling_sharpe_30d(0), rolling_sharpe_90d(0), rolling_sortino_30d(0)
        , rolling_calmar(0), rolling_max_dd_30d(0), rolling_win_rate_100(0)
        , rolling_profit_factor(0)
    {}
};

// ===== Walk-Forward Result =====

struct WalkForwardResult {
    double in_sample_sharpe;            // In-Sample Sharpe
    double out_sample_sharpe;           // Out-of-Sample Sharpe
    double degradation_ratio;           // ?깅뒫 ???鍮꾩쑉
    bool is_robust;                     // 寃ш퀬???щ?
    
    WalkForwardResult()
        : in_sample_sharpe(0), out_sample_sharpe(0)
        , degradation_ratio(0), is_robust(false)
    {}
};

// ===== Advanced Momentum Strategy =====

class MomentumStrategy : public IStrategy {
public:
    MomentumStrategy(std::shared_ptr<network::UpbitHttpClient> client);
    
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
    
    // === 異붽? 怨듦컻 硫붿꽌??===
    
    // Trailing Stop ?낅뜲?댄듃
    double updateTrailingStop(
        double entry_price,
        double highest_price,
        double current_price,
        const std::vector<Candle>& recent_candles
    );
    
private:
        // ?ъ????곹깭 ?낅뜲?댄듃
        void updateState(const std::string& market, double current_price) override;

    std::shared_ptr<network::UpbitHttpClient> client_;
    bool enabled_;
    Statistics stats_;
    mutable std::recursive_mutex mutex_;
    
    // [?좉퇋 異붽?] 以묐났 留ㅼ닔 諛⑹???
    std::set<std::string> active_positions_;
    struct EntryDecisionContext {
        double setup_score = 0.0;
        double trigger_score = 0.0;
        double signal_strength = 0.0;
        double mtf_alignment = 0.0;
        double flow_bias = 0.0;
        int archetype = 0;
        double invalidation_drawdown_pct = 0.0;
        double progress_floor_30m = 0.0;
        double progress_floor_60m = 0.0;
        analytics::MarketRegime regime = analytics::MarketRegime::UNKNOWN;
        long long accepted_timestamp_ms = 0;
    };
    std::map<std::string, EntryDecisionContext> pending_entry_contexts_;
    std::map<std::string, EntryDecisionContext> active_entry_contexts_;
    struct ArchetypeAdaptiveStats {
        int trades = 0;
        int wins = 0;
        double pnl_sum = 0.0;
        double pnl_ema = 0.0;
    };
    std::map<int, ArchetypeAdaptiveStats> archetype_adaptive_stats_;
    static constexpr int ADAPTIVE_ARCHETYPE_MIN_TRADES = 6;
    
    // ?대젰 ?곗씠??(1000媛?
    std::deque<double> recent_returns_;
    std::deque<double> recent_volatility_;
    std::deque<long long> trade_timestamps_;
    
    // 濡ㅻ쭅 ?듦퀎
    RollingStatistics rolling_stats_;
    
    // Regime Model
    RegimeModel regime_model_;
    
    // 留덉?留??좏샇 ?쒓컙 (怨쇰ℓ留?諛⑹?)
    long long last_signal_time_;
    
    // ===== ?뚮씪誘명꽣 =====
    
    static constexpr double BASE_TAKE_PROFIT = 0.05;       // 5%
    static constexpr double BASE_STOP_LOSS = 0.02;         // 2%
    static constexpr double MAX_HOLDING_TIME = 7200.0;     // 2?쒓컙
    static constexpr double FEE_RATE = 0.0005;             // 0.05%
    static constexpr double EXPECTED_SLIPPAGE = 0.0002;    // 0.02%
    static constexpr double CONFIDENCE_LEVEL = 0.95;       // 95%
    static constexpr double MIN_SHARPE_RATIO = 1.0;
    static constexpr double MAX_POSITION_SIZE = 0.10;      // 10%
    static constexpr double HALF_KELLY_FRACTION = 0.5;
    static constexpr double MIN_LIQUIDITY = 50.0;
    static constexpr int MIN_SIGNAL_INTERVAL_SEC = 300;    // 5遺?
    static constexpr double MIN_RISK_REWARD_RATIO = 2.5;
    static constexpr double MIN_EXPECTED_SHARPE = 1.5;
    
    // ===== 1. Market Regime (HMM) =====
    
    MarketRegime detectMarketRegime(
        const std::vector<Candle>& candles
    );
    
    void updateRegimeModel(
        const std::vector<Candle>& candles,
        RegimeModel& model
    );
    
    // ===== 2. Statistical Significance =====
    
    bool isVolumeSurgeSignificant(
        const analytics::CoinMetrics& metrics,
        const std::vector<Candle>& candles
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
    
    bool isKSTestPassed(
        const std::vector<double>& sample
    ) const;
    
    // ===== 3. Multi-Timeframe Analysis =====
    
    MultiTimeframeSignal analyzeMultiTimeframe(
        const analytics::CoinMetrics& metrics,
        const std::vector<Candle>& candles_1m
    ) const;
    
    std::vector<Candle> resampleTo5m(
        const std::vector<Candle>& candles_1m
    ) const;
    
    std::vector<Candle> resampleTo15m(
        const std::vector<Candle>& candles_1m
    ) const;
    
    bool isBullishOnTimeframe(
        const std::vector<Candle>& candles,
        MultiTimeframeSignal::TimeframeMetrics& metrics
    ) const;
    
    // ===== 4. Advanced Order Flow =====
    
    AdvancedOrderFlowMetrics analyzeAdvancedOrderFlow(
        const analytics::CoinMetrics& metrics,
        double current_price
    ) const;
    
    double calculateVWAPDeviation(
        const std::vector<Candle>& candles,
        double current_price
    ) const;
    
    AdvancedOrderFlowMetrics::VolumeProfile calculateVolumeProfile(
        const std::vector<Candle>& candles
    ) const;
    
    double calculateCumulativeDelta(
        const nlohmann::json& orderbook_units
    ) const;
    
    // ===== 5. Position Sizing (Kelly + Volatility) =====
    
    PositionMetrics calculateAdvancedPositionSize(
        double capital,
        double entry_price,
        double stop_loss,
        const analytics::CoinMetrics& metrics,
        const std::vector<Candle>& candles
    ) const;
    
    double calculateKellyFraction(
        double win_rate,
        double avg_win,
        double avg_loss
    ) const;
    
    double adjustForVolatility(
        double kelly_size,
        double volatility
    ) const;
    
    // ===== 6. Dynamic Stops =====
    
    DynamicStops calculateDynamicStops(
        double entry_price,
        const std::vector<Candle>& candles
    ) const;
    
    double calculateATRBasedStop(
        double entry_price,
        const std::vector<Candle>& candles,
        double multiplier
    ) const;
    
    double findNearestSupport(
        double entry_price,
        const std::vector<Candle>& candles
    ) const;
    
    double calculateChandelierExit(
        double highest_price,
        double atr,
        double multiplier = 3.0
    ) const;
    
    double calculateParabolicSAR(
        const std::vector<Candle>& candles,
        double acceleration = 0.02,
        double max_af = 0.20
    ) const;
    
    // ===== 7. Trade Cost Analysis =====
    
    bool isWorthTrading(
        double expected_return,
        double expected_sharpe
    ) const;
    
    double calculateExpectedSlippage(
        const analytics::CoinMetrics& metrics
    ) const;
    
    // ===== 8. Signal Strength =====
    
    double calculateSignalStrength(
        const analytics::CoinMetrics& metrics,
        const std::vector<Candle>& candles,
        const MultiTimeframeSignal& mtf_signal,
        const AdvancedOrderFlowMetrics& order_flow,
        MarketRegime regime
    ) const;
    
    // ===== 9. Risk Management =====
    
    double calculateCVaR(
        double position_size,
        double volatility,
        double confidence_level
    ) const;
    
    double calculateExpectedShortfall(
        const std::vector<double>& returns,
        double confidence_level
    ) const;
    
    // ===== 10. Performance Metrics =====
    
    double calculateSharpeRatio(
        double risk_free_rate = 0.03,
        int periods_per_year = 525600  // 遺꾨큺 湲곗?
    ) const;
    
    double calculateSortinoRatio(
        double mar = 0.0,
        int periods_per_year = 525600
    ) const;
    
    double calculateCalmarRatio() const;
    
    double calculateMaxDrawdown() const;
    
    void updateRollingStatistics();
    
    // ===== 11. Walk-Forward Validation =====
    
    WalkForwardResult validateStrategy(
        const std::vector<Candle>& historical_data
    ) const;
    
    // ===== 12. Helpers =====
    
    double calculateMean(const std::vector<double>& values) const;
    double calculateStdDev(const std::vector<double>& values, double mean) const;
    double calculateVolatility(const std::vector<Candle>& candles) const;
    
    long long getCurrentTimestamp() const;
    bool canTradeNow();
    void recordTrade();
    void resetDailyCounters();
    void resetHourlyCounters();
    void checkCircuitBreaker();
    bool isCircuitBreakerActive() const;
    double getArchetypeQualityBias(int archetype, analytics::MarketRegime regime) const;
    bool shouldBlockArchetypeByAdaptiveStats(int archetype, analytics::MarketRegime regime) const;
    void recordArchetypeOutcome(
        int archetype,
        analytics::MarketRegime regime,
        bool is_win,
        double profit_loss
    );
    void loadAdaptiveArchetypeStats();
    void saveAdaptiveArchetypeStats() const;
    engine::EngineConfig engine_config_;
    bool shouldGenerateSignal(
        double expected_return,
        double expected_sharpe
    ) const;

    int daily_trades_count_ = 0;
    int hourly_trades_count_ = 0;
    int consecutive_losses_ = 0;
    bool circuit_breaker_active_ = false;
    long long circuit_breaker_until_ = 0;
    long long current_day_start_ = 0;
    long long current_hour_start_ = 0;
    mutable long long latest_market_timestamp_ms_ = 0;
    static constexpr long long CIRCUIT_BREAKER_COOLDOWN_MS = 3600000; // 1h
};

} // namespace strategy
} // namespace autolife


