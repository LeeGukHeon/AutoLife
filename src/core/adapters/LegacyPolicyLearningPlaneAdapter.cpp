#include "core/adapters/LegacyPolicyLearningPlaneAdapter.h"

namespace autolife {
namespace core {

LegacyPolicyLearningPlaneAdapter::LegacyPolicyLearningPlaneAdapter(
    engine::AdaptivePolicyController& controller,
    const engine::PerformanceStore* performance_store
)
    : controller_(controller)
    , performance_store_(performance_store) {}

void LegacyPolicyLearningPlaneAdapter::setPerformanceStore(
    const engine::PerformanceStore* performance_store
) {
    performance_store_ = performance_store;
}

PolicyDecisionBatch LegacyPolicyLearningPlaneAdapter::selectCandidates(
    const std::vector<strategy::Signal>& candidates,
    const PolicyContext& context
) {
    engine::PolicyInput input;
    input.candidates = candidates;
    input.small_seed_mode = context.small_seed_mode;
    input.max_new_orders_per_scan = context.max_new_orders_per_scan;
    input.dominant_regime = context.dominant_regime;

    if (performance_store_ != nullptr) {
        input.strategy_stats = &performance_store_->byStrategy();
        input.bucket_stats = &performance_store_->byBucket();
    }

    engine::PolicyOutput output = controller_.selectCandidates(input);

    PolicyDecisionBatch batch;
    batch.selected_candidates = std::move(output.selected_candidates);
    batch.dropped_by_policy = output.dropped_by_policy;
    batch.decisions = std::move(output.decisions);
    return batch;
}

} // namespace core
} // namespace autolife

