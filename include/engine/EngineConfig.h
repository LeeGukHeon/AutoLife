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
    // Live signal quality guard:
    // use only confirmed candles (drop potentially in-progress latest bar).
    bool use_confirmed_candle_only_for_signals = true;
    // Live entry veto: one final microstructure check just before buy submit.
    bool enable_realtime_entry_veto = true;
    int realtime_entry_veto_tracking_window_seconds = 90;
    double realtime_entry_veto_max_drop_pct = 0.0035;            // 0.35%
    double realtime_entry_veto_max_spread_pct = 0.0030;          // 0.30%
    double realtime_entry_veto_min_orderbook_imbalance = -0.35;  // [-1, +1]
    // Live warm-up: keep scanning (cache build) but block new entries until
    // minimum scan count and parity-ready ratio are satisfied.
    bool enable_live_cache_warmup = true;
    int live_cache_warmup_min_scans = 5;
    double live_cache_warmup_min_ready_ratio = 0.75;
    bool enable_live_mtf_dataset_capture = true;
    int live_mtf_dataset_capture_interval_seconds = 300;
    std::string live_mtf_dataset_capture_output_dir = "data/backtest_real_live";
    std::vector<std::string> live_mtf_dataset_capture_timeframes = {
        "1m", "5m", "15m", "1h", "4h", "1d"
    };
    // Probabilistic runtime overlay (shared in live/backtest).
    bool enable_probabilistic_runtime_model = true;
    std::string probabilistic_runtime_bundle_path = "config/model/probabilistic_runtime_bundle_v1.json";
    bool probabilistic_runtime_hard_gate = false;
    double probabilistic_runtime_hard_gate_margin = -0.08;
    double probabilistic_runtime_score_weight = 0.12;
    double probabilistic_runtime_expected_edge_weight = 0.00030;
    // Probabilistic-primary routing:
    // make probabilistic inference the first-class decision signal across
    // scan -> signal gating -> entry sizing.
    bool probabilistic_runtime_primary_mode = true;
    bool probabilistic_runtime_scan_prefilter_enabled = true;
    double probabilistic_runtime_scan_prefilter_margin = -0.10;
    double probabilistic_runtime_strength_blend = 0.45;
    double probabilistic_runtime_position_scale_weight = 0.35;
    bool probabilistic_runtime_online_learning_enabled = true;
    int probabilistic_runtime_online_learning_window = 80;
    int probabilistic_runtime_online_learning_min_samples = 12;
    double probabilistic_runtime_online_learning_max_margin_bias = 0.02;
    double probabilistic_runtime_online_learning_strength_gain = 0.35;
    // EXT-53 (default OFF): uncertainty-aware sizing from ensemble disagreement.
    bool probabilistic_uncertainty_ensemble_enabled = false;
    std::string probabilistic_uncertainty_size_mode = "linear"; // linear | exp
    double probabilistic_uncertainty_u_max = 0.06;
    double probabilistic_uncertainty_exp_k = 8.0;
    double probabilistic_uncertainty_min_scale = 0.10;
    bool probabilistic_uncertainty_skip_when_high = false;
    double probabilistic_uncertainty_skip_u = 0.12;
    // EXT-55 (default OFF): explicit probabilistic runtime regime policy.
    bool probabilistic_regime_spec_enabled = false;
    int probabilistic_regime_volatility_window = 48;
    int probabilistic_regime_drawdown_window = 36;
    double probabilistic_regime_volatile_zscore = 1.20;
    double probabilistic_regime_hostile_zscore = 2.00;
    double probabilistic_regime_volatile_drawdown_speed_bps = 3.0;
    double probabilistic_regime_hostile_drawdown_speed_bps = 8.0;
    bool probabilistic_regime_enable_btc_correlation_shock = false;
    int probabilistic_regime_correlation_window = 48;
    double probabilistic_regime_correlation_shock_threshold = 1.20;
    bool probabilistic_regime_hostile_block_new_entries = true;
    double probabilistic_regime_volatile_threshold_add = 0.010;
    double probabilistic_regime_hostile_threshold_add = 0.030;
    double probabilistic_regime_volatile_size_multiplier = 0.50;
    double probabilistic_regime_hostile_size_multiplier = 0.20;
    double probabilistic_primary_promotion_min_margin = -0.02;
    double probabilistic_primary_promotion_min_calibrated = 0.47;
    double probabilistic_primary_promotion_max_strength_gap = 0.12;
    double probabilistic_primary_promotion_max_ev_gap = 0.00045;
    // Probabilistic-primary minimum conditions:
    // rank by probabilistic confidence first, but enforce these baseline
    // operating constraints before entry.
    double probabilistic_primary_min_h5_calibrated = 0.48;
    double probabilistic_primary_min_h5_margin = -0.03;
    double probabilistic_primary_min_liquidity_score = 42.0;
    double probabilistic_primary_min_signal_strength = 0.34;
    // Candidate supply controls (runtime primary path).
    bool foundation_signal_supply_fallback_enabled = true;
    bool manager_soft_queue_enabled = true;
    double manager_soft_queue_position_scale = 0.70;
    double manager_soft_queue_max_strength_gap = 0.05;
    double manager_soft_queue_max_ev_gap = 0.00015;
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
    // Gate4 shadow evidence helper: keep policy decisions flowing without
    // position-state side effects during backtest replays.
    bool backtest_shadow_policy_only = false;
    // Correctness probe (default OFF): allow strategy-less runtime positions
    // to use live-like RiskManager exit mapping in backtest.
    bool backtest_strategyless_runtime_live_exit_mapping = false;
    // Guard flag (default OFF): prefilter-qualified uptrend rescue tail guard.
    bool enable_uptrend_rescue_prefilter_tail_guard = false;
    // Deprecated alias (default OFF): retained for backward-compatible config.
    bool enable_v21_rescue_prefiltered_pair_probe = false;
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
