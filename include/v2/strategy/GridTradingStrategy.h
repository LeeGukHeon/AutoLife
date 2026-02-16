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

// ===== Grid Types =====

enum class GridType {
    NONE,
    ARITHMETIC,              // ?깆감 洹몃━??(洹좊벑 媛꾧꺽)
    GEOMETRIC,               // ?깅퉬 洹몃━??(% 媛꾧꺽)
    FIBONACCI,               // ?쇰낫?섏튂 鍮꾩쑉 洹몃━??
    DYNAMIC,                 // ?숈쟻 洹몃━??(蹂?숈꽦 湲곕컲)
    VOLUME_WEIGHTED,         // 嫄곕옒??媛以?洹몃━??
    SUPPORT_RESISTANCE       // 吏吏/????덈꺼 洹몃━??
};

enum class RangeState {
    UNKNOWN,
    RANGING,                 // ?〓낫 以?
    TRENDING_UP,             // ?곸듅 異붿꽭
    TRENDING_DOWN,           // ?섎씫 異붿꽭
    BREAKOUT_UP,             // ?곹뼢 ?뚰뙆
    BREAKOUT_DOWN            // ?섑뼢 ?뚰뙆
};

enum class GridStatus {
    INACTIVE,
    ACTIVE,
    PAUSED,
    REBALANCING,
    EMERGENCY_EXIT           // 湲닿툒 泥?궛 以?
};

enum class ExitReason {
    NONE,
    NORMAL_PROFIT,           // ?뺤긽 ?섏씡 ?ㅽ쁽
    STOP_LOSS,               // ?먯젅
    BREAKOUT,                // ?덉씤吏 ?댄깉
    FLASH_CRASH,             // 湲됰씫
    MAX_TIME,                // 理쒕? 蹂댁쑀 ?쒓컙
    MANUAL                   // ?섎룞 泥?궛
};

// ===== Grid Level =====

struct GridLevel {
    int level_id;
    double price;                    // 洹몃━??媛寃?
    bool buy_order_placed;           // 留ㅼ닔 二쇰Ц ?щ?
    bool buy_order_filled;           // 留ㅼ닔 泥닿껐 ?щ?
    bool sell_order_placed;          // 留ㅻ룄 二쇰Ц ?щ?
    bool sell_order_filled;          // 留ㅻ룄 泥닿껐 ?щ?
    double quantity;                 // ?섎웾
    long long buy_timestamp;         // 留ㅼ닔 ?쒓컙
    long long sell_timestamp;        // 留ㅻ룄 ?쒓컙
    double profit_loss;              // ?덈꺼 ?먯씡
    int round_trips;                 // ?뺣났 嫄곕옒 ?잛닔
    double cumulative_profit;        // ?꾩쟻 ?섏씡
    
    GridLevel()
        : level_id(0), price(0)
        , buy_order_placed(false), buy_order_filled(false)
        , sell_order_placed(false), sell_order_filled(false)
        , quantity(0), buy_timestamp(0), sell_timestamp(0)
        , profit_loss(0), round_trips(0), cumulative_profit(0)
    {}
};

// ===== Grid Risk Limits =====

struct GridRiskLimits {
    double stop_loss_pct;            // 洹몃━???꾩껜 ?먯젅??(%)
    double max_drawdown_pct;         // 理쒕? ?덉슜 ?먯떎 (%)
    double flash_crash_threshold;    // 湲됰씫 媛먯? ?꾧퀎媛?(%)
    double breakout_tolerance_pct;   // ?뚰뙆 ?덉슜 踰붿쐞 (%)
    long long max_holding_time_ms;   // 理쒕? 蹂댁쑀 ?쒓컙
    bool auto_liquidate_on_breakout; // ?뚰뙆???먮룞 泥?궛
    
    GridRiskLimits()
        : stop_loss_pct(0.10)        // 10% ?먯떎
        , max_drawdown_pct(0.15)     // 15% 理쒕? ?먯떎
        , flash_crash_threshold(0.05) // 5遺꾨궡 5% ?섎씫
        , breakout_tolerance_pct(0.02) // 2% ?덉슜
        , max_holding_time_ms(172800000) // 48?쒓컙
        , auto_liquidate_on_breakout(true)
    {}
};

// ===== Grid Configuration =====

struct GridConfiguration {
    GridType type;
    double center_price;             // 以묒떖 媛寃?
    double upper_bound;              // ?곷떒 寃쎄퀎
    double lower_bound;              // ?섎떒 寃쎄퀎
    int num_grids;                   // 洹몃━??媛쒖닔
    double grid_spacing_pct;         // 洹몃━??媛꾧꺽 (%)
    double total_capital_allocated;  // ?좊떦 ?먮낯
    double capital_per_grid;         // 洹몃━?쒕떦 ?먮낯
    bool auto_rebalance;             // ?먮룞 ?ъ“??
    double rebalance_threshold_pct;  // ?ъ“???꾧퀎媛?
    GridRiskLimits risk_limits;      // 由ъ뒪???쒗븳
    
    GridConfiguration()
        : type(GridType::ARITHMETIC)
        , center_price(0), upper_bound(0), lower_bound(0)
        , num_grids(0), grid_spacing_pct(0)
        , total_capital_allocated(0), capital_per_grid(0)
        , auto_rebalance(true), rebalance_threshold_pct(0.05)
    {}
};

// ===== Range Detection =====

struct RangeDetectionMetrics {
    RangeState state;
    double range_width_pct;          // ?덉씤吏 ??
    double range_high;
    double range_low;
    double range_center;
    double adx;                      // ADX (異붿꽭 媛뺣룄)
    double plus_di;                  // +DI
    double minus_di;                 // -DI
    double bb_width;                 // Bollinger Bands Width
    double donchian_width;           // Donchian Width
    double atr;                      // ATR
    double confidence;               // ?〓낫 ?좊ː??
    int consolidation_bars;          // ?〓낫 吏??湲곌컙
    bool is_ranging;                 // ?〓낫 ?щ?
    
    RangeDetectionMetrics()
        : state(RangeState::UNKNOWN)
        , range_width_pct(0), range_high(0), range_low(0)
        , range_center(0), adx(0), plus_di(0), minus_di(0)
        , bb_width(0), donchian_width(0), atr(0)
        , confidence(0), consolidation_bars(0), is_ranging(false)
    {}
};

// ===== Grid Signal =====

struct GridSignalMetrics {
    GridType recommended_type;
    double optimal_spacing_pct;      // 理쒖쟻 媛꾧꺽
    int optimal_grid_count;          // 理쒖쟻 洹몃━????
    double expected_profit_per_cycle; // ?ъ씠?대떦 ?덉긽 ?섏씡
    double expected_cycles_per_day;  // ?쇱씪 ?덉긽 ?ъ씠??
    double fee_adjusted_profit;      // ?섏닔猷?李④컧 ???섏씡
    bool is_profitable_after_fees;   // ?섏닔猷????섏씡??
    double risk_score;               // 由ъ뒪???먯닔
    double strength;                 // ?좏샇 媛뺣룄
    bool is_valid;
    
    GridSignalMetrics()
        : recommended_type(GridType::NONE)
        , optimal_spacing_pct(0), optimal_grid_count(0)
        , expected_profit_per_cycle(0), expected_cycles_per_day(0)
        , fee_adjusted_profit(0), is_profitable_after_fees(false)
        , risk_score(0), strength(0), is_valid(false)
    {}
};

// ===== Flash Crash Detection =====

struct FlashCrashMetrics {
    bool detected;
    double price_drop_pct;           // ?섎씫 ??
    double drop_speed;               // ?섎씫 ?띾룄 (%/遺?
    long long detection_time;
    int consecutive_drops;           // ?곗냽 ?섎씫 罹붾뱾
    
    FlashCrashMetrics()
        : detected(false), price_drop_pct(0)
        , drop_speed(0), detection_time(0)
        , consecutive_drops(0)
    {}
};

// ===== Grid Position =====

struct GridPositionData {
    std::string market;
    GridConfiguration config;
    GridStatus status;
    std::map<int, GridLevel> levels; // level_id -> GridLevel
    double total_invested;
    double total_profit;
    double unrealized_pnl;
    double realized_pnl;
    double max_drawdown;             // 理쒕? ?먯떎
    double current_drawdown;         // ?꾩옱 ?먯떎
    int active_buy_orders;
    int active_sell_orders;
    int completed_cycles;            // ?꾨즺???ъ씠??
    long long creation_timestamp;
    long long last_rebalance_timestamp;
    long long last_price_update_timestamp;
    double last_price;
    FlashCrashMetrics flash_crash;   // 湲됰씫 媛먯?
    ExitReason exit_reason;
    bool exit_requested;
    
    GridPositionData()
        : status(GridStatus::INACTIVE)
        , total_invested(0), total_profit(0)
        , unrealized_pnl(0), realized_pnl(0)
        , max_drawdown(0), current_drawdown(0)
        , active_buy_orders(0), active_sell_orders(0)
        , completed_cycles(0), creation_timestamp(0)
        , last_rebalance_timestamp(0)
        , last_price_update_timestamp(0), last_price(0)
        , exit_reason(ExitReason::NONE), exit_requested(false)
    {}
};

// ===== Rolling Statistics =====

struct GridRollingStatistics {
    double rolling_win_rate;
    double avg_profit_per_cycle;
    double avg_cycle_time_minutes;
    double rolling_profit_factor;
    double grid_efficiency;          // 洹몃━???⑥쑉??
    double avg_daily_return;         // ?쇳룊洹??섏씡瑜?
    int total_grids_created;
    int successful_grids;
    int failed_grids;
    int emergency_exits;
    double avg_range_accuracy;       // ?덉씤吏 ?덉륫 ?뺥솗??
    double sharpe_ratio;
    
    GridRollingStatistics()
        : rolling_win_rate(0), avg_profit_per_cycle(0)
        , avg_cycle_time_minutes(0), rolling_profit_factor(0)
        , grid_efficiency(0), avg_daily_return(0)
        , total_grids_created(0), successful_grids(0)
        , failed_grids(0), emergency_exits(0)
        , avg_range_accuracy(0), sharpe_ratio(0)
    {}
};

// ===== Grid Trading Strategy =====

class GridTradingStrategy : public IStrategy {
public:
    GridTradingStrategy(std::shared_ptr<network::UpbitHttpClient> client);
    
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
    
    void updateState(const std::string& market, double current_price) override;
    
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
    std::vector<OrderRequest> drainOrderRequests() override;
    void onOrderResult(const OrderResult& result) override;
    std::vector<std::string> drainReleasedMarkets() override;
    std::vector<std::string> getActiveMarkets() const override;
    
    // === Grid ?꾩슜 湲곕뒫 ===
    
    bool shouldRebalanceGrid(const std::string& market, double current_price);
    void updateGridLevels(const std::string& market, double current_price);
    bool shouldEmergencyExit(const std::string& market, double current_price);
    void emergencyLiquidateGrid(const std::string& market, ExitReason reason);
    
private:
    std::shared_ptr<network::UpbitHttpClient> client_;
    bool enabled_;
    Statistics stats_;
    mutable std::recursive_mutex mutex_;
    
    // [?좉퇋 異붽?] 以묐났 吏꾩엯 諛⑹???Set (湲곗〈 active_grids_ 留듦낵 援щ텇)
    std::set<std::string> active_positions_;
    
    // ?대젰
    std::deque<double> recent_returns_;
    std::deque<double> cycle_times_;
    std::deque<long long> trade_timestamps_;
    
    GridRollingStatistics rolling_stats_;
    long long last_signal_time_;
    
    // 洹몃━???ъ???異붿쟻
    std::map<std::string, GridPositionData> active_grids_;
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
    std::deque<OrderRequest> pending_orders_;
    std::deque<std::string> released_markets_;

    std::map<std::string, analytics::CoinMetrics> last_metrics_cache_;
    std::map<std::string, std::vector<Candle>> last_candles_cache_;
    std::map<std::string, double> last_price_cache_;
    
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
    
    // ===== 嫄곕옒 鍮덈룄 ?쒖뼱 =====
    
    int daily_trades_count_;
    int hourly_trades_count_;
    long long current_day_start_;
    long long current_hour_start_;
    mutable long long latest_market_timestamp_ms_ = 0;
    
    static constexpr int MAX_DAILY_GRID_TRADES = 15;
    static constexpr int MAX_HOURLY_GRID_TRADES = 5;
    
    // ===== ?쒗궥 釉뚮젅?댁빱 =====
    
    int consecutive_losses_;
    bool circuit_breaker_active_;
    long long circuit_breaker_until_;
    
    static constexpr int MAX_CONSECUTIVE_LOSSES = 3;
    static constexpr long long CIRCUIT_BREAKER_COOLDOWN_MS = 3600000;  // 1?쒓컙
    
    // ===== ?뚮씪誘명꽣 =====
    
    static constexpr double UPBIT_FEE_RATE = 0.0005;
    static constexpr double EXPECTED_SLIPPAGE = 0.0003;
    static constexpr double MIN_ORDER_AMOUNT_KRW = 5000.0;
    
    // Grid ?ㅼ젙
    static constexpr int MIN_GRID_COUNT = 5;
    static constexpr int MAX_GRID_COUNT = 20;
    static constexpr double BASE_GRID_SPACING_PCT = 0.01;     // 1%
    static constexpr double MIN_GRID_SPACING_PCT = 0.006;      // 0.6% (?섏닔猷?3諛?
    static constexpr double MAX_GRID_SPACING_PCT = 0.03;       // 3%
    
    // Range Detection
    static constexpr double MIN_RANGE_WIDTH_PCT = 0.03;        // 3%
    static constexpr double MAX_RANGE_WIDTH_PCT = 0.15;        // 15%
    static constexpr double ADX_RANGING_THRESHOLD = 25.0;       // ADX < 25 = ?〓낫
    static constexpr double ADX_STRONG_TREND = 40.0;            // ADX > 40 = 媛뺥븳 異붿꽭
    static constexpr int MIN_CONSOLIDATION_BARS = 20;
    
    // Risk Management
    static constexpr double MAX_GRID_CAPITAL_PCT = 0.30;       // 理쒕? 30%
    static constexpr double MIN_CAPITAL_PER_GRID = 10000.0;    // 洹몃━?쒕떦 理쒖냼 1留뚯썝
    static constexpr double GRID_STOP_LOSS_PCT = 0.10;         // 洹몃━???꾩껜 ?먯젅 10%
    static constexpr double FLASH_CRASH_THRESHOLD_PCT = 0.05;  // 5% 湲됰씫
    static constexpr double FLASH_CRASH_SPEED = 1.0;           // 1%/遺?
    
    // Rebalancing
    static constexpr double REBALANCE_THRESHOLD_PCT = 0.05;    // 5% ?댄깉???ъ“??
    static constexpr long long MIN_REBALANCE_INTERVAL_MS = 3600000;  // 理쒖냼 1?쒓컙
    
    // Exit
    static constexpr double BREAKOUT_EXIT_TOLERANCE = 0.02;    // 2% 珥덇낵??泥?궛
    static constexpr double MAX_HOLDING_TIME_HOURS = 48.0;     // 48?쒓컙
    static constexpr double MIN_LIQUIDITY_SCORE = 60.0;
    static constexpr double MIN_SIGNAL_STRENGTH = 0.60;
    static constexpr int MIN_SIGNAL_INTERVAL_SEC = 900;        // 15遺?
    
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
    
    // ===== 1. Range Detection =====
    
    RangeDetectionMetrics detectRange(
        const std::vector<Candle>& candles,
        double current_price
    ) const;
    
    double calculateADX(
        const std::vector<Candle>& candles,
        int period = 14
    ) const;
    
    void calculateDMI(
        const std::vector<Candle>& candles,
        int period,
        double& plus_di,
        double& minus_di
    ) const;
    
    bool isConsolidating(
        const std::vector<Candle>& candles,
        int lookback = 50
    ) const;
    
    // ===== 2. Grid Configuration =====
    
    GridConfiguration createGridConfiguration(
        const std::string& market,
        const RangeDetectionMetrics& range,
        const analytics::CoinMetrics& metrics,
        double current_price,
        double available_capital
    );
    
    int calculateOptimalGridCount(
        double range_width_pct,
        double volatility,
        double capital
    ) const;
    
    double calculateOptimalSpacing(
        double range_width_pct,
        double volatility,
        int grid_count
    ) const;
    
    GridType selectGridType(
        const RangeDetectionMetrics& range,
        double volatility,
        double price_level
    ) const;
    
    // ===== 3. Grid Generation =====
    
    std::map<int, GridLevel> generateGridLevels(
        const GridConfiguration& config
    );
    
    std::map<int, GridLevel> generateArithmeticGrid(
        double center,
        double spacing,
        int count
    );
    
    std::map<int, GridLevel> generateGeometricGrid(
        double center,
        double spacing_pct,
        int count
    );
    
    std::map<int, GridLevel> generateFibonacciGrid(
        double center,
        double range_width,
        int count
    );
    
    std::map<int, GridLevel> generateDynamicGrid(
        const GridConfiguration& config,
        const std::vector<Candle>& candles
    );
    
    // ===== 4. Grid Signal Analysis =====
    
    GridSignalMetrics analyzeGridOpportunity(
        const std::string& market,
        const analytics::CoinMetrics& metrics,
        const std::vector<Candle>& candles,
        double current_price
    );
    
    double calculateExpectedProfitPerCycle(
        double grid_spacing_pct,
        double fee_rate,
        double slippage
    ) const;
    
    bool validateProfitabilityAfterFees(
        double grid_spacing_pct
    ) const;
    
    double calculateGridRiskScore(
        const RangeDetectionMetrics& range,
        double volatility
    ) const;
    
    // ===== 5. Grid Management =====
    
    void executeGridBuy(
        const std::string& market,
        GridLevel& level,
        double current_price
    );
    
    void executeGridSell(
        const std::string& market,
        GridLevel& level,
        double current_price
    );
    
    bool shouldPlaceBuyOrder(
        const GridLevel& level,
        double current_price
    ) const;
    
    bool shouldPlaceSellOrder(
        const GridLevel& level,
        double current_price,
        double spacing_pct
    ) const;
    
    // ===== 6. Risk Monitoring =====
    
    bool checkStopLoss(
        const GridPositionData& grid,
        double current_price
    ) const;
    
    bool detectBreakout(
        const GridPositionData& grid,
        const RangeDetectionMetrics& range,
        double current_price
    ) const;
    
    FlashCrashMetrics detectFlashCrash(
        const std::string& market,
        const std::vector<Candle>& candles,
        double current_price
    );
    
    bool isMaxDrawdownExceeded(
        const GridPositionData& grid
    ) const;
    
    // ===== 7. Grid Rebalancing =====
    
    bool needsRebalancing(
        const GridPositionData& grid,
        double current_price
    ) const;
    
    void rebalanceGrid(
        const std::string& market,
        double current_price,
        const RangeDetectionMetrics& new_range
    );
    
    // ===== 8. Exit Management =====
    
    void exitGrid(
        const std::string& market,
        ExitReason reason,
        double current_price
    );
    
    void liquidateAllLevels(
        GridPositionData& grid,
        double current_price
    );
    
    // ===== 9. Profit Management =====
    
    double calculateGridUnrealizedPnL(
        const GridPositionData& grid,
        double current_price
    ) const;
    
    void compoundProfits(
        GridPositionData& grid
    );
    
    // ===== 10. Performance Tracking =====
    
    void updateRollingStatistics();
    
    double calculateGridEfficiency(
        const GridPositionData& grid
    ) const;
    
    // ===== 11. Helpers =====
    
    double calculateMean(const std::vector<double>& values) const;
    double calculateStdDev(const std::vector<double>& values, double mean) const;
    double calculateVolatility(const std::vector<Candle>& candles) const;
    double calculateATR(const std::vector<Candle>& candles, int period) const;
    
    std::vector<double> extractPrices(
        const std::vector<Candle>& candles,
        const std::string& type = "close"
    ) const;
    
    long long getCurrentTimestamp() const;
    
    bool shouldGenerateGridSignal(
        const GridSignalMetrics& metrics
    ) const;
    engine::EngineConfig engine_config_;
    std::vector<Candle> parseCandlesFromJson(
        const nlohmann::json& json_data
    ) const;
};

} // namespace strategy
} // namespace autolife


