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
