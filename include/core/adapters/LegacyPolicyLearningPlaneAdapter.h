#pragma once

#include "core/contracts/IPolicyLearningPlane.h"
#include "engine/AdaptivePolicyController.h"
#include "engine/PerformanceStore.h"

namespace autolife {
namespace core {

class LegacyPolicyLearningPlaneAdapter : public IPolicyLearningPlane {
public:
    explicit LegacyPolicyLearningPlaneAdapter(
        engine::AdaptivePolicyController& controller,
        const engine::PerformanceStore* performance_store = nullptr
    );

    void setPerformanceStore(const engine::PerformanceStore* performance_store);

    PolicyDecisionBatch selectCandidates(
        const std::vector<strategy::Signal>& candidates,
        const PolicyContext& context
    ) override;

private:
    engine::AdaptivePolicyController& controller_;
    const engine::PerformanceStore* performance_store_;
};

} // namespace core
} // namespace autolife

