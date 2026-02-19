#pragma once

#include <string>
#include <vector>

namespace autolife {
namespace engine {

enum class TradingMode {
    LIVE,
    PAPER,
    BACKTEST
};

struct EngineConfig {
    TradingMode mode;
    double initial_capital;

    int scan_interval_seconds;
    long long min_volume_krw;

    int max_positions;
    int max_daily_trades;
    double max_drawdown;
    double max_exposure_pct = 0.85;
    double max_daily_loss_pct = 0.05;
    double risk_per_trade_pct = 0.005;
    double max_slippage_pct = 0.003;

    double max_daily_loss_krw = 50000.0;
    double max_order_krw = 500000.0;
    double min_order_krw = 5000.0;
    // Small-account lot control (prevents oversized jumps from min-order constraints)
    double small_account_tier1_capital_krw = 60000.0;
    double small_account_tier2_capital_krw = 100000.0;
    double small_account_tier1_max_order_pct = 0.20;
    double small_account_tier2_max_order_pct = 0.15;
    // Extra reserve for entry fee/rounding safety (e.g., 0.001 = 0.1%)
    double order_fee_reserve_pct = 0.001;
    int max_new_orders_per_scan = 2;
    bool dry_run = false;
    bool allow_live_orders = false;
    bool enable_live_mtf_dataset_capture = true;
    int live_mtf_dataset_capture_interval_seconds = 300;
    std::string live_mtf_dataset_capture_output_dir = "data/backtest_real_live";
    std::vector<std::string> live_mtf_dataset_capture_timeframes = {
        "1m", "5m", "15m", "1h", "4h", "1d"
    };
    double min_expected_edge_pct = 0.0010; // 0.10% after cost
    double min_reward_risk = 1.20;         // TP/SL ratio
    double min_rr_weak_signal = 1.80;      // dynamic RR target for weak signals
    double min_rr_strong_signal = 1.25;    // dynamic RR target for strong signals
    int min_strategy_trades_for_ev = 30;   // minimum samples before EV gating
    double min_strategy_expectancy_krw = 0.0;
    double min_strategy_profit_factor = 1.00;
    bool enable_strategy_ev_pre_cat_soften_non_severe = false;
    bool enable_strategy_ev_pre_cat_relaxed_recovery_evidence = false;
    bool enable_strategy_ev_pre_cat_relaxed_recovery_full_history_anchor = false;
    bool enable_strategy_ev_pre_cat_recovery_evidence_hysteresis = false;
    int strategy_ev_pre_cat_recovery_evidence_hysteresis_hold_steps = 12;
    int strategy_ev_pre_cat_recovery_evidence_hysteresis_min_trades = 8;
    bool enable_strategy_ev_pre_cat_recovery_quality_hysteresis_relief = false;
    double strategy_ev_pre_cat_recovery_quality_hysteresis_min_strength = 0.67;
    double strategy_ev_pre_cat_recovery_quality_hysteresis_min_expected_value = 0.00060;
    double strategy_ev_pre_cat_recovery_quality_hysteresis_min_liquidity = 56.0;
    int strategy_ev_pre_cat_relaxed_recovery_min_trades = 8;
    double strategy_ev_pre_cat_relaxed_recovery_expectancy_gap_krw = 2.0;
    double strategy_ev_pre_cat_relaxed_recovery_profit_factor_gap = 0.12;
    double strategy_ev_pre_cat_relaxed_recovery_min_win_rate = 0.48;
    // Recovery evidence bridge:
    // when strict recent/regime recovery is unavailable, allow bounded
    // relaxed evidence usage under per-key budgets and severe-pressure caps.
    bool enable_strategy_ev_pre_cat_recovery_evidence_bridge = false;
    bool enable_strategy_ev_pre_cat_recovery_evidence_bridge_surrogate = false;
    int strategy_ev_pre_cat_recovery_evidence_bridge_max_strategy_trades = 24;
    double strategy_ev_pre_cat_recovery_evidence_bridge_max_full_history_pressure = 0.96;
    int strategy_ev_pre_cat_recovery_evidence_bridge_max_severe_axis_count = 2;
    int strategy_ev_pre_cat_recovery_evidence_bridge_max_activations_per_key = 1;
    // Pattern-targeted rebound relief for "high-quality + high full-history pressure lock".
    // This path is strictly bounded and intended for controlled experiments (default OFF).
    bool enable_strategy_ev_pre_cat_pressure_rebound_relief = false;
    int strategy_ev_pre_cat_pressure_rebound_relief_max_strategy_trades = 28;
    double strategy_ev_pre_cat_pressure_rebound_relief_min_strength = 0.74;
    double strategy_ev_pre_cat_pressure_rebound_relief_min_expected_value = 0.00110;
    double strategy_ev_pre_cat_pressure_rebound_relief_min_liquidity = 60.0;
    double strategy_ev_pre_cat_pressure_rebound_relief_min_reward_risk = 1.25;
    double strategy_ev_pre_cat_pressure_rebound_relief_min_full_history_pressure = 0.95;
    double strategy_ev_pre_cat_pressure_rebound_relief_max_recent_history_pressure = 0.78;
    double strategy_ev_pre_cat_pressure_rebound_relief_max_regime_history_pressure = 0.80;
    int strategy_ev_pre_cat_pressure_rebound_relief_max_severe_axis_count = 2;
    int strategy_ev_pre_cat_pressure_rebound_relief_max_activations_per_key = 1;
    // Negative-history quarantine:
    // after blocked-no-soft lock with strongly negative full-history profile,
    // temporarily tighten the same strategy+regime key to reduce repeated damage.
    bool enable_strategy_ev_pre_cat_negative_history_quarantine = false;
    int strategy_ev_pre_cat_negative_history_quarantine_hold_steps = 6;
    double strategy_ev_pre_cat_negative_history_quarantine_min_full_history_pressure = 0.95;
    double strategy_ev_pre_cat_negative_history_quarantine_max_history_pf = 0.45;
    double strategy_ev_pre_cat_negative_history_quarantine_max_history_expectancy_krw = -10.0;
    bool enable_strategy_ev_pre_cat_relaxed_severe_gate = false;
    int strategy_ev_pre_cat_relaxed_severe_min_trades = 18;
    double strategy_ev_pre_cat_relaxed_severe_pressure_threshold = 0.78;
    bool enable_strategy_ev_pre_cat_composite_severe_model = false;
    double strategy_ev_pre_cat_composite_pressure_threshold = 0.84;
    int strategy_ev_pre_cat_composite_min_critical_signals = 2;
    bool enable_strategy_ev_pre_cat_sync_guard = false;
    bool enable_strategy_ev_pre_cat_contextual_severe_downgrade = false;
    double strategy_ev_pre_cat_contextual_severe_max_pressure = 0.90;
    int strategy_ev_pre_cat_contextual_severe_max_axis_count = 2;
    // Guarded bootstrap relief for blocked-no-soft path:
    // allows limited override when recovery evidence is not yet available,
    // but signal quality is high and severe pre-cat is not active.
    bool enable_strategy_ev_pre_cat_no_soft_quality_relief = false;
    int strategy_ev_pre_cat_no_soft_quality_relief_max_strategy_trades = 20;
    double strategy_ev_pre_cat_no_soft_quality_relief_min_strength = 0.72;
    double strategy_ev_pre_cat_no_soft_quality_relief_min_expected_value = 0.00100;
    double strategy_ev_pre_cat_no_soft_quality_relief_min_liquidity = 58.0;
    double strategy_ev_pre_cat_no_soft_quality_relief_min_reward_risk = 1.30;
    double strategy_ev_pre_cat_no_soft_quality_relief_max_full_history_pressure = 1.00;
    int strategy_ev_pre_cat_no_soft_quality_relief_max_severe_axis_count = 2;
    int strategy_ev_pre_cat_no_soft_quality_relief_max_activations_per_key = 1;
    // Guarded failsafe path for candidate_rr_ok when soften_non_severe is OFF.
    // Keeps activation tightly budgeted to avoid broad relaxation regressions.
    bool enable_strategy_ev_pre_cat_candidate_rr_failsafe = false;
    int strategy_ev_pre_cat_candidate_rr_failsafe_max_strategy_trades = 18;
    double strategy_ev_pre_cat_candidate_rr_failsafe_min_strength = 0.73;
    double strategy_ev_pre_cat_candidate_rr_failsafe_min_expected_value = 0.00095;
    double strategy_ev_pre_cat_candidate_rr_failsafe_min_liquidity = 59.0;
    double strategy_ev_pre_cat_candidate_rr_failsafe_min_reward_risk = 1.30;
    double strategy_ev_pre_cat_candidate_rr_failsafe_max_full_history_pressure = 0.96;
    int strategy_ev_pre_cat_candidate_rr_failsafe_max_severe_axis_count = 2;
    int strategy_ev_pre_cat_candidate_rr_failsafe_max_activations_per_key = 1;
    double strategy_ev_pre_cat_unsynced_override_min_strength = 0.74;
    double strategy_ev_pre_cat_unsynced_override_min_expected_value = 0.00085;
    double strategy_ev_pre_cat_unsynced_override_min_liquidity = 60.0;
    double strategy_ev_pre_cat_unsynced_override_min_reward_risk = 1.28;
    double strategy_ev_pre_cat_unsynced_override_max_full_history_pressure = 1.00;
    int strategy_ev_pre_cat_unsynced_override_max_severe_axis_count = 4;
    bool avoid_high_volatility = true;
    bool avoid_trending_down = true;
    bool enable_core_plane_bridge = true;
    bool enable_core_policy_plane = true;
    bool enable_core_risk_plane = true;
    bool enable_core_execution_plane = true;
    bool use_strategy_alpha_head_mode = false;
    double hostility_ewma_alpha = 0.14;
    double hostility_hostile_threshold = 0.62;
    double hostility_severe_threshold = 0.82;
    double hostility_extreme_threshold = 0.88;
    int hostility_pause_scans = 4;
    int hostility_pause_scans_extreme = 6;
    int hostility_pause_recent_sample_min = 10;
    double hostility_pause_recent_expectancy_krw = 0.0;
    double hostility_pause_recent_win_rate = 0.40;
    int backtest_hostility_pause_candles = 36;
    int backtest_hostility_pause_candles_extreme = 60;
    // Entry-quality topology relief: allow limited adaptive-gate relief only
    // for strong/high-quality signals, while reducing position size.
    bool enable_entry_quality_adaptive_relief = true;
    double entry_quality_adaptive_relief_rr_max_gap = 0.08;
    double entry_quality_adaptive_relief_edge_max_gap = 0.00012;
    double entry_quality_adaptive_relief_min_signal_strength = 0.68;
    double entry_quality_adaptive_relief_min_expected_value = 0.00035;
    double entry_quality_adaptive_relief_min_liquidity_score = 58.0;
    int entry_quality_adaptive_relief_min_strategy_trades = 16;
    double entry_quality_adaptive_relief_min_strategy_win_rate = 0.47;
    double entry_quality_adaptive_relief_min_strategy_profit_factor = 0.98;
    bool entry_quality_adaptive_relief_block_high_stress_regime = true;
    double entry_quality_adaptive_relief_position_scale = 0.80;
    // Second-stage history safety: keep severe-history pressure bounded while
    // allowing limited quality-conditioned relief in non-hostile regimes.
    double second_stage_history_safety_severe_scale = 1.00;
    bool enable_second_stage_history_safety_severe_relief = true;
    double second_stage_history_safety_relief_max_scale = 0.45;
    int second_stage_history_safety_relief_min_strategy_trades = 12;
    double second_stage_history_safety_relief_min_signal_strength = 0.68;
    double second_stage_history_safety_relief_min_expected_value = 0.00045;
    double second_stage_history_safety_relief_min_liquidity_score = 58.0;
    bool second_stage_history_safety_relief_block_hostile_regime = false;
    // Two-head aggregation for entry acceptance:
    // combine entry-quality and second-stage head scores so one near-miss head
    // can be compensated by the other under bounded conditions.
    bool enable_two_head_entry_second_stage_aggregation = true;
    double two_head_entry_quality_weight = 0.55;
    double two_head_second_stage_weight = 0.45;
    double two_head_min_entry_quality_score = 0.90;
    double two_head_min_second_stage_score = 0.88;
    double two_head_min_aggregate_score = 0.98;
    bool two_head_aggregation_block_high_stress_regime = false;
    int two_head_aggregation_min_strategy_trades = 8;
    bool enable_two_head_rr_margin_near_miss_floor_relax = false;
    double two_head_rr_margin_near_miss_second_stage_floor_relax = 0.06;
    double two_head_rr_margin_near_miss_aggregate_floor_relax = 0.03;
    // Adaptive near-miss floor-relax:
    // scales floor relaxation by quality/gap tightness to reduce blunt tuning.
    bool enable_two_head_rr_margin_near_miss_adaptive_floor_relax = false;
    double two_head_rr_margin_near_miss_adaptive_floor_relax_min_activation = 0.45;
    double two_head_rr_margin_near_miss_adaptive_floor_relax_max_second_stage = 0.08;
    double two_head_rr_margin_near_miss_adaptive_floor_relax_max_aggregate = 0.04;
    double two_head_rr_margin_near_miss_adaptive_floor_relax_quality_weight = 0.55;
    double two_head_rr_margin_near_miss_adaptive_floor_relax_gap_weight = 0.45;
    // Near-miss surplus compensation:
    // keep floors unchanged, but allow bounded compensation only when
    // entry-head surplus and edge quality are strong.
    bool enable_two_head_rr_margin_near_miss_surplus_compensation = false;
    double two_head_rr_margin_near_miss_surplus_min_entry_surplus = 0.05;
    double two_head_rr_margin_near_miss_surplus_min_edge_score = 1.03;
    double two_head_rr_margin_near_miss_surplus_max_second_stage_deficit = 0.05;
    double two_head_rr_margin_near_miss_surplus_max_aggregate_deficit = 0.04;
    double two_head_rr_margin_near_miss_surplus_entry_weight = 0.35;
    double two_head_rr_margin_near_miss_surplus_max_aggregate_bonus = 0.05;
    // RR-margin near-miss relief:
    // allow a bounded score bump only when second-stage RR margin miss is small
    // and overall signal quality/history conditions are strong.
    bool enable_second_stage_rr_margin_near_miss_relief = true;
    double second_stage_rr_margin_near_miss_max_gap = 0.02;
    double second_stage_rr_margin_near_miss_min_signal_strength = 0.70;
    double second_stage_rr_margin_near_miss_min_expected_value = 0.00055;
    double second_stage_rr_margin_near_miss_min_liquidity_score = 60.0;
    int second_stage_rr_margin_near_miss_min_strategy_trades = 12;
    bool second_stage_rr_margin_near_miss_block_high_stress_regime = true;
    double second_stage_rr_margin_near_miss_score_boost = 0.08;
    // Soft-score path for slight negative RR margin.
    // Keeps second-stage head score from collapsing to zero when miss is small.
    bool enable_second_stage_rr_margin_soft_score = false;
    double second_stage_rr_margin_soft_score_max_gap = 0.02;
    double second_stage_rr_margin_soft_score_floor = 0.70;
    double second_stage_rr_margin_soft_score_gap_tightness_weight = 0.20;
    // Optional floor for near-miss second-stage head score.
    // Helps avoid score collapse when RR margin is slightly below zero but
    // quality/gap context is still strong.
    bool enable_second_stage_rr_margin_near_miss_head_score_floor = false;
    double second_stage_rr_margin_near_miss_head_score_floor_base = 0.0;
    double second_stage_rr_margin_near_miss_head_score_floor_quality_weight = 0.0;
    double second_stage_rr_margin_near_miss_head_score_floor_gap_weight = 0.0;
    double second_stage_rr_margin_near_miss_head_score_floor_max = 1.0;

    std::vector<std::string> enabled_strategies;

    EngineConfig()
        : mode(TradingMode::PAPER)
        , initial_capital(50000)
        , scan_interval_seconds(60)
        , min_volume_krw(5000000000LL)
        , max_positions(10)
        , max_daily_trades(50)
        , max_drawdown(0.10)
    {}
};

} // namespace engine
} // namespace autolife
