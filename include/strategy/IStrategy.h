#pragma once

#include "common/Types.h"
#include "analytics/TechnicalIndicators.h"
#include "analytics/MarketScanner.h"
#include "analytics/RegimeDetector.h"
#include <string>
#include <vector>
#include <memory>

namespace autolife {
namespace strategy {

// 전략이 생성할 수 있는 기본 매매 신호 타입
enum class SignalType {
    NONE,
    STRONG_BUY,
    BUY,
    HOLD,
    SELL,
    STRONG_SELL
};

// 주문 타입 정책 (지정가/시장가/폴백)
enum class OrderTypePolicy {
    LIMIT,
    MARKET,
    LIMIT_WITH_FALLBACK,
    LIMIT_AGGRESSIVE
};

// 주문 방향
enum class OrderSide {
    BUY,
    SELL
};

// 주문 요청 DTO
struct OrderRequest {
    std::string market;
    OrderSide side;
    double price;
    double quantity;
    int level_id;
    std::string reason;

    OrderRequest()
        : side(OrderSide::BUY)
        , price(0.0)
        , quantity(0.0)
        , level_id(-1)
    {}
};

// 주문 결과 DTO
struct OrderResult {
    std::string market;
    OrderSide side;
    bool success;
    double executed_price;
    double executed_volume;
    int level_id;
    std::string reason;

    OrderResult()
        : side(OrderSide::BUY)
        , success(false)
        , executed_price(0.0)
        , executed_volume(0.0)
        , level_id(-1)
    {}
};

// 전략이 엔진으로 전달하는 표준 신호 객체
struct Signal {
    SignalType type;
    std::string market;
    std::string strategy_name;
    double strength;
    double entry_price;
    double entry_amount;
    double stop_loss;
    double take_profit_1;
    double take_profit_2;
    double breakeven_trigger;
    double trailing_start;
    double position_size;
    
    OrderTypePolicy buy_order_type;
    OrderTypePolicy sell_order_type;
    
    int max_retries;
    int retry_wait_ms;
    
    double signal_filter;

    double expected_return_pct;
    double expected_risk_pct;
    double expected_value;
    double probabilistic_h1_calibrated;
    double probabilistic_h5_calibrated;
    double probabilistic_h5_threshold;
    double probabilistic_h5_margin;
    bool probabilistic_runtime_applied;
    double liquidity_score;
    double volatility;
    double strategy_win_rate;
    double strategy_profit_factor;
    int strategy_trade_count;           // strategy trade count
    double score;                       // integrated entry score
    analytics::MarketRegime market_regime; // signal-time market regime
    std::string entry_archetype;        // normalized entry archetype label
    bool used_preloaded_tf_5m;          // scanner preloaded 5m candles used
    bool used_preloaded_tf_1h;          // scanner preloaded 1h candles used
    bool used_resampled_tf_fallback;    // fallback to in-strategy resampling path
    
    std::string reason;
    long long timestamp;
    
    Signal()
        : type(SignalType::NONE)
        , strength(0.0)
        , entry_price(0.0)
        , stop_loss(0.0)
        , take_profit_1(0.0)
        , take_profit_2(0.0)
        , breakeven_trigger(0.0)
        , trailing_start(0.0)
        , position_size(0.0)
        , buy_order_type(OrderTypePolicy::LIMIT_WITH_FALLBACK)
        , sell_order_type(OrderTypePolicy::LIMIT_WITH_FALLBACK)
        , max_retries(3)
        , retry_wait_ms(1000)
        , signal_filter(0.5)
        , expected_return_pct(0.0)
        , expected_risk_pct(0.0)
        , expected_value(0.0)
        , probabilistic_h1_calibrated(0.5)
        , probabilistic_h5_calibrated(0.5)
        , probabilistic_h5_threshold(0.6)
        , probabilistic_h5_margin(0.0)
        , probabilistic_runtime_applied(false)
        , liquidity_score(0.0)
        , volatility(0.0)
        , strategy_win_rate(0.0)
        , strategy_profit_factor(0.0)
        , strategy_trade_count(0)
        , score(0.0)
        , market_regime(analytics::MarketRegime::UNKNOWN)
        , entry_archetype("UNSPECIFIED")
        , used_preloaded_tf_5m(false)
        , used_preloaded_tf_1h(false)
        , used_resampled_tf_fallback(false)
        , timestamp(0)
    {}
    
    double getPrimaryTakeProfit() const {
        return (take_profit_2 > 0) ? take_profit_2 : take_profit_1;
    }
};

// 전략 메타 정보
struct StrategyInfo {
    std::string name;
    std::string description;
    std::string timeframe;
    double min_capital;
    double expected_winrate;
    double risk_level;
    
    StrategyInfo()
        : min_capital(0)
        , expected_winrate(0.5)
        , risk_level(5.0)
    {}
};

// 모든 전략이 따라야 하는 공통 인터페이스
class IStrategy {
public:
    virtual ~IStrategy() = default;
    
    virtual StrategyInfo getInfo() const = 0;

    virtual Signal generateSignal(
        const std::string& market,
        const analytics::CoinMetrics& metrics,
        const std::vector<Candle>& candles,
        double current_price,
        double available_capital,
        const analytics::RegimeAnalysis& regime
    ) = 0;
    
    virtual bool shouldEnter(
        const std::string& market,
        const analytics::CoinMetrics& metrics,
        const std::vector<Candle>& candles,
        double current_price,
        const analytics::RegimeAnalysis& regime
    ) = 0;
    
    virtual bool shouldExit(
        const std::string& market,
        double entry_price,
        double current_price,
        double holding_time_seconds
    ) = 0;
    
    virtual double calculateStopLoss(
        double entry_price,
        const std::vector<Candle>& candles
    ) = 0;
    
    virtual double calculateTakeProfit(
        double entry_price,
        const std::vector<Candle>& candles
    ) = 0;
    
    virtual double calculatePositionSize(
        double capital,
        double entry_price,
        double stop_loss,
        const analytics::CoinMetrics& metrics
    ) = 0;
    
    virtual void setEnabled(bool enabled) = 0;
    virtual bool isEnabled() const = 0;
    
    struct Statistics {
        int total_signals;
        int winning_trades;
        int losing_trades;
        double total_profit;
        double total_loss;
        double win_rate;
        double avg_profit;
        double avg_loss;
        double profit_factor;
        double sharpe_ratio;
        
        Statistics()
            : total_signals(0), winning_trades(0), losing_trades(0)
            , total_profit(0), total_loss(0), win_rate(0)
            , avg_profit(0), avg_loss(0), profit_factor(0), sharpe_ratio(0)
        {}
    };
    
    virtual Statistics getStatistics() const = 0;
    virtual void updateStatistics(const std::string& market, bool is_win, double profit_loss) = 0;
    virtual void setStatistics(const Statistics& stats) { (void)stats; }
    virtual void updateState(const std::string&, double) {}
    virtual bool onSignalAccepted(const Signal&, double) { return false; }
    virtual std::vector<OrderRequest> drainOrderRequests() { return {}; }
    virtual void onOrderResult(const OrderResult&) {}
    virtual std::vector<std::string> drainReleasedMarkets() { return {}; }
    virtual std::vector<std::string> getActiveMarkets() const { return {}; }
};

} // namespace strategy
} // namespace autolife


