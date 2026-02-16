#pragma once

#include <vector>

#include "core/model/PlaneTypes.h"
#include "v2/strategy/IStrategy.h"

namespace autolife {
namespace core {

class IPolicyLearningPlane {
public:
    virtual ~IPolicyLearningPlane() = default;

    virtual PolicyDecisionBatch selectCandidates(
        const std::vector<strategy::Signal>& candidates,
        const PolicyContext& context
    ) = 0;
};

} // namespace core
} // namespace autolife


