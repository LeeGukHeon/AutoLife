#pragma once

#include <vector>
#include <string>
#include <map>
#include <array>
#include "common/Types.h"
#include "common/Config.h"
#include "backtest/DataHistory.h"
#include "strategy/StrategyManager.h"
#include "analytics/RegimeDetector.h"
#include "analytics/ProbabilisticRuntimeModel.h"
#include "risk/RiskManager.h"
#include "engine/AdaptivePolicyController.h"
#include "engine/PerformanceStore.h"
#include "network/UpbitHttpClient.h"
#include <memory>

namespace autolife {
namespace backtest {

class BacktestEngine {
public:
    BacktestEngine();

    // Initialize engine with configuration
    void init(const Config& config);

    // Load historical data
    void loadData(const std::string& file_path);

    // Run the backtest simulation
    void run();

    // Get results
    struct Result {
        struct StrategySummary {
            std::string strategy_name;
            int total_trades = 0;
            int winning_trades = 0;
            int losing_trades = 0;
            double win_rate = 0.0;
            double total_profit = 0.0;
            double avg_win_krw = 0.0;
            double avg_loss_krw = 0.0;
            double profit_factor = 0.0;
        };
        struct PatternSummary {
            std::string strategy_name;
            std::string entry_archetype;
            std::string regime;
            std::string volatility_bucket;
            std::string liquidity_bucket;
            std::string strength_bucket;
            std::string expected_value_bucket;
            std::string reward_risk_bucket;
            int total_trades = 0;
            int winning_trades = 0;
            int losing_trades = 0;
            double win_rate = 0.0;
            double total_profit = 0.0;
            double avg_profit_krw = 0.0;
            double profit_factor = 0.0;
        };
        struct TradeHistorySample {
            std::string market;
            std::string strategy_name;
            std::string entry_archetype;
            std::string regime;
            std::string exit_reason;
            long long entry_time = 0;
            long long exit_time = 0;
            double entry_price = 0.0;
            double exit_price = 0.0;
            double quantity = 0.0;
            double holding_minutes = 0.0;
            double profit_loss_krw = 0.0;
            double profit_loss_pct = 0.0;
            double fee_paid_krw = 0.0;
            double signal_filter = 0.0;
            double signal_strength = 0.0;
            double liquidity_score = 0.0;
            double volatility = 0.0;
            double expected_value = 0.0;
            double reward_risk_ratio = 0.0;
            bool probabilistic_runtime_applied = false;
            double probabilistic_h5_calibrated = 0.0;
            double probabilistic_h5_margin = 0.0;
        };
        struct EntryFunnelSummary {
            int entry_rounds = 0;
            int skipped_due_to_open_position = 0;
            int no_signal_generated = 0;
            int filtered_out_by_manager = 0;
            int filtered_out_by_policy = 0;
            int reject_expected_edge_negative_count = 0;
            int reject_regime_entry_disabled_count = 0;
            std::map<std::string, int> reject_regime_entry_disabled_by_regime;
            bool regime_entry_disable_enabled = false;
            std::map<std::string, bool> regime_entry_disable;
            int no_best_signal = 0;
            int blocked_pattern_gate = 0;
            int blocked_rr_rebalance = 0;
            int blocked_risk_gate = 0;
            int blocked_risk_gate_strategy_ev = 0;
            int blocked_risk_gate_strategy_ev_severe_threshold = 0;
            int blocked_risk_gate_strategy_ev_catastrophic_history = 0;
            int blocked_risk_gate_strategy_ev_loss_asymmetry = 0;
            int blocked_risk_gate_strategy_ev_unknown = 0;
            int blocked_risk_gate_regime = 0;
            int blocked_risk_gate_entry_quality_invalid_levels = 0;
            int blocked_risk_gate_other = 0;
            int blocked_risk_manager = 0;
            int blocked_min_order_or_capital = 0;
            int blocked_order_sizing = 0;
            int entries_executed = 0;
        };
        struct RangingShadowSummary {
            int shadow_count_total = 0;
            std::map<std::string, int> shadow_count_by_regime;
            std::map<std::string, int> shadow_count_by_market;
            int shadow_would_pass_frontier_count = 0;
            int shadow_would_pass_manager_count = 0;
            int shadow_would_pass_execution_guard_count = 0;
            int shadow_edge_neg_count = 0;
            int shadow_edge_pos_count = 0;
        };
        struct ShadowFunnelSummary {
            int rounds = 0;
            int primary_generated_signals = 0;
            int primary_after_manager_filter = 0;
            int shadow_after_manager_filter = 0;
            int primary_after_policy_filter = 0;
            int shadow_after_policy_filter = 0;
            int primary_best_signal_available = 0;
            int shadow_best_signal_available = 0;
            int supply_improved_rounds = 0;
            double manager_supply_lift_sum = 0.0;
            double policy_supply_lift_sum = 0.0;
            double avg_manager_supply_lift = 0.0;
            double avg_policy_supply_lift = 0.0;
        };
        struct StrategySignalFunnel {
            std::string strategy_name;
            int generated_signals = 0;
            int selected_best = 0;
            int blocked_by_risk_manager = 0;
            int entries_executed = 0;
        };
        struct StrategyCollectionSummary {
            std::string strategy_name;
            int skipped_disabled = 0;
            int no_signal = 0;
            int generated = 0;
        };
        struct PostEntryRiskTelemetry {
            int adaptive_stop_updates = 0;
            int adaptive_tp_recalibration_updates = 0;
            int adaptive_partial_ratio_samples = 0;
            double adaptive_partial_ratio_sum = 0.0;
            std::array<int, 5> adaptive_partial_ratio_histogram{{0, 0, 0, 0, 0}};
        };
        struct Phase4CorrelationNearCapSample {
            std::string market;
            std::string cluster;
            double exposure_current = 0.0;
            double cluster_cap_value = 0.0;
            double candidate_position_size = 0.0;
            double projected_exposure = 0.0;
            double headroom_before = 0.0;
            bool rejected_by_cluster_cap = false;
        };
        struct Phase4CorrelationPenaltyScoreSample {
            std::string market;
            std::string cluster;
            double score_before_penalty = 0.0;
            double penalty = 0.0;
            double score_after_penalty = 0.0;
            bool rejected_by_penalty = false;
        };
        struct Phase4ClusterCapDebugSample {
            std::string market;
            std::string cluster;
            double cluster_exposure_before = 0.0;
            double candidate_notional_fraction = 0.0;
            double cluster_cap_value = 0.0;
            bool would_exceed = false;
            double after_accept_cluster_exposure = 0.0;
        };
        struct Phase4PortfolioDiagnostics {
            bool enabled = false;
            bool phase4_portfolio_allocator_enabled = false;
            bool phase4_correlation_control_enabled = false;
            bool phase4_risk_budget_enabled = false;
            bool phase4_drawdown_governor_enabled = false;
            bool phase4_execution_aware_sizing_enabled = false;
            bool phase4_portfolio_diagnostics_enabled = false;
            int allocator_top_k = 0;
            double allocator_min_score = 0.0;
            double risk_budget_per_market_cap = 0.0;
            double risk_budget_gross_cap = 0.0;
            double risk_budget_cap = 0.0;
            std::map<std::string, double> risk_budget_regime_multipliers;
            int regime_budget_multiplier_applied_count = 0;
            std::map<std::string, int> regime_budget_multiplier_count_by_regime;
            std::map<std::string, double> regime_budget_multiplier_sum_by_regime;
            std::map<std::string, double> regime_budget_multiplier_avg_by_regime;
            double drawdown_current = 0.0;
            double drawdown_budget_multiplier = 1.0;
            double correlation_default_cluster_cap = 0.0;
            int correlation_market_cluster_count = 0;
            double execution_liquidity_low_threshold = 0.0;
            double execution_liquidity_mid_threshold = 0.0;
            double execution_min_position_size = 0.0;
            int candidates_total = 0;
            int selected_total = 0;
            int rejected_by_budget = 0;
            int rejected_by_cluster_cap = 0;
            int rejected_by_correlation_penalty = 0;
            int cluster_cap_skips_count = 0;
            int cluster_cap_would_exceed_count = 0;
            int cluster_exposure_update_count = 0;
            int rejected_by_execution_cap = 0;
            int rejected_by_drawdown_governor = 0;
            int candidate_snapshot_total = 0;
            int candidate_snapshot_sampled = 0;
            double selection_rate = 0.0;
            std::string correlation_constraint_apply_stage;
            std::string correlation_constraint_unit;
            int correlation_cluster_eval_count = 0;
            int correlation_cluster_near_cap_count = 0;
            int correlation_penalty_applied_count = 0;
            double correlation_penalty_avg = 0.0;
            double correlation_penalty_max = 0.0;
            std::map<std::string, double> correlation_cluster_exposure_current;
            std::map<std::string, double> correlation_cluster_cap_values;
            std::vector<Phase4CorrelationNearCapSample> correlation_near_cap_candidates;
            std::vector<Phase4CorrelationPenaltyScoreSample> correlation_penalty_score_samples;
            std::vector<Phase4ClusterCapDebugSample> cluster_cap_debug_trace_samples;
        };
        struct Phase4CandidateSnapshotSample {
            std::string market;
            long long decision_time = 0;
            std::string strategy_name;
            std::string regime;
            std::string volatility_bucket;
            std::string liquidity_bucket;
            double expected_edge_after_cost_pct = 0.0;
            double expected_edge_tail_after_cost_pct = 0.0;
            double margin = 0.0;
            double prob_confidence = 0.0;
            double ev_confidence = 0.0;
            double signal_strength = 0.0;
            double liquidity_score = 0.0;
            double volatility = 0.0;
            double entry_cost_pct = 0.0;
            double exit_cost_pct = 0.0;
            double tail_cost_pct = 0.0;
            std::string cost_mode = "mean_mode";
            bool edge_regressor_used = false;
            bool selected = false;
            bool has_open_position = false;
            double open_position_qty = 0.0;
            double open_position_unrealized_pnl = 0.0;
            double open_position_time_in_position_minutes = 0.0;
        };

        double final_balance;
        double total_profit;
        double max_drawdown;
        int total_trades;
        int winning_trades;
        int losing_trades;
        double win_rate;
        double avg_win_krw;
        double avg_loss_krw;
        double profit_factor;
        double expectancy_krw;
        double avg_holding_minutes = 0.0;
        double avg_fee_krw = 0.0;
        int intrabar_stop_tp_collision_count = 0;
        std::map<std::string, int> exit_reason_counts;
        std::map<std::string, int> entry_rejection_reason_counts;
        std::map<std::string, int> no_signal_pattern_counts;
        std::map<std::string, int> entry_quality_edge_gap_buckets;
        std::map<std::string, int> intrabar_collision_by_strategy;
        RangingShadowSummary ranging_shadow;
        std::vector<StrategySummary> strategy_summaries;
        std::vector<PatternSummary> pattern_summaries;
        std::vector<TradeHistorySample> trade_history_samples;
        std::vector<StrategySignalFunnel> strategy_signal_funnel;
        std::vector<StrategyCollectionSummary> strategy_collection_summaries;
        int strategy_collect_exception_count = 0;
        EntryFunnelSummary entry_funnel;
        ShadowFunnelSummary shadow_funnel;
        PostEntryRiskTelemetry post_entry_risk_telemetry;
        Phase4PortfolioDiagnostics phase4_portfolio_diagnostics;
        std::vector<Phase4CandidateSnapshotSample> phase4_candidate_snapshot_samples;
        int strategyless_position_checks = 0;
        int strategyless_runtime_archetype_checks = 0;
        int strategyless_risk_exit_signals = 0;
        int strategyless_current_stop_hits = 0;
        int strategyless_current_tp1_hits = 0;
        int strategyless_current_tp2_hits = 0;
    };
    Result getResult() const;

private:

    std::vector<Candle> history_data_;
    engine::EngineConfig engine_config_;
    
    // Account State
    double balance_krw_;
    double balance_asset_; // Simplified for single asset for now
    std::string market_name_ = "KRW-BTC"; // Default for backtest
    
    // Execution State
    std::vector<Candle> current_candles_;
    std::map<std::string, std::vector<Candle>> loaded_tf_candles_;
    std::map<std::string, size_t> loaded_tf_cursors_;
    bool strict_live_equivalent_data_parity_ = false;
    int strict_data_parity_skip_count_ = 0;
    double market_hostility_ewma_ = 0.0;
    double recent_best_ask_price_ = 0.0;
    long long recent_best_ask_timestamp_ms_ = 0;
    bool was_open_position_prev_candle_ = false; // Count open-position skip once per holding episode
    struct PendingBacktestOrder {
        Order order;
        double requested_price = 0.0;
        long long enqueued_at_ms = 0;
    };
    std::vector<PendingBacktestOrder> pending_orders_;
    long long backtest_order_seq_ = 0;
    
    // Components
    std::shared_ptr<network::UpbitHttpClient> http_client_; // Mockable
    std::unique_ptr<strategy::StrategyManager> strategy_manager_;
    std::unique_ptr<analytics::RegimeDetector> regime_detector_;
    analytics::ProbabilisticRuntimeModel probabilistic_runtime_model_;
    bool probabilistic_runtime_model_loaded_ = false;
    std::unique_ptr<engine::AdaptivePolicyController> policy_controller_;
    std::unique_ptr<engine::PerformanceStore> performance_store_;
    std::unique_ptr<risk::RiskManager> risk_manager_;

    // Performance Metrics
    double max_balance_;
    double max_drawdown_;
    int total_trades_;
    int winning_trades_;
    Result::EntryFunnelSummary entry_funnel_;
    std::map<std::string, int> strategy_generated_counts_;
    std::map<std::string, int> strategy_selected_best_counts_;
    std::map<std::string, int> strategy_blocked_by_risk_manager_counts_;
    std::map<std::string, int> strategy_entries_executed_counts_;
    std::map<std::string, int> strategy_skipped_disabled_counts_;
    std::map<std::string, int> strategy_no_signal_counts_;
    int strategy_collect_exception_count_ = 0;
    std::map<std::string, int> entry_rejection_reason_counts_;
    std::map<std::string, int> no_signal_pattern_counts_;
    std::map<std::string, int> entry_quality_edge_gap_buckets_;
    int intrabar_stop_tp_collision_count_ = 0;
    std::map<std::string, int> intrabar_collision_by_strategy_;
    int adaptive_stop_update_count_ = 0;
    int adaptive_tp_recalibration_count_ = 0;
    int adaptive_partial_ratio_samples_ = 0;
    double adaptive_partial_ratio_sum_ = 0.0;
    std::array<int, 5> adaptive_partial_ratio_histogram_{{0, 0, 0, 0, 0}};
    int strategyless_position_checks_ = 0;
    int strategyless_runtime_archetype_checks_ = 0;
    int strategyless_risk_exit_signals_ = 0;
    int strategyless_current_stop_hits_ = 0;
    int strategyless_current_tp1_hits_ = 0;
    int strategyless_current_tp2_hits_ = 0;
    Result::RangingShadowSummary ranging_shadow_summary_;
    Result::ShadowFunnelSummary shadow_funnel_;
    Result::Phase4PortfolioDiagnostics phase4_portfolio_diagnostics_;
    std::vector<Result::Phase4CandidateSnapshotSample> phase4_candidate_snapshot_samples_;
    size_t current_candle_index_ = 0;

    // Simulation Methods
    void processCandle(const Candle& candle);
    void checkOrders(const Candle& candle);
    void executeOrder(const Order& order, double price);

    void loadCompanionTimeframes(const std::string& file_path);
    std::vector<Candle> getTimeframeCandles(
        const std::string& timeframe,
        long long current_timestamp,
        int fallback_minutes,
        size_t max_bars
    );
    static void normalizeTimestampsToMs(std::vector<Candle>& candles);
    static std::vector<Candle> aggregateCandles(
        const std::vector<Candle>& candles_1m,
        int timeframe_minutes,
        size_t max_bars
    );
    static long long toMsTimestamp(long long ts);
    
};

} // namespace backtest
} // namespace autolife
