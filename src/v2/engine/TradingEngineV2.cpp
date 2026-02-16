#include "v2/engine/TradingEngineV2.h"

#include <algorithm>

namespace autolife {
namespace v2 {
namespace engine {

TradingEngineV2::TradingEngineV2(
    std::shared_ptr<IPolicyPlane> policy_plane,
    std::shared_ptr<IRiskPlane> risk_plane,
    std::shared_ptr<IExecutionPlane> execution_plane,
    TradingEngineV2Config config
)
    : kernel_(std::move(policy_plane), std::move(risk_plane), std::move(execution_plane))
    , config_(config) {}

DecisionResult TradingEngineV2::runCycle(
    const MarketSnapshot& market_snapshot,
    const PortfolioSnapshot& portfolio_snapshot
) {
    KernelConfig kernel_config;
    kernel_config.enable_policy_plane = config_.enable_policy_plane;
    kernel_config.enable_risk_plane = config_.enable_risk_plane;
    kernel_config.enable_execution_plane = config_.enable_execution_plane;
    kernel_config.max_new_orders_per_cycle = std::max(1, config_.max_new_orders_per_cycle);
    kernel_config.dry_run = config_.dry_run;

    DecisionResult result = kernel_.runCycle(market_snapshot, portfolio_snapshot, kernel_config);
    cycle_count_++;
    return result;
}

void TradingEngineV2::setConfig(const TradingEngineV2Config& config) {
    config_ = config;
}

const TradingEngineV2Config& TradingEngineV2::config() const {
    return config_;
}

std::size_t TradingEngineV2::cycleCount() const {
    return cycle_count_;
}

} // namespace engine
} // namespace v2
} // namespace autolife
