#pragma once

#include "engine/AdaptivePolicyController.h"
#include "engine/PerformanceStore.h"
#include "v2/contracts/IPolicyPlane.h"

namespace autolife {
namespace v2 {

class LegacyPolicyPlaneAdapter : public IPolicyPlane {
public:
    LegacyPolicyPlaneAdapter(
        engine::AdaptivePolicyController& controller,
        const engine::PerformanceStore* performance_store = nullptr
    );

    void setPerformanceStore(const engine::PerformanceStore* performance_store);

    PolicyDecisionBatch selectCandidates(
        const std::vector<SignalCandidate>& candidates,
        const PolicyContext& context
    ) override;

private:
    engine::AdaptivePolicyController& controller_;
    const engine::PerformanceStore* performance_store_;
};

} // namespace v2
} // namespace autolife

