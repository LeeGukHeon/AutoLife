#pragma once

#include <cstddef>
#include <memory>

#include "v2/contracts/IExecutionPlane.h"
#include "v2/contracts/IPolicyPlane.h"
#include "v2/contracts/IRiskPlane.h"
#include "v2/model/KernelTypes.h"
#include "v2/orchestration/DecisionKernel.h"

namespace autolife {
namespace v2 {
namespace engine {

struct TradingEngineV2Config {
    bool enable_policy_plane = true;
    bool enable_risk_plane = true;
    bool enable_execution_plane = true;
    bool dry_run = true;
    int max_new_orders_per_cycle = 1;
};

class TradingEngineV2 {
public:
    TradingEngineV2(
        std::shared_ptr<IPolicyPlane> policy_plane,
        std::shared_ptr<IRiskPlane> risk_plane,
        std::shared_ptr<IExecutionPlane> execution_plane,
        TradingEngineV2Config config = {}
    );

    DecisionResult runCycle(
        const MarketSnapshot& market_snapshot,
        const PortfolioSnapshot& portfolio_snapshot
    );

    void setConfig(const TradingEngineV2Config& config);
    const TradingEngineV2Config& config() const;
    std::size_t cycleCount() const;

private:
    DecisionKernel kernel_;
    TradingEngineV2Config config_;
    std::size_t cycle_count_ = 0;
};

} // namespace engine
} // namespace v2
} // namespace autolife
