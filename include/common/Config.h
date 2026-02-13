#pragma once

#include <string>
#include <mutex>
#include <nlohmann/json.hpp>
#include <nlohmann/json.hpp>
#include "engine/EngineConfig.h"
#include "strategy/StrategyConfig.h"

namespace autolife {

class Config {
public:
    static Config& getInstance();
    void load(const std::string& config_path);
    
    std::string getAccessKey() const { return access_key_; }
    std::string getSecretKey() const { return secret_key_; }
    double getInitialCapital() const { return initial_capital_; }
    void setInitialCapital(double v) { initial_capital_ = v; engine_config_.initial_capital = v; }
    void setEnabledStrategies(const std::vector<std::string>& v) { engine_config_.enabled_strategies = v; }
    double getMaxDrawdown() const { return max_drawdown_; }
    double getPositionSizeRatio() const { return position_size_ratio_; }
    std::string getLogLevel() const { return log_level_; }
    // [✅ 추가] 완성된 엔진 설정 구조체 반환
    engine::EngineConfig getEngineConfig() const { return engine_config_; }
    // [✅ 추가] 동적 손절 배수 설정
    double getStopLossMultiplier() const { return stop_loss_multiplier_; }
    
    // [Refactor] Centralized Trading Constants
    double getFeeRate() const { return fee_rate_; }
    double getMinOrderKrw() const { return min_order_krw_; }
    double getMaxSlippagePct() const { return max_slippage_pct_; }
    double getRiskPerTradePct() const { return risk_per_trade_pct_; }
    
    // Strategy Configs
    strategy::ScalpingStrategyConfig getScalpingConfig() const { return scalping_config_; }
    strategy::MomentumStrategyConfig getMomentumConfig() const { return momentum_config_; }
    strategy::BreakoutStrategyConfig getBreakoutConfig() const { return breakout_config_; }
    strategy::MeanReversionStrategyConfig getMeanReversionConfig() const { return mean_reversion_config_; }
    strategy::GridTradingStrategyConfig getGridTradingConfig() const { return grid_trading_config_; }

private:
    Config() = default;
    std::string access_key_;
    std::string secret_key_;
    double initial_capital_ = 50000.0;
    double max_drawdown_ = 0.15;
    double position_size_ratio_ = 0.01;
    std::string log_level_ = "info";
    
    // Trading Constants (Defaults)
    double fee_rate_ = 0.0005;        // 0.05% (Upbit KRW)
    double min_order_krw_ = 5000.0;   // 5000 KRW
    double max_slippage_pct_ = 0.003; // 0.3%
    double risk_per_trade_pct_ = 0.01;// 1% per trade

    engine::EngineConfig engine_config_;
    strategy::ScalpingStrategyConfig scalping_config_;
    strategy::MomentumStrategyConfig momentum_config_;
    strategy::BreakoutStrategyConfig breakout_config_;
    strategy::MeanReversionStrategyConfig mean_reversion_config_;
    strategy::GridTradingStrategyConfig grid_trading_config_;
    double stop_loss_multiplier_ = 1.0;
};

} // namespace autolife
