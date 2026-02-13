#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/contracts/IExecutionPlane.h"
#include "core/contracts/IPolicyLearningPlane.h"
#include "core/contracts/IRiskCompliancePlane.h"
#include "core/model/PlaneTypes.h"
#include "risk/RiskManager.h"
#include "strategy/IStrategy.h"

namespace autolife {
namespace core {

class TradingCycleCoordinator {
public:
    TradingCycleCoordinator(
        std::shared_ptr<IPolicyLearningPlane> policy_plane,
        std::shared_ptr<IRiskCompliancePlane> risk_plane,
        std::shared_ptr<IExecutionPlane> execution_plane
    );

    PolicyDecisionBatch selectPolicyCandidates(
        const std::vector<strategy::Signal>& candidates,
        const PolicyContext& context
    );

    PreTradeCheck validateEntry(
        const ExecutionRequest& request,
        const strategy::Signal& signal
    );

    PreTradeCheck validateExit(
        const std::string& market,
        const risk::Position& position,
        double exit_price
    );

    bool submit(const ExecutionRequest& request);
    bool cancel(const std::string& order_id);
    void pollExecution();
    std::vector<ExecutionUpdate> drainExecutionUpdates();

private:
    std::shared_ptr<IPolicyLearningPlane> policy_plane_;
    std::shared_ptr<IRiskCompliancePlane> risk_plane_;
    std::shared_ptr<IExecutionPlane> execution_plane_;
};

} // namespace core
} // namespace autolife

