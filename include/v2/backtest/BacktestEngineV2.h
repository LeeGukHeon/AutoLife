#pragma once

#include <memory>
#include <vector>

#include "v2/contracts/IExecutionPlane.h"
#include "v2/contracts/IPolicyPlane.h"
#include "v2/contracts/IRiskPlane.h"
#include "v2/engine/TradingEngineV2.h"
#include "v2/model/KernelTypes.h"

namespace autolife {
namespace v2 {
namespace backtest {

struct BacktestEngineV2Config {
    engine::TradingEngineV2Config runtime_config;
    bool collect_cycle_results = true;
};

class BacktestEngineV2 {
public:
    BacktestEngineV2(
        std::shared_ptr<IPolicyPlane> policy_plane,
        std::shared_ptr<IRiskPlane> risk_plane,
        std::shared_ptr<IExecutionPlane> execution_plane,
        BacktestEngineV2Config config = {}
    );

    DecisionResult runCycle(
        const MarketSnapshot& market_snapshot,
        const PortfolioSnapshot& portfolio_snapshot
    );

    const std::vector<DecisionResult>& cycleResults() const;
    std::size_t cycleCount() const;

private:
    engine::TradingEngineV2 runtime_engine_;
    BacktestEngineV2Config config_;
    std::vector<DecisionResult> cycle_results_;
};

} // namespace backtest
} // namespace v2
} // namespace autolife
