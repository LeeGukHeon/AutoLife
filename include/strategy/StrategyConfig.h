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

} // namespace strategy
} // namespace autolife
