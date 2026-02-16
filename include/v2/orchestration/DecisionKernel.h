#pragma once

#include <memory>

#include "v2/contracts/IExecutionPlane.h"
#include "v2/contracts/IPolicyPlane.h"
#include "v2/contracts/IRiskPlane.h"
#include "v2/model/KernelTypes.h"

namespace autolife {
namespace v2 {

class DecisionKernel {
public:
    DecisionKernel(
        std::shared_ptr<IPolicyPlane> policy_plane,
        std::shared_ptr<IRiskPlane> risk_plane,
        std::shared_ptr<IExecutionPlane> execution_plane
    );

    DecisionResult runCycle(
        const MarketSnapshot& market_snapshot,
        const PortfolioSnapshot& portfolio_snapshot,
        const KernelConfig& config
    );

    void setPolicyPlane(std::shared_ptr<IPolicyPlane> policy_plane);
    void setRiskPlane(std::shared_ptr<IRiskPlane> risk_plane);
    void setExecutionPlane(std::shared_ptr<IExecutionPlane> execution_plane);

private:
    std::shared_ptr<IPolicyPlane> policy_plane_;
    std::shared_ptr<IRiskPlane> risk_plane_;
    std::shared_ptr<IExecutionPlane> execution_plane_;
};

} // namespace v2
} // namespace autolife

