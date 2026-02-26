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
    struct Phase3PolicySnapshot {
        struct PrimaryPriorityPolicy {
            bool enabled = false;
            double margin_score_shift = 0.10;
            double margin_score_scale = 0.20;
            double edge_score_shift = 0.0005;
            double edge_score_scale = 0.0025;
            double prob_weight = 0.50;
            double margin_weight = 0.22;
            double liquidity_weight = 0.10;
            double strength_weight = 0.10;
            double edge_weight = 0.08;
            double hostile_prob_weight = 0.54;
            double hostile_margin_weight = 0.22;
            double hostile_liquidity_weight = 0.11;
            double hostile_strength_weight = 0.09;
            double hostile_edge_weight = 0.04;
            double strong_buy_bonus = 0.02;
            double margin_bonus_scale = 0.08;
            double margin_bonus_cap = 0.03;
            double range_penalty = 0.11;
            double range_bonus = 0.03;
            double range_penalty_strength_floor = 0.50;
            double range_penalty_margin_floor = 0.008;
            double range_penalty_prob_floor = 0.54;
            double range_bonus_margin_floor = 0.012;
            double range_bonus_prob_floor = 0.57;
            double uptrend_bonus = 0.03;
            double uptrend_bonus_margin_floor = 0.0;
            double uptrend_bonus_prob_floor = 0.52;
        };

        struct ManagerFilterPolicy {
            bool enabled = false;
            double required_strength_cap = 0.95;
            double core_signal_ownership_strength_relief = 0.02;
            double core_signal_ownership_expected_value_floor = -0.00005;
            double policy_hold_strength_add = 0.05;
            double policy_hold_expected_value_add_core = 0.00010;
            double policy_hold_expected_value_add_other = 0.00018;
            double off_trend_strength_add = 0.06;
            double off_trend_expected_value_add_core = 0.00009;
            double off_trend_expected_value_add_other = 0.00015;
            double hostile_regime_strength_add = 0.03;
            double hostile_regime_expected_value_add_core = 0.00005;
            double hostile_regime_expected_value_add_other = 0.00008;
            double probabilistic_confidence_strength_relief_scale = 0.03;
            double probabilistic_confidence_expected_value_relief_scale = 0.00010;
            double probabilistic_confidence_prob_shift = 0.50;
            double probabilistic_confidence_prob_scale = 0.25;
            double probabilistic_confidence_margin_shift = 0.02;
            double probabilistic_confidence_margin_scale = 0.12;
            double probabilistic_confidence_prob_weight = 0.40;
            double probabilistic_confidence_margin_weight = 0.60;
            double probabilistic_high_confidence_threshold = 0.65;
            double history_gate_min_win_rate_base = 0.50;
            double history_gate_min_profit_factor_base = 1.10;
            int history_gate_min_sample_trades_base = 16;
            double history_gate_win_rate_add_trending_down = 0.03;
            double history_gate_profit_factor_add_trending_down = 0.05;
            double history_gate_win_rate_add_high_volatility = 0.02;
            double history_gate_profit_factor_add_high_volatility = 0.04;
            int history_min_sample_hostile = 18;
            int history_min_sample_calm = 36;
            double history_severe_win_rate_shortfall = 0.08;
            double history_severe_profit_factor_shortfall = 0.30;
            int history_relief_max_trade_count = 52;
            double history_relief_min_h5_calibrated = 0.48;
            double history_relief_min_h5_margin = -0.012;
            double history_guard_scale_base = 0.45;
            double history_guard_scale_confidence_scale = 0.35;
            double history_guard_scale_min_hostile = 0.18;
            double history_guard_scale_min_calm = 0.10;
            double history_guard_scale_max_hostile = 0.60;
            double history_guard_scale_max_calm = 0.45;
            double history_strength_bump_prob = 0.012;
            double history_strength_bump_non_prob = 0.05;
            double history_edge_bump_core_prob = 0.00002;
            double history_edge_bump_core_non_prob = 0.00005;
            double history_edge_bump_other_prob = 0.00003;
            double history_edge_bump_other_non_prob = 0.00010;
            double rr_guard_floor_hostile = 1.12;
            double rr_guard_floor_calm = 1.08;
            double rr_guard_skip_min_rr = 0.95;
            double rr_guard_scale_base = 0.90;
            double rr_guard_scale_confidence_scale = 0.60;
            double rr_guard_scale_min = 0.20;
            double rr_guard_scale_max = 0.90;
            double rr_guard_strength_add = 0.03;
            double rr_guard_expected_value_add_core = 0.00003;
            double rr_guard_expected_value_add_other = 0.00006;
            double frontier_uncertainty_prob_weight = 0.60;
            double frontier_uncertainty_ev_weight = 0.40;
            double scan_prefilter_margin_add_hostile = 0.015;
            double scan_prefilter_margin_add_trending_up = -0.005;
            double scan_prefilter_margin_clamp_min = -0.30;
            double scan_prefilter_margin_clamp_max = 0.15;
            double scan_prefilter_margin_with_regime_clamp_min = -0.30;
            double scan_prefilter_margin_with_regime_clamp_max = 0.30;
        };

        bool frontier_enabled = false;
        bool ev_calibration_enabled = false;
        bool cost_tail_enabled = false;
        bool adaptive_ev_blend_enabled = false;
        bool diagnostics_v2_enabled = false;
        bool edge_regressor_used = false;
        bool ev_calibration_applied = false;
        double ev_confidence = 1.0;
        double expected_edge_raw_pct = 0.0;
        double expected_edge_calibrated_pct = 0.0;
        double cost_entry_pct = 0.0;
        double cost_exit_pct = 0.0;
        double cost_tail_pct = 0.0;
        double cost_used_pct = 0.0;
        double adaptive_ev_blend = 0.20;
        double frontier_k_margin = 0.0;
        double frontier_k_margin_scale = 1.0;
        double frontier_k_uncertainty = 0.0;
        double frontier_k_cost_tail = 0.0;
        double required_ev_offset = 0.0;
        double frontier_min_required_ev = -0.0002;
        double frontier_max_required_ev = 0.0050;
        double frontier_margin_floor = -1.0;
        double frontier_ev_confidence_floor = 0.0;
        double frontier_cost_tail_reject_threshold_pct = 1.0;
        double ev_blend_scale = 1.0;
        bool primary_minimums_enabled = false;
        double primary_min_h5_calibrated = 0.48;
        double primary_min_h5_margin = -0.03;
        double primary_min_liquidity_score = 42.0;
        double primary_min_signal_strength = 0.34;
        PrimaryPriorityPolicy primary_priority;
        ManagerFilterPolicy manager_filter;
        std::string cost_mode = "mean_mode";
    };

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
    double probabilistic_h5_uncertainty_std;
    int probabilistic_ensemble_member_count;
    bool probabilistic_runtime_applied;
    Phase3PolicySnapshot phase3;
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
        , probabilistic_h5_uncertainty_std(0.0)
        , probabilistic_ensemble_member_count(1)
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


