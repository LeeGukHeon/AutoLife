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

// ?꾨왂???앹꽦?????덈뒗 湲곕낯 留ㅻℓ ?좏샇 ???
enum class SignalType {
    NONE,
    STRONG_BUY,
    BUY,
    HOLD,
    SELL,
    STRONG_SELL
};

// 二쇰Ц ????뺤콉 (吏?뺢?/?쒖옣媛/?대갚)
enum class OrderTypePolicy {
    LIMIT,
    MARKET,
    LIMIT_WITH_FALLBACK,
    LIMIT_AGGRESSIVE
};

// 二쇰Ц 諛⑺뼢
enum class OrderSide {
    BUY,
    SELL
};

// 二쇰Ц ?붿껌 DTO
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

// 二쇰Ц 寃곌낵 DTO
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

// ?꾨왂???붿쭊?쇰줈 ?꾨떖?섎뒗 ?쒖? ?좏샇 媛앹껜
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

        bool ev_calibration_enabled = false;
        bool cost_tail_enabled = false;
        bool adaptive_ev_blend_enabled = false;
        bool diagnostics_v2_enabled = false;
        bool edge_regressor_used = false;
        bool ev_calibration_applied = false;
        std::string prob_model_backend = "sgd";
        bool lgbm_ev_affine_enabled = false;
        bool lgbm_ev_affine_applied = false;
        double lgbm_ev_affine_scale = 1.0;
        double lgbm_ev_affine_shift = 0.0;
        std::string edge_semantics = "net";
        bool root_cost_model_enabled_configured = false;
        bool phase3_cost_model_enabled_configured = false;
        bool root_cost_model_enabled_effective = false;
        bool phase3_cost_model_enabled_effective = false;
        bool edge_semantics_guard_violation = false;
        bool edge_semantics_guard_forced_off = false;
        std::string edge_semantics_guard_action = "none";
        double ev_confidence = 1.0;
        double expected_edge_raw_pct = 0.0;
        double expected_edge_calibrated_raw_bps = 0.0;
        double expected_edge_calibrated_pct = 0.0;
        double expected_edge_calibrated_bps = 0.0;
        double expected_edge_used_for_gate_bps = 0.0;
        double cost_entry_pct = 0.0;
        double cost_exit_pct = 0.0;
        double cost_tail_pct = 0.0;
        double cost_used_pct = 0.0;
        double cost_used_bps_estimate = 0.0;
        bool liq_vol_gate_telemetry_valid = false;
        std::string liq_vol_gate_mode = "static";
        double liq_vol_gate_observed = 0.0;
        double liq_vol_gate_threshold_dynamic = 0.0;
        int liq_vol_gate_history_count = 0;
        bool liq_vol_gate_pass = false;
        bool liq_vol_gate_low_conf_triggered = false;
        double liq_vol_gate_quantile_q = 0.0;
        int liq_vol_gate_window_minutes = 0;
        int liq_vol_gate_min_samples_required = 0;
        std::string liq_vol_gate_low_conf_action = "hold";
        bool structure_gate_telemetry_valid = false;
        std::string structure_gate_mode = "static";
        double structure_gate_observed_score = 0.0;
        double structure_gate_threshold_before = 0.0;
        double structure_gate_threshold_after = 0.0;
        bool structure_gate_pass = false;
        bool structure_gate_relax_applied = false;
        double structure_gate_relax_delta = 0.0;
        bool bear_rebound_guard_telemetry_valid = false;
        std::string bear_rebound_guard_mode = "static";
        double bear_rebound_observed = 0.0;
        double bear_rebound_threshold_dynamic = 0.0;
        int bear_rebound_history_count = 0;
        bool bear_rebound_pass = false;
        bool bear_rebound_low_conf_triggered = false;
        double bear_rebound_quantile_q = 0.0;
        int bear_rebound_window_minutes = 0;
        int bear_rebound_min_samples_required = 0;
        std::string bear_rebound_low_conf_action = "hold";
        double adaptive_ev_blend = 0.20;
        double implied_win_runtime = 0.5;
        double required_ev_offset = 0.0;
        double required_ev_offset_trending_add = 0.0;
        double ev_blend_scale = 1.0;
        bool primary_minimums_enabled = false;
        double primary_min_h5_calibrated = 0.48;
        double primary_min_h5_margin = -0.03;
        double primary_min_liquidity_score = 42.0;
        double primary_min_signal_strength = 0.34;
        PrimaryPriorityPolicy primary_priority;
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

// ?꾨왂 硫뷀? ?뺣낫
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

// 紐⑤뱺 ?꾨왂???곕씪???섎뒗 怨듯넻 ?명꽣?섏씠??
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


