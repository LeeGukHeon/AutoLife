#pragma once

#include <string>
#include <vector>
#include "engine/EngineConfig.h"

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
    void setBacktestShadowPolicyOnly(bool v) { engine_config_.backtest_shadow_policy_only = v; }
    double getMaxDrawdown() const { return max_drawdown_; }
    double getPositionSizeRatio() const { return position_size_ratio_; }
    std::string getLogLevel() const { return log_level_; }
    // [??異붽?] ?꾩꽦???붿쭊 ?ㅼ젙 援ъ“泥?諛섑솚
    engine::EngineConfig getEngineConfig() const { return engine_config_; }
    // [??異붽?] ?숈쟻 ?먯젅 諛곗닔 ?ㅼ젙
    double getStopLossMultiplier() const { return stop_loss_multiplier_; }
    
    // [Refactor] Centralized Trading Constants
    double getFeeRate() const { return fee_rate_; }
    double getMinOrderKrw() const { return min_order_krw_; }
    double getMaxSlippagePct() const { return max_slippage_pct_; }
    double getRiskPerTradePct() const { return risk_per_trade_pct_; }

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
    double stop_loss_multiplier_ = 1.0;
};

} // namespace autolife
