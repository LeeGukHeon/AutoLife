#pragma once

#include "strategy/IStrategy.h"
#include "network/UpbitHttpClient.h"
#include "engine/TradingEngine.h"
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
    ARITHMETIC,              // 등차 그리드 (균등 간격)
    GEOMETRIC,               // 등비 그리드 (% 간격)
    FIBONACCI,               // 피보나치 비율 그리드
    DYNAMIC,                 // 동적 그리드 (변동성 기반)
    VOLUME_WEIGHTED,         // 거래량 가중 그리드
    SUPPORT_RESISTANCE       // 지지/저항 레벨 그리드
};

enum class RangeState {
    UNKNOWN,
    RANGING,                 // 횡보 중
    TRENDING_UP,             // 상승 추세
    TRENDING_DOWN,           // 하락 추세
    BREAKOUT_UP,             // 상향 돌파
    BREAKOUT_DOWN            // 하향 돌파
};

enum class GridStatus {
    INACTIVE,
    ACTIVE,
    PAUSED,
    REBALANCING,
    EMERGENCY_EXIT           // 긴급 청산 중
};

enum class ExitReason {
    NONE,
    NORMAL_PROFIT,           // 정상 수익 실현
    STOP_LOSS,               // 손절
    BREAKOUT,                // 레인지 이탈
    FLASH_CRASH,             // 급락
    MAX_TIME,                // 최대 보유 시간
    MANUAL                   // 수동 청산
};

// ===== Grid Level =====

struct GridLevel {
    int level_id;
    double price;                    // 그리드 가격
    bool buy_order_placed;           // 매수 주문 여부
    bool buy_order_filled;           // 매수 체결 여부
    bool sell_order_placed;          // 매도 주문 여부
    bool sell_order_filled;          // 매도 체결 여부
    double quantity;                 // 수량
    long long buy_timestamp;         // 매수 시간
    long long sell_timestamp;        // 매도 시간
    double profit_loss;              // 레벨 손익
    int round_trips;                 // 왕복 거래 횟수
    double cumulative_profit;        // 누적 수익
    
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
    double stop_loss_pct;            // 그리드 전체 손절선 (%)
    double max_drawdown_pct;         // 최대 허용 손실 (%)
    double flash_crash_threshold;    // 급락 감지 임계값 (%)
    double breakout_tolerance_pct;   // 돌파 허용 범위 (%)
    long long max_holding_time_ms;   // 최대 보유 시간
    bool auto_liquidate_on_breakout; // 돌파시 자동 청산
    
    GridRiskLimits()
        : stop_loss_pct(0.10)        // 10% 손실
        , max_drawdown_pct(0.15)     // 15% 최대 손실
        , flash_crash_threshold(0.05) // 5분내 5% 하락
        , breakout_tolerance_pct(0.02) // 2% 허용
        , max_holding_time_ms(172800000) // 48시간
        , auto_liquidate_on_breakout(true)
    {}
};

// ===== Grid Configuration =====

struct GridConfiguration {
    GridType type;
    double center_price;             // 중심 가격
    double upper_bound;              // 상단 경계
    double lower_bound;              // 하단 경계
    int num_grids;                   // 그리드 개수
    double grid_spacing_pct;         // 그리드 간격 (%)
    double total_capital_allocated;  // 할당 자본
    double capital_per_grid;         // 그리드당 자본
    bool auto_rebalance;             // 자동 재조정
    double rebalance_threshold_pct;  // 재조정 임계값
    GridRiskLimits risk_limits;      // 리스크 제한
    
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
    double range_width_pct;          // 레인지 폭
    double range_high;
    double range_low;
    double range_center;
    double adx;                      // ADX (추세 강도)
    double plus_di;                  // +DI
    double minus_di;                 // -DI
    double bb_width;                 // Bollinger Bands Width
    double donchian_width;           // Donchian Width
    double atr;                      // ATR
    double confidence;               // 횡보 신뢰도
    int consolidation_bars;          // 횡보 지속 기간
    bool is_ranging;                 // 횡보 여부
    
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
    double optimal_spacing_pct;      // 최적 간격
    int optimal_grid_count;          // 최적 그리드 수
    double expected_profit_per_cycle; // 사이클당 예상 수익
    double expected_cycles_per_day;  // 일일 예상 사이클
    double fee_adjusted_profit;      // 수수료 차감 후 수익
    bool is_profitable_after_fees;   // 수수료 후 수익성
    double risk_score;               // 리스크 점수
    double strength;                 // 신호 강도
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
    double price_drop_pct;           // 하락 폭
    double drop_speed;               // 하락 속도 (%/분)
    long long detection_time;
    int consecutive_drops;           // 연속 하락 캔들
    
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
    double max_drawdown;             // 최대 손실
    double current_drawdown;         // 현재 손실
    int active_buy_orders;
    int active_sell_orders;
    int completed_cycles;            // 완료된 사이클
    long long creation_timestamp;
    long long last_rebalance_timestamp;
    long long last_price_update_timestamp;
    double last_price;
    FlashCrashMetrics flash_crash;   // 급락 감지
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
    double grid_efficiency;          // 그리드 효율성
    double avg_daily_return;         // 일평균 수익률
    int total_grids_created;
    int successful_grids;
    int failed_grids;
    int emergency_exits;
    double avg_range_accuracy;       // 레인지 예측 정확도
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
    
    // === Grid 전용 기능 ===
    
    bool shouldRebalanceGrid(const std::string& market, double current_price);
    void updateGridLevels(const std::string& market, double current_price);
    bool shouldEmergencyExit(const std::string& market, double current_price);
    void emergencyLiquidateGrid(const std::string& market, ExitReason reason);
    
    GridRollingStatistics getRollingStatistics() const;

private:
    std::shared_ptr<network::UpbitHttpClient> client_;
    bool enabled_;
    Statistics stats_;
    mutable std::recursive_mutex mutex_;
    
    // [신규 추가] 중복 진입 방지용 Set (기존 active_grids_ 맵과 구분)
    std::set<std::string> active_positions_;
    
    // 이력
    std::deque<double> recent_returns_;
    std::deque<double> cycle_times_;
    std::deque<long long> trade_timestamps_;
    
    GridRollingStatistics rolling_stats_;
    long long last_signal_time_;
    
    // 그리드 포지션 추적
    std::map<std::string, GridPositionData> active_grids_;
    std::deque<OrderRequest> pending_orders_;
    std::deque<std::string> released_markets_;

    std::map<std::string, analytics::CoinMetrics> last_metrics_cache_;
    std::map<std::string, std::vector<Candle>> last_candles_cache_;
    std::map<std::string, double> last_price_cache_;
    
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
    
    static constexpr int MAX_DAILY_GRID_TRADES = 15;
    static constexpr int MAX_HOURLY_GRID_TRADES = 5;
    
    // ===== 서킷 브레이커 =====
    
    int consecutive_losses_;
    bool circuit_breaker_active_;
    long long circuit_breaker_until_;
    
    static constexpr int MAX_CONSECUTIVE_LOSSES = 3;
    static constexpr long long CIRCUIT_BREAKER_COOLDOWN_MS = 3600000;  // 1시간
    
    // ===== 파라미터 =====
    
    static constexpr double UPBIT_FEE_RATE = 0.0005;
    static constexpr double EXPECTED_SLIPPAGE = 0.0003;
    static constexpr double MIN_ORDER_AMOUNT_KRW = 5000.0;
    
    // Grid 설정
    static constexpr int MIN_GRID_COUNT = 5;
    static constexpr int MAX_GRID_COUNT = 20;
    static constexpr double BASE_GRID_SPACING_PCT = 0.01;     // 1%
    static constexpr double MIN_GRID_SPACING_PCT = 0.006;      // 0.6% (수수료 3배)
    static constexpr double MAX_GRID_SPACING_PCT = 0.03;       // 3%
    
    // Range Detection
    static constexpr double MIN_RANGE_WIDTH_PCT = 0.03;        // 3%
    static constexpr double MAX_RANGE_WIDTH_PCT = 0.15;        // 15%
    static constexpr double ADX_RANGING_THRESHOLD = 25.0;       // ADX < 25 = 횡보
    static constexpr double ADX_STRONG_TREND = 40.0;            // ADX > 40 = 강한 추세
    static constexpr int MIN_CONSOLIDATION_BARS = 20;
    
    // Risk Management
    static constexpr double MAX_GRID_CAPITAL_PCT = 0.30;       // 최대 30%
    static constexpr double MIN_CAPITAL_PER_GRID = 10000.0;    // 그리드당 최소 1만원
    static constexpr double GRID_STOP_LOSS_PCT = 0.10;         // 그리드 전체 손절 10%
    static constexpr double FLASH_CRASH_THRESHOLD_PCT = 0.05;  // 5% 급락
    static constexpr double FLASH_CRASH_SPEED = 1.0;           // 1%/분
    
    // Rebalancing
    static constexpr double REBALANCE_THRESHOLD_PCT = 0.05;    // 5% 이탈시 재조정
    static constexpr long long MIN_REBALANCE_INTERVAL_MS = 3600000;  // 최소 1시간
    
    // Exit
    static constexpr double BREAKOUT_EXIT_TOLERANCE = 0.02;    // 2% 초과시 청산
    static constexpr double MAX_HOLDING_TIME_HOURS = 48.0;     // 48시간
    static constexpr double MIN_LIQUIDITY_SCORE = 60.0;
    static constexpr double MIN_SIGNAL_STRENGTH = 0.60;
    static constexpr int MIN_SIGNAL_INTERVAL_SEC = 900;        // 15분
    
    // ===== API 호출 관리 =====
    
    bool canMakeOrderBookAPICall() const;
    bool canMakeCandleAPICall() const;
    void recordAPICall() const;
    
    nlohmann::json getCachedOrderBook(const std::string& market);
    std::vector<Candle> getCachedCandles(const std::string& market, int count);
    
    // ===== 거래 빈도 관리 =====
    
    bool canTradeNow();
    void recordTrade();
    void resetDailyCounters();
    void resetHourlyCounters();
    
    // ===== 서킷 브레이커 =====
    
    void checkCircuitBreaker();
    void activateCircuitBreaker();
    bool isCircuitBreakerActive() const;
    
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
