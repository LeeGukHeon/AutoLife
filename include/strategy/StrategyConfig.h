#pragma once

namespace autolife {
namespace strategy {

struct ScalpingStrategyConfig {
    int max_daily_trades = 15;
    int max_hourly_trades = 5;
    int max_consecutive_losses = 5;
    
    // RSI Thresholds
    double rsi_lower = 20.0;
    double rsi_upper = 75.0;
    
    // Volume Spike
    double volume_z_score_threshold = 1.15;
    
    // Profit/Loss
    double base_take_profit = 0.02;     // 2%
    double base_stop_loss = 0.01;       // 1%
    double min_risk_reward_ratio = 1.5; // (Reduced from 1.8 for config flexibility)
    
    // Signal Strength
    double min_signal_strength = 0.65;
};

struct MomentumStrategyConfig {
    int max_daily_trades = 12;
    int max_hourly_trades = 4;
    int max_consecutive_losses = 4;
    double min_liquidity_score = 50.0;
    double min_signal_strength = 0.40;
    int min_signal_interval_sec = 300;
    double min_risk_reward_ratio = 2.0;
    double min_expected_sharpe = 1.0;
};

struct BreakoutStrategyConfig {
    int max_daily_trades = 10;
    int max_hourly_trades = 3;
    int max_consecutive_losses = 4;
    double min_liquidity_score = 50.0;
    double min_signal_strength = 0.40;
    int min_signal_interval_sec = 720;
    double min_risk_reward_ratio = 1.5;
};

struct MeanReversionStrategyConfig {
    int max_daily_trades = 12;
    int max_hourly_trades = 4;
    int max_consecutive_losses = 4;
    double min_liquidity_score = 50.0;
    double min_signal_strength = 0.40;
    int min_signal_interval_sec = 600;
    double min_reversion_probability = 0.70;
};

struct GridTradingStrategyConfig {
    int max_daily_trades = 15;
    int max_hourly_trades = 5;
    int max_consecutive_losses = 3;
    double min_liquidity_score = 60.0;
    double min_signal_strength = 0.40;
    int min_signal_interval_sec = 900;
    double max_grid_capital_pct = 0.30;
};

} // namespace strategy
} // namespace autolife
