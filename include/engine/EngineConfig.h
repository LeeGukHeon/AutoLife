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
    double min_expected_edge_pct = 0.0010; // 0.10% after cost
    double min_reward_risk = 1.20;         // TP/SL ratio
    double min_rr_weak_signal = 1.80;      // dynamic RR target for weak signals
    double min_rr_strong_signal = 1.25;    // dynamic RR target for strong signals
    int min_strategy_trades_for_ev = 30;   // minimum samples before EV gating
    double min_strategy_expectancy_krw = 0.0;
    double min_strategy_profit_factor = 1.00;
    bool avoid_high_volatility = true;
    bool avoid_trending_down = true;
    bool enable_core_plane_bridge = false;
    bool enable_core_policy_plane = false;
    bool enable_core_risk_plane = false;
    bool enable_core_execution_plane = false;
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
