#include "v2/backtest/BacktestEngineV2.h"

namespace autolife {
namespace v2 {
namespace backtest {

BacktestEngineV2::BacktestEngineV2(
    std::shared_ptr<IPolicyPlane> policy_plane,
    std::shared_ptr<IRiskPlane> risk_plane,
    std::shared_ptr<IExecutionPlane> execution_plane,
    BacktestEngineV2Config config
)
    : runtime_engine_(
          std::move(policy_plane),
          std::move(risk_plane),
          std::move(execution_plane),
          config.runtime_config
      )
    , config_(config) {}

DecisionResult BacktestEngineV2::runCycle(
    const MarketSnapshot& market_snapshot,
    const PortfolioSnapshot& portfolio_snapshot
) {
    DecisionResult result = runtime_engine_.runCycle(market_snapshot, portfolio_snapshot);
    if (config_.collect_cycle_results) {
        cycle_results_.push_back(result);
    }
    return result;
}

const std::vector<DecisionResult>& BacktestEngineV2::cycleResults() const {
    return cycle_results_;
}

std::size_t BacktestEngineV2::cycleCount() const {
    return runtime_engine_.cycleCount();
}

} // namespace backtest
} // namespace v2
} // namespace autolife
