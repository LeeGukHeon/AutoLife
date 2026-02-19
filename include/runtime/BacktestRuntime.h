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
        struct EntryFunnelSummary {
            int entry_rounds = 0;
            int skipped_due_to_open_position = 0;
            int no_signal_generated = 0;
            int filtered_out_by_manager = 0;
            int filtered_out_by_policy = 0;
            int no_best_signal = 0;
            int blocked_pattern_gate = 0;
            int blocked_rr_rebalance = 0;
            int blocked_risk_gate = 0;
            int blocked_risk_gate_strategy_ev = 0;
            int blocked_risk_gate_strategy_ev_pre_catastrophic = 0;
            int blocked_risk_gate_strategy_ev_severe_threshold = 0;
            int blocked_risk_gate_strategy_ev_catastrophic_history = 0;
            int blocked_risk_gate_strategy_ev_loss_asymmetry = 0;
            int blocked_risk_gate_strategy_ev_unknown = 0;
            int strategy_ev_pre_cat_observed = 0;
            int strategy_ev_pre_cat_recovery_quality_context = 0;
            int strategy_ev_pre_cat_recovery_evidence_any = 0;
            int strategy_ev_pre_cat_recovery_evidence_relaxed_any = 0;
            int strategy_ev_pre_cat_recovery_evidence_relaxed_recent_regime = 0;
            int strategy_ev_pre_cat_recovery_evidence_relaxed_full_history = 0;
            int strategy_ev_pre_cat_recovery_evidence_for_soften = 0;
            int strategy_ev_pre_cat_recovery_evidence_bridge = 0;
            int strategy_ev_pre_cat_recovery_evidence_bridge_surrogate = 0;
            int strategy_ev_pre_cat_recovery_evidence_hysteresis_override = 0;
            int strategy_ev_pre_cat_quality_hysteresis_override = 0;
            int strategy_ev_pre_cat_quality_context_relaxed_overlap = 0;
            int strategy_ev_pre_cat_quality_fail_regime = 0;
            int strategy_ev_pre_cat_quality_fail_strength = 0;
            int strategy_ev_pre_cat_quality_fail_expected_value = 0;
            int strategy_ev_pre_cat_quality_fail_liquidity = 0;
            int strategy_ev_pre_cat_soften_ready = 0;
            int strategy_ev_pre_cat_soften_candidate_quality_and_evidence = 0;
            int strategy_ev_pre_cat_soften_candidate_non_severe = 0;
            int strategy_ev_pre_cat_soften_candidate_non_hostile = 0;
            int strategy_ev_pre_cat_soften_candidate_rr_ok = 0;
            int strategy_ev_pre_cat_severe_legacy_hits = 0;
            int strategy_ev_pre_cat_severe_composite_hits = 0;
            int strategy_ev_pre_cat_severe_composite_catastrophic_hits = 0;
            int strategy_ev_pre_cat_severe_composite_pressure_axis_hits = 0;
            int strategy_ev_pre_cat_severe_composite_pressure_only_hits = 0;
            int strategy_ev_pre_cat_contextual_severe_downgrade_hits = 0;
            int strategy_ev_pre_cat_severe_active_hits = 0;
            int strategy_ev_pre_cat_softened_contextual = 0;
            int strategy_ev_pre_cat_softened_override = 0;
            int strategy_ev_pre_cat_softened_no_soft_quality_relief = 0;
            int strategy_ev_pre_cat_softened_candidate_rr_failsafe = 0;
            int strategy_ev_pre_cat_softened_pressure_rebound_relief = 0;
            int strategy_ev_pre_cat_negative_history_quarantine_set = 0;
            int strategy_ev_pre_cat_negative_history_quarantine_active = 0;
            int strategy_ev_pre_cat_blocked_severe_sync = 0;
            int strategy_ev_pre_cat_blocked_no_soft_path = 0;
            int blocked_risk_gate_regime = 0;
            int blocked_risk_gate_entry_quality = 0;
            int blocked_risk_gate_entry_quality_rr = 0;
            int blocked_risk_gate_entry_quality_rr_base = 0;
            int blocked_risk_gate_entry_quality_rr_adaptive = 0;
            int blocked_risk_gate_entry_quality_rr_adaptive_history = 0;
            int blocked_risk_gate_entry_quality_rr_adaptive_regime = 0;
            int blocked_risk_gate_entry_quality_rr_adaptive_mixed = 0;
            int blocked_risk_gate_entry_quality_edge = 0;
            int blocked_risk_gate_entry_quality_edge_base = 0;
            int blocked_risk_gate_entry_quality_edge_adaptive = 0;
            int blocked_risk_gate_entry_quality_edge_adaptive_history = 0;
            int blocked_risk_gate_entry_quality_edge_adaptive_regime = 0;
            int blocked_risk_gate_entry_quality_edge_adaptive_mixed = 0;
            int blocked_risk_gate_entry_quality_rr_edge = 0;
            int blocked_risk_gate_entry_quality_rr_edge_base = 0;
            int blocked_risk_gate_entry_quality_rr_edge_adaptive = 0;
            int blocked_risk_gate_entry_quality_rr_edge_adaptive_history = 0;
            int blocked_risk_gate_entry_quality_rr_edge_adaptive_regime = 0;
            int blocked_risk_gate_entry_quality_rr_edge_adaptive_mixed = 0;
            int blocked_risk_gate_entry_quality_invalid_levels = 0;
            int blocked_risk_gate_other = 0;
            int blocked_second_stage_confirmation = 0;
            int blocked_second_stage_confirmation_rr_margin = 0;
            int blocked_second_stage_confirmation_rr_margin_near_miss = 0;
            int blocked_second_stage_confirmation_edge_margin = 0;
            int blocked_second_stage_confirmation_hostile_safety_adders = 0;
            int blocked_second_stage_confirmation_hostile_regime_safety_adders = 0;
            int blocked_second_stage_confirmation_hostile_liquidity_safety_adders = 0;
            int blocked_second_stage_confirmation_hostile_history_safety_adders = 0;
            int blocked_second_stage_confirmation_hostile_history_mild_safety_adders = 0;
            int blocked_second_stage_confirmation_hostile_history_moderate_safety_adders = 0;
            int blocked_second_stage_confirmation_hostile_history_severe_safety_adders = 0;
            int blocked_second_stage_confirmation_hostile_dynamic_tighten_safety_adders = 0;
            int second_stage_rr_margin_near_miss_observed = 0;
            int second_stage_rr_margin_near_miss_relief_applied = 0;
            int second_stage_rr_margin_soft_score_applied = 0;
            int two_head_aggregation_rr_margin_near_miss_head_score_floor_applied = 0;
            int two_head_aggregation_rr_margin_near_miss_floor_relax_applied = 0;
            int two_head_aggregation_rr_margin_near_miss_adaptive_floor_relax_applied = 0;
            int two_head_aggregation_rr_margin_near_miss_surplus_compensation_applied = 0;
            int two_head_aggregation_override_accept = 0;
            int two_head_aggregation_override_accept_rr_margin_near_miss = 0;
            int two_head_aggregation_rr_margin_near_miss_relief_blocked = 0;
            int two_head_aggregation_rr_margin_near_miss_relief_blocked_override_disallowed = 0;
            int two_head_aggregation_rr_margin_near_miss_relief_blocked_entry_floor = 0;
            int two_head_aggregation_rr_margin_near_miss_relief_blocked_second_stage_floor = 0;
            int two_head_aggregation_rr_margin_near_miss_relief_blocked_aggregate_score = 0;
            int two_head_aggregation_blocked = 0;
            int blocked_risk_manager = 0;
            int blocked_min_order_or_capital = 0;
            int blocked_order_sizing = 0;
            int entries_executed = 0;
        };
        struct PreCatFeatureSnapshotBranch {
            int samples = 0;
            int recovery_quality_context_hits = 0;
            int recovery_evidence_hits = 0;
            int recovery_evidence_relaxed_hits = 0;
            int recovery_evidence_hysteresis_hits = 0;
            int non_hostile_regime_hits = 0;
            int severe_active_hits = 0;
            int contextual_downgrade_hits = 0;
            int soften_ready_hits = 0;
            int soften_candidate_rr_ok_hits = 0;
            int severe_axis_ge_threshold_hits = 0;
            double avg_signal_strength = 0.0;
            double avg_signal_expected_value = 0.0;
            double avg_signal_liquidity = 0.0;
            double avg_signal_reward_risk = 0.0;
            double avg_history_expectancy_krw = 0.0;
            double avg_history_profit_factor = 0.0;
            double avg_history_win_rate = 0.0;
            double avg_history_trades = 0.0;
            double avg_history_loss_to_win = 0.0;
            double avg_full_history_pressure = 0.0;
            double avg_recent_history_pressure = 0.0;
            double avg_regime_history_pressure = 0.0;
            double avg_severe_axis_count = 0.0;
        };
        struct PreCatFeatureSnapshot {
            PreCatFeatureSnapshotBranch observed;
            PreCatFeatureSnapshotBranch softened_contextual;
            PreCatFeatureSnapshotBranch softened_override;
            PreCatFeatureSnapshotBranch blocked_severe_sync;
            PreCatFeatureSnapshotBranch blocked_no_soft_path;
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
        std::vector<StrategySummary> strategy_summaries;
        std::vector<PatternSummary> pattern_summaries;
        std::vector<StrategySignalFunnel> strategy_signal_funnel;
        std::vector<StrategyCollectionSummary> strategy_collection_summaries;
        int strategy_collect_exception_count = 0;
        EntryFunnelSummary entry_funnel;
        PreCatFeatureSnapshot pre_cat_feature_snapshot;
        PostEntryRiskTelemetry post_entry_risk_telemetry;
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
    double dynamic_filter_value_ = 0.46; // Self-learning filter (backtest bootstrap)
    int no_entry_streak_candles_ = 0;   // Regime-aware minimum activation helper
    double market_hostility_ewma_ = 0.0;
    int hostile_entry_pause_candles_ = 0;
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
    std::unique_ptr<engine::AdaptivePolicyController> policy_controller_;
    std::unique_ptr<engine::PerformanceStore> performance_store_;
    std::unique_ptr<risk::RiskManager> risk_manager_;

    // Performance Metrics
    double max_balance_;
    double max_drawdown_;
    int total_trades_;
    int winning_trades_;
    Result::EntryFunnelSummary entry_funnel_;
    Result::PreCatFeatureSnapshot pre_cat_feature_snapshot_;
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
    std::map<std::string, int> pre_cat_recovery_hysteresis_hold_by_key_;
    std::map<std::string, int> pre_cat_recovery_bridge_activation_by_key_;
    std::map<std::string, int> pre_cat_no_soft_quality_relief_activation_by_key_;
    std::map<std::string, int> pre_cat_candidate_rr_failsafe_activation_by_key_;
    std::map<std::string, int> pre_cat_pressure_rebound_relief_activation_by_key_;
    std::map<std::string, int> pre_cat_negative_history_quarantine_hold_by_key_;
    int adaptive_stop_update_count_ = 0;
    int adaptive_tp_recalibration_count_ = 0;
    int adaptive_partial_ratio_samples_ = 0;
    double adaptive_partial_ratio_sum_ = 0.0;
    std::array<int, 5> adaptive_partial_ratio_histogram_{{0, 0, 0, 0, 0}};

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
    
    // Self-learning
    void updateDynamicFilter();
};

} // namespace backtest
} // namespace autolife
