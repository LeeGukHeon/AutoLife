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
    double foundation_exit_stop_loss_pct = 0.006;
    double foundation_exit_take_profit_pct = 0.018;
    double foundation_exit_time_limit_hours = 6.0;
    double foundation_exit_time_limit_min_profit_pct = 0.001;
    double foundation_position_risk_budget_pct = 0.0065;
    double foundation_position_notional_cap_pct = 0.22;
    double foundation_position_liq_mult_lt_50 = 0.60;
    double foundation_position_liq_mult_lt_55 = 0.75;
    double foundation_position_vol_mult_gt_4 = 0.85;
    double foundation_position_spread_threshold_pct = 0.0025;
    double foundation_position_spread_mult = 0.80;
    double foundation_position_min_notional_pct_low_liq = 0.02;
    double foundation_position_min_notional_pct_default = 0.03;
    double foundation_position_output_min_pct = 0.02;
    double foundation_position_output_max_pct = 0.22;
    double foundation_position_mult_thin_liquidity_adaptive = 0.48;
    double foundation_position_mult_downtrend_low_flow_rebound = 0.32;
    double foundation_position_mult_ranging_low_flow = 0.50;
    double foundation_position_mult_thin_low_vol_uptrend = 0.55;
    double foundation_position_mult_thin_low_vol = 0.62;
    double foundation_risk_mult_uptrend = 1.35;
    double foundation_risk_floor_uptrend = 0.0040;
    double foundation_risk_cap_uptrend = 0.0180;
    double foundation_risk_mult_ranging = 1.15;
    double foundation_risk_floor_ranging = 0.0035;
    double foundation_risk_cap_ranging = 0.0140;
    double foundation_risk_mult_hostile = 0.90;
    double foundation_risk_floor_hostile = 0.0030;
    double foundation_risk_cap_hostile = 0.0100;
    double foundation_risk_mult_unknown = 1.00;
    double foundation_risk_floor_unknown = 0.0038;
    double foundation_risk_cap_unknown = 0.0140;
    double foundation_reward_risk_uptrend = 1.90;
    double foundation_reward_risk_ranging = 1.45;
    double foundation_reward_risk_hostile = 1.15;
    double foundation_reward_risk_unknown = 1.35;
    double foundation_take_profit_1_rr_multiplier = 0.55;
    double foundation_breakeven_rr_multiplier = 0.80;
    double foundation_trailing_rr_multiplier = 1.20;
    double foundation_strong_buy_strength_threshold = 0.74;
    double foundation_implied_win_base = 0.47;
    double foundation_implied_win_strength_scale = 0.18;
    double foundation_implied_win_preclamp_min = 0.44;
    double foundation_implied_win_preclamp_max = 0.66;
    double foundation_implied_win_hostile_uptrend_penalty = 0.08;
    double foundation_implied_win_hostile_min = 0.36;
    double foundation_implied_win_hostile_max = 0.58;
    double foundation_implied_win_mtf_scale = 0.22;
    double foundation_implied_win_mtf_add_min = -0.05;
    double foundation_implied_win_mtf_add_max = 0.06;
    double foundation_implied_win_final_min = 0.34;
    double foundation_implied_win_final_max = 0.72;
    double foundation_signal_filter_base = 0.52;
    double foundation_signal_filter_strength_scale = 0.30;
    double foundation_signal_filter_min = 0.55;
    double foundation_signal_filter_max = 0.85;
    double foundation_signal_filter_hostile_uptrend_add = 0.03;
    double foundation_signal_filter_hostile_uptrend_max = 0.88;
    int foundation_mtf_min_bars_5m = 24;
    int foundation_mtf_min_bars_15m = 20;
    int foundation_mtf_min_bars_1h = 16;
    int foundation_mtf_momentum_5m_lookback = 6;
    int foundation_mtf_momentum_15m_lookback = 4;
    int foundation_mtf_momentum_1h_lookback = 3;
    int foundation_mtf_ema_fast_15m = 8;
    int foundation_mtf_ema_slow_15m = 21;
    int foundation_mtf_ema_fast_1h = 5;
    int foundation_mtf_ema_slow_1h = 13;
    int foundation_mtf_rsi_period = 14;
    double foundation_mtf_score_base = 0.50;
    double foundation_mtf_score_momentum_5m_weight = 14.0;
    double foundation_mtf_score_momentum_5m_clip = 0.08;
    double foundation_mtf_score_momentum_15m_weight = 18.0;
    double foundation_mtf_score_momentum_15m_clip = 0.10;
    double foundation_mtf_score_momentum_1h_weight = 12.0;
    double foundation_mtf_score_momentum_1h_clip = 0.08;
    double foundation_mtf_score_ema_gap_15m_weight = 16.0;
    double foundation_mtf_score_ema_gap_15m_clip = 0.08;
    double foundation_mtf_score_ema_gap_1h_weight = 10.0;
    double foundation_mtf_score_ema_gap_1h_clip = 0.06;
    double foundation_mtf_score_rsi_15m_center = 56.0;
    double foundation_mtf_score_rsi_15m_divisor = 180.0;
    double foundation_mtf_score_rsi_15m_clip = 0.05;
    double foundation_mtf_score_rsi_5m_center = 54.0;
    double foundation_mtf_score_rsi_5m_divisor = 220.0;
    double foundation_mtf_score_rsi_5m_clip = 0.04;
    double foundation_mtf_uptrend_min_momentum_15m = -0.0020;
    double foundation_mtf_uptrend_min_ema_gap_15m = -0.0014;
    double foundation_mtf_uptrend_min_momentum_1h = -0.0050;
    double foundation_mtf_uptrend_max_rsi_15m = 71.0;
    double foundation_mtf_uptrend_min_score = 0.43;
    double foundation_mtf_ranging_max_abs_momentum_15m = 0.0240;
    double foundation_mtf_ranging_max_rsi_15m = 66.0;
    double foundation_mtf_ranging_min_score = 0.37;
    double foundation_mtf_hostile_min_momentum_1h = -0.0100;
    double foundation_mtf_hostile_min_momentum_15m = -0.0070;
    double foundation_mtf_hostile_max_rsi_5m = 50.0;
    double foundation_mtf_hostile_min_score = 0.50;
    double foundation_mtf_unknown_min_score = 0.47;
    double foundation_entry_base_liquidity_min = 42.0;
    double foundation_entry_base_volume_surge_min = 0.68;
    double foundation_entry_spread_guard_max = 0.0042;
    double foundation_entry_adaptive_liquidity_floor_default = 32.0;
    double foundation_entry_adaptive_liquidity_floor_ranging = 20.0;
    double foundation_entry_adaptive_liquidity_floor_uptrend = 30.0;
    double foundation_entry_adaptive_liquidity_floor_unknown = 24.0;
    double foundation_entry_adaptive_volume_floor_default = 0.45;
    double foundation_entry_adaptive_volume_floor_ranging = 0.20;
    double foundation_entry_adaptive_volume_floor_uptrend = 0.34;
    double foundation_entry_adaptive_volume_floor_unknown = 0.28;
    double foundation_entry_adaptive_thin_volatility_max = 3.6;
    double foundation_entry_adaptive_thin_imbalance_min = -0.26;
    double foundation_entry_adaptive_thin_buy_pressure_ratio_min = 0.74;
    double foundation_entry_narrow_relief_spread_max = 0.0018;
    double foundation_entry_narrow_relief_volatility_max = 2.2;
    double foundation_entry_narrow_relief_liquidity_min = 34.0;
    double foundation_entry_narrow_relief_volume_surge_min = 0.58;
    double foundation_entry_narrow_relief_imbalance_min = -0.14;
    double foundation_entry_narrow_relief_buy_pressure_ratio_min = 0.85;
    bool foundation_entry_enable_low_liq_relaxed_path = true;
    double foundation_entry_low_liq_relaxed_liquidity_min = 28.0;
    double foundation_entry_low_liq_relaxed_liquidity_max = 45.0;
    double foundation_entry_low_liq_relaxed_volume_surge_min = 0.46;
    double foundation_entry_low_liq_relaxed_imbalance_min = -0.18;
    double foundation_entry_low_liq_relaxed_buy_pressure_ratio_min = 0.80;
    bool foundation_entry_low_liq_uptrend_enabled = true;
    double foundation_entry_low_liq_uptrend_liquidity_min = 34.0;
    double foundation_entry_low_liq_uptrend_liquidity_max = 48.0;
    double foundation_entry_low_liq_uptrend_volume_surge_min = 0.62;
    double foundation_entry_low_liq_uptrend_volume_surge_max = 1.70;
    double foundation_entry_low_liq_uptrend_imbalance_min = -0.06;
    double foundation_entry_low_liq_uptrend_ret5_min = 0.0004;
    double foundation_entry_low_liq_uptrend_ret20_min = 0.0010;
    double foundation_entry_low_liq_uptrend_price_to_ema_fast_min = 0.9992;
    double foundation_entry_low_liq_uptrend_ema_fast_to_ema_slow_min = 0.9990;
    double foundation_entry_low_liq_uptrend_rsi_min = 48.0;
    double foundation_entry_low_liq_uptrend_rsi_max = 68.0;
    bool foundation_entry_thin_liq_adaptive_enabled = true;
    double foundation_entry_thin_liq_adaptive_liquidity_min = 28.0;
    double foundation_entry_thin_liq_adaptive_liquidity_max = 56.0;
    double foundation_entry_thin_liq_adaptive_volume_surge_min = 0.52;
    double foundation_entry_thin_liq_adaptive_volume_surge_max = 1.85;
    double foundation_entry_thin_liq_adaptive_spread_max = 0.0038;
    double foundation_entry_thin_liq_adaptive_uptrend_price_to_ema_fast_min = 0.9995;
    double foundation_entry_thin_liq_adaptive_uptrend_ema_fast_to_ema_slow_min = 0.9990;
    double foundation_entry_thin_liq_adaptive_uptrend_rsi_min = 44.0;
    double foundation_entry_thin_liq_adaptive_uptrend_rsi_max = 68.0;
    double foundation_entry_thin_liq_adaptive_uptrend_ret8_min = -0.0004;
    double foundation_entry_thin_liq_adaptive_uptrend_ret20_min = 0.0003;
    double foundation_entry_thin_liq_adaptive_uptrend_imbalance_min = -0.08;
    double foundation_entry_thin_liq_adaptive_ranging_price_to_bb_middle_max = 1.0030;
    double foundation_entry_thin_liq_adaptive_ranging_rsi_min = 34.0;
    double foundation_entry_thin_liq_adaptive_ranging_rsi_max = 50.0;
    double foundation_entry_thin_liq_adaptive_ranging_ret3_max = 0.0018;
    double foundation_entry_thin_liq_adaptive_ranging_ret20_min = -0.0015;
    double foundation_entry_thin_liq_adaptive_ranging_imbalance_min = -0.12;
    double foundation_entry_thin_liq_adaptive_hostile_rsi_max = 35.0;
    double foundation_entry_thin_liq_adaptive_hostile_price_to_ema_fast_min = 0.9985;
    double foundation_entry_thin_liq_adaptive_hostile_buy_pressure_ratio_min = 0.95;
    double foundation_entry_thin_liq_adaptive_hostile_ret3_min = -0.0010;
    double foundation_entry_thin_liq_adaptive_hostile_ret8_min = -0.0020;
    double foundation_entry_thin_liq_adaptive_unknown_price_to_ema_fast_min = 0.9990;
    double foundation_entry_thin_liq_adaptive_unknown_rsi_max = 55.0;
    double foundation_entry_thin_liq_adaptive_unknown_imbalance_min = -0.08;
    double foundation_entry_thin_liq_adaptive_unknown_ret8_min = -0.0006;
    bool foundation_entry_ranging_low_flow_enabled = true;
    double foundation_entry_ranging_low_flow_liquidity_min = 22.0;
    double foundation_entry_ranging_low_flow_liquidity_max = 55.0;
    double foundation_entry_ranging_low_flow_volume_surge_min = 0.34;
    double foundation_entry_ranging_low_flow_volume_surge_max = 1.30;
    double foundation_entry_ranging_low_flow_spread_max = 0.0042;
    double foundation_entry_ranging_low_flow_price_to_bb_middle_max = 1.0030;
    double foundation_entry_ranging_low_flow_price_to_ema_fast_min = 0.9950;
    double foundation_entry_ranging_low_flow_rsi_min = 28.0;
    double foundation_entry_ranging_low_flow_rsi_max = 52.0;
    double foundation_entry_ranging_low_flow_ret3_max = 0.0015;
    double foundation_entry_ranging_low_flow_ret8_min = -0.0034;
    double foundation_entry_ranging_low_flow_imbalance_min = -0.14;
    double foundation_entry_ranging_low_flow_buy_pressure_ratio_min = 0.82;
    bool foundation_entry_downtrend_rebound_enabled = true;
    double foundation_entry_downtrend_rebound_liquidity_min = 18.0;
    double foundation_entry_downtrend_rebound_liquidity_max = 52.0;
    double foundation_entry_downtrend_rebound_volume_surge_min = 0.18;
    double foundation_entry_downtrend_rebound_volume_surge_max = 1.40;
    double foundation_entry_downtrend_rebound_spread_max = 0.0032;
    double foundation_entry_downtrend_rebound_imbalance_min = -0.12;
    double foundation_entry_downtrend_rebound_buy_pressure_ratio_min = 0.86;
    double foundation_entry_downtrend_rebound_ret3_min = -0.0012;
    double foundation_entry_downtrend_rebound_ret8_min = -0.0060;
    double foundation_entry_downtrend_rebound_ret20_min = -0.0280;
    double foundation_entry_downtrend_rebound_price_to_ema_fast_min = 0.9970;
    double foundation_entry_downtrend_rebound_ema_fast_to_ema_slow_min = 0.9920;
    double foundation_entry_downtrend_rebound_rsi_min = 23.0;
    double foundation_entry_downtrend_rebound_rsi_max = 43.0;
    double foundation_entry_uptrend_ret20_floor_liquidity_pivot = 60.0;
    double foundation_entry_uptrend_ret20_floor_high_liq = 0.0008;
    double foundation_entry_uptrend_ret20_floor_default = 0.0004;
    double foundation_entry_uptrend_overextended_rsi_min = 68.0;
    double foundation_entry_uptrend_overextended_ret5_min = 0.0045;
    double foundation_entry_uptrend_base_price_to_ema_fast_min = 0.9985;
    double foundation_entry_uptrend_base_ema_fast_to_ema_slow_min = 0.9980;
    double foundation_entry_uptrend_base_rsi_min = 42.0;
    double foundation_entry_uptrend_base_rsi_max = 74.0;
    double foundation_entry_uptrend_base_imbalance_min = -0.16;
    double foundation_entry_uptrend_base_buy_pressure_ratio_min = 0.86;
    double foundation_entry_uptrend_base_ret5_min = -0.0016;
    double foundation_entry_uptrend_base_ret20_offset = -0.0008;
    double foundation_entry_uptrend_relief_context_liquidity_max = 55.0;
    double foundation_entry_uptrend_relief_context_volatility_max = 1.8;
    double foundation_entry_uptrend_relief_price_to_ema_fast_min = 0.9978;
    double foundation_entry_uptrend_relief_ema_fast_to_ema_slow_min = 0.9972;
    double foundation_entry_uptrend_relief_rsi_min = 40.0;
    double foundation_entry_uptrend_relief_rsi_max = 75.0;
    double foundation_entry_uptrend_relief_imbalance_min = -0.20;
    double foundation_entry_uptrend_relief_buy_pressure_ratio_min = 0.82;
    double foundation_entry_uptrend_relief_ret5_min = -0.0022;
    double foundation_entry_uptrend_relief_ret20_offset = -0.0014;
    double foundation_entry_uptrend_exhaustion_context_liquidity_min = 62.0;
    double foundation_entry_uptrend_exhaustion_context_volatility_max = 1.7;
    double foundation_entry_uptrend_exhaustion_rsi_min = 63.0;
    double foundation_entry_uptrend_exhaustion_ret3_min = 0.0038;
    double foundation_entry_uptrend_exhaustion_ret5_min = 0.0055;
    double foundation_entry_uptrend_exhaustion_ema_premium_min = 0.0045;
    double foundation_entry_uptrend_thin_context_volume_surge_min = 0.74;
    double foundation_entry_uptrend_thin_context_imbalance_min = -0.10;
    double foundation_entry_uptrend_thin_context_rsi_max = 72.0;
    double foundation_entry_uptrend_thin_context_ret5_min = -0.0006;
    double foundation_entry_uptrend_thin_context_ret20_min = 0.0004;
    double foundation_entry_ranging_structure_price_to_bb_middle_max = 1.0025;
    double foundation_entry_ranging_structure_rsi_max = 46.0;
    double foundation_entry_ranging_structure_imbalance_min = -0.24;
    double foundation_entry_ranging_structure_buy_pressure_ratio_min = 0.80;
    double foundation_entry_ranging_relief_price_to_bb_middle_max = 1.0035;
    double foundation_entry_ranging_relief_rsi_max = 47.0;
    double foundation_entry_ranging_relief_imbalance_min = -0.20;
    double foundation_entry_ranging_relief_buy_pressure_ratio_min = 0.82;
    double foundation_entry_hostile_bear_rebound_rsi_max = 32.0;
    double foundation_entry_hostile_bear_rebound_price_to_ema_fast_min = 1.0;
    double foundation_entry_hostile_bear_rebound_buy_pressure_ratio_min = 1.0;
    double foundation_entry_hostile_bear_rebound_liquidity_min = 62.0;
    double foundation_entry_hostile_bear_rebound_volume_surge_min = 1.10;
    double foundation_entry_unknown_structure_price_to_ema_fast_min = 1.0;
    double foundation_entry_unknown_structure_rsi_max = 52.0;
    double foundation_entry_unknown_structure_imbalance_min = -0.10;
    int foundation_entry_snapshot_min_bars = 60;
    int foundation_entry_snapshot_ema_fast_period = 12;
    int foundation_entry_snapshot_ema_slow_period = 48;
    int foundation_entry_snapshot_rsi_period = 14;
    int foundation_entry_snapshot_bb_period = 20;
    double foundation_entry_snapshot_bb_stddev = 2.0;
    double foundation_signal_context_thin_low_vol_liquidity_max = 55.0;
    double foundation_signal_context_thin_low_vol_volatility_max = 1.8;
    double foundation_signal_context_hostile_uptrend_liquidity_max = 50.0;
    double foundation_signal_context_hostile_uptrend_volatility_max = 1.9;
    double foundation_signal_path_risk_mult_thin_liq_adaptive = 0.74;
    double foundation_signal_path_reward_risk_min_thin_liq_adaptive = 2.10;
    double foundation_signal_path_risk_mult_downtrend_rebound = 0.58;
    double foundation_signal_path_reward_risk_min_downtrend_rebound = 2.00;
    double foundation_signal_path_risk_mult_low_liq_relaxed = 0.84;
    double foundation_signal_path_reward_risk_min_low_liq_relaxed = 2.25;
    double foundation_signal_path_risk_mult_ranging_low_flow = 0.78;
    double foundation_signal_path_reward_risk_min_ranging_low_flow = 1.80;
    double foundation_signal_context_thin_low_vol_risk_mult = 0.88;
    double foundation_signal_context_thin_low_vol_reward_min_uptrend = 1.35;
    double foundation_signal_context_thin_low_vol_reward_max_uptrend = 1.75;
    double foundation_signal_context_thin_low_vol_reward_min_other = 1.45;
    double foundation_signal_context_thin_low_vol_reward_max_other = 1.90;
    double foundation_signal_context_hostile_uptrend_risk_mult = 0.92;
    double foundation_signal_context_hostile_uptrend_reward_min = 1.30;
    double foundation_signal_context_hostile_uptrend_reward_max = 1.65;
    double foundation_signal_mtf_risk_scale = 0.40;
    double foundation_signal_mtf_risk_scale_min = 0.84;
    double foundation_signal_mtf_risk_scale_max = 1.12;
    double foundation_signal_mtf_reward_add_scale = 0.90;
    double foundation_signal_mtf_reward_add_min = -0.12;
    double foundation_signal_mtf_reward_add_max = 0.18;
    double foundation_signal_risk_pct_min = 0.0022;
    double foundation_signal_risk_pct_max = 0.0185;
    double foundation_signal_reward_risk_min = 1.10;
    double foundation_signal_reward_risk_max = 2.50;
    double foundation_signal_strength_base = 0.50;
    double foundation_signal_strength_ema_gap_weight = 12.0;
    double foundation_signal_strength_ema_gap_add_min = -0.10;
    double foundation_signal_strength_ema_gap_add_max = 0.20;
    double foundation_signal_strength_rsi_center = 55.0;
    double foundation_signal_strength_rsi_divisor = 120.0;
    double foundation_signal_strength_rsi_add_min = -0.08;
    double foundation_signal_strength_rsi_add_max = 0.10;
    double foundation_signal_strength_liquidity_center = 50.0;
    double foundation_signal_strength_liquidity_divisor = 350.0;
    double foundation_signal_strength_liquidity_add_min = -0.07;
    double foundation_signal_strength_liquidity_add_max = 0.12;
    double foundation_signal_strength_uptrend_add = 0.08;
    double foundation_signal_strength_hostile_regime_add = -0.06;
    double foundation_signal_strength_path_thin_liq_add = 0.03;
    double foundation_signal_strength_path_ranging_add = 0.02;
    double foundation_signal_strength_path_low_liq_add = 0.04;
    double foundation_signal_strength_hostile_uptrend_add = -0.03;
    double foundation_signal_strength_mtf_scale = 0.30;
    double foundation_signal_strength_mtf_add_min = -0.10;
    double foundation_signal_strength_mtf_add_max = 0.12;
    double foundation_signal_strength_final_min = 0.35;
    double foundation_signal_strength_final_max = 0.92;

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
    double entry_capacity_default_stop_guard_pct = 0.03;
    double entry_capacity_stop_guard_min_pct = 0.03;
    double entry_capacity_stop_guard_max_pct = 0.20;
    double entry_capacity_exit_retention_floor = 0.50;
    double entry_capacity_slippage_guard_multiplier = 1.50;
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
    // Realtime entry veto dynamic policy coefficients.
    double execution_guard_veto_quality_strength_center = 0.60;
    double execution_guard_veto_quality_strength_scale = 0.25;
    double execution_guard_veto_quality_liquidity_center = 58.0;
    double execution_guard_veto_quality_liquidity_scale = 20.0;
    double execution_guard_veto_quality_strength_weight = 0.55;
    double execution_guard_veto_quality_liquidity_weight = 0.45;
    double execution_guard_veto_hostile_tighten_add = 0.12;
    double execution_guard_veto_uptrend_relief_strength_threshold = 0.75;
    double execution_guard_veto_uptrend_relief_liquidity_threshold = 65.0;
    double execution_guard_veto_uptrend_relief_add = 0.08;
    double execution_guard_veto_max_drop_tighten_scale = 0.35;
    double execution_guard_veto_max_drop_relief_scale = 0.15;
    double execution_guard_veto_max_spread_tighten_scale = 0.40;
    double execution_guard_veto_max_spread_relief_scale = 0.18;
    double execution_guard_veto_min_imbalance_tighten_scale = 0.22;
    double execution_guard_veto_min_imbalance_relief_scale = 0.12;
    double execution_guard_veto_max_drop_clamp_min = 0.0004;
    double execution_guard_veto_max_drop_clamp_max = 0.0200;
    double execution_guard_veto_max_spread_clamp_min = 0.0002;
    double execution_guard_veto_max_spread_clamp_max = 0.0150;
    double execution_guard_veto_min_imbalance_clamp_min = -0.90;
    double execution_guard_veto_min_imbalance_clamp_max = -0.05;
    // Live scan prefilter dynamic policy coefficients.
    double execution_guard_live_scan_volume_quantile_median = 0.50;
    double execution_guard_live_scan_volume_quantile_p70 = 0.70;
    double execution_guard_live_scan_base_spread_multiplier = 1.25;
    double execution_guard_live_scan_base_spread_clamp_min = 0.0005;
    double execution_guard_live_scan_base_spread_clamp_max = 0.01;
    double execution_guard_live_scan_base_floor_krw = 50000000.0;
    double execution_guard_live_scan_base_floor_min_order_multiplier = 2000.0;
    double execution_guard_live_scan_volume_tighten_base = 0.80;
    double execution_guard_live_scan_volume_tighten_scale = 0.45;
    double execution_guard_live_scan_universe_anchor_base = 0.85;
    double execution_guard_live_scan_universe_anchor_tighten_scale = 0.20;
    double execution_guard_live_scan_dynamic_ceiling_base_volume_multiplier = 2.0;
    double execution_guard_live_scan_dynamic_ceiling_p70_multiplier = 1.40;
    double execution_guard_live_scan_spread_tighten_scale = 0.30;
    double execution_guard_live_scan_final_spread_clamp_min = 0.0003;
    double execution_guard_live_scan_final_spread_clamp_max = 0.01;
    double execution_guard_live_scan_ask_notional_base_multiplier = 5.0;
    double execution_guard_live_scan_ask_notional_tighten_scale = 2.0;
    double execution_guard_live_scan_ask_notional_min_multiplier = 3.0;
    double execution_guard_live_scan_ask_notional_max_multiplier = 12.0;
    // Dynamic slippage policy coefficients.
    double execution_guard_slippage_buy_hostile_tighten_add = 0.15;
    double execution_guard_slippage_buy_low_liquidity_threshold = 55.0;
    double execution_guard_slippage_buy_low_liquidity_scale = 25.0;
    double execution_guard_slippage_buy_low_liquidity_tighten_cap = 0.25;
    double execution_guard_slippage_buy_regime_quality_relief_add = 0.10;
    double execution_guard_slippage_buy_quality_strength_center = 0.65;
    double execution_guard_slippage_buy_quality_strength_scale = 0.25;
    double execution_guard_slippage_buy_quality_strength_weight = 0.20;
    double execution_guard_slippage_buy_quality_liquidity_center = 60.0;
    double execution_guard_slippage_buy_quality_liquidity_scale = 20.0;
    double execution_guard_slippage_buy_quality_liquidity_weight = 0.15;
    double execution_guard_slippage_buy_quality_ev_center = 0.0004;
    double execution_guard_slippage_buy_quality_ev_scale = 0.0010;
    double execution_guard_slippage_buy_quality_ev_weight = 0.12;
    double execution_guard_slippage_buy_quality_tighten_dampen = 0.60;
    double execution_guard_slippage_buy_tighten_scale = 0.35;
    double execution_guard_slippage_buy_relief_scale = 0.18;
    double execution_guard_slippage_sell_relax_tighten_scale = 0.15;
    double execution_guard_slippage_sell_relax_urgent_add = 0.45;
    double execution_guard_slippage_sell_relax_low_liquidity_threshold = 50.0;
    double execution_guard_slippage_sell_relax_low_liquidity_add = 0.10;
    double execution_guard_slippage_sell_relax_uptrend_liquidity_threshold = 65.0;
    double execution_guard_slippage_sell_relax_uptrend_relief = 0.05;
    double execution_guard_slippage_sell_relax_clamp_min = -0.15;
    double execution_guard_slippage_sell_relax_clamp_max = 1.20;
    double execution_guard_slippage_sell_urgent_cap = 0.03;
    double execution_guard_slippage_sell_nonurgent_cap = 0.02;
    double execution_guard_slippage_base_clamp_min = 0.0002;
    double execution_guard_slippage_base_clamp_max = 0.05;
    double execution_guard_slippage_buy_clamp_min = 0.0003;
    double execution_guard_slippage_buy_clamp_max = 0.012;
    double execution_guard_slippage_sell_clamp_min = 0.0003;
    double execution_guard_slippage_guard_clamp_min = 0.0005;
    double execution_guard_slippage_guard_buy_clamp_max = 0.02;
    double execution_guard_slippage_guard_sell_clamp_max = 0.03;
    double execution_guard_slippage_guard_multiplier = 1.5;
    // Hostility tighten pressure policy coefficients.
    double execution_guard_tighten_severe_gap_min = 0.01;
    double execution_guard_tighten_severe_clamp_min = 0.01;
    double execution_guard_tighten_severe_clamp_max = 1.0;
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
    std::string probabilistic_runtime_bundle_path = "config/model/probabilistic_runtime_bundle_v2.json";
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
    // Probabilistic-primary minimum conditions:
    // rank by probabilistic confidence first, but enforce these baseline
    // operating constraints before entry.
    double probabilistic_primary_min_h5_calibrated = 0.48;
    double probabilistic_primary_min_h5_margin = -0.03;
    double probabilistic_primary_min_liquidity_score = 42.0;
    double probabilistic_primary_min_signal_strength = 0.34;
    double min_expected_edge_pct = 0.0010; // 0.10% after cost
    double min_reward_risk = 1.20;         // TP/SL ratio
    double min_rr_weak_signal = 1.80;      // dynamic RR target for weak signals
    double min_rr_strong_signal = 1.25;    // dynamic RR target for strong signals
    int min_strategy_trades_for_ev = 30;   // minimum samples before EV gating
    double min_strategy_expectancy_krw = 0.0;
    double min_strategy_profit_factor = 1.00;
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
    double hostility_score_regime_high_vol = 0.72;
    double hostility_score_regime_trending_down = 0.62;
    double hostility_score_regime_ranging = 0.34;
    double hostility_score_regime_trending_up = 0.12;
    double hostility_score_regime_unknown = 0.28;
    double hostility_score_volatility_pivot = 1.8;
    double hostility_score_volatility_divisor = 6.0;
    double hostility_score_volatility_cap = 0.28;
    double hostility_score_liquidity_pivot = 55.0;
    double hostility_score_liquidity_divisor = 90.0;
    double hostility_score_liquidity_cap = 0.20;
    double hostility_score_spread_pct_pivot = 0.18;
    double hostility_score_spread_pct_divisor = 0.40;
    double hostility_score_spread_pct_cap = 0.18;
    double hostility_scan_buy_limit_hostile_add = 0.13;
    // Gate4 shadow evidence helper: keep policy decisions flowing without
    // position-state side effects during backtest replays.
    bool backtest_shadow_policy_only = false;
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
