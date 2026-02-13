#include "core/orchestration/TradingCycleCoordinator.h"

namespace autolife {
namespace core {

TradingCycleCoordinator::TradingCycleCoordinator(
    std::shared_ptr<IPolicyLearningPlane> policy_plane,
    std::shared_ptr<IRiskCompliancePlane> risk_plane,
    std::shared_ptr<IExecutionPlane> execution_plane
)
    : policy_plane_(std::move(policy_plane))
    , risk_plane_(std::move(risk_plane))
    , execution_plane_(std::move(execution_plane)) {}

PolicyDecisionBatch TradingCycleCoordinator::selectPolicyCandidates(
    const std::vector<strategy::Signal>& candidates,
    const PolicyContext& context
) {
    if (!policy_plane_) {
        PolicyDecisionBatch batch;
        batch.selected_candidates = candidates;
        return batch;
    }
    return policy_plane_->selectCandidates(candidates, context);
}

PreTradeCheck TradingCycleCoordinator::validateEntry(
    const ExecutionRequest& request,
    const strategy::Signal& signal
) {
    if (!risk_plane_) {
        return {true, "risk_plane_unset"};
    }
    return risk_plane_->validateEntry(request, signal);
}

PreTradeCheck TradingCycleCoordinator::validateExit(
    const std::string& market,
    const risk::Position& position,
    double exit_price
) {
    if (!risk_plane_) {
        return {true, "risk_plane_unset"};
    }
    return risk_plane_->validateExit(market, position, exit_price);
}

bool TradingCycleCoordinator::submit(const ExecutionRequest& request) {
    if (!execution_plane_) {
        return false;
    }
    return execution_plane_->submit(request);
}

bool TradingCycleCoordinator::cancel(const std::string& order_id) {
    if (!execution_plane_) {
        return false;
    }
    return execution_plane_->cancel(order_id);
}

void TradingCycleCoordinator::pollExecution() {
    if (execution_plane_) {
        execution_plane_->poll();
    }
}

std::vector<ExecutionUpdate> TradingCycleCoordinator::drainExecutionUpdates() {
    if (!execution_plane_) {
        return {};
    }
    return execution_plane_->drainUpdates();
}

} // namespace core
} // namespace autolife

