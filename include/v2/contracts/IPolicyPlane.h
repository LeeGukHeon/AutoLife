#pragma once

#include <vector>

#include "v2/model/KernelTypes.h"

namespace autolife {
namespace v2 {

class IPolicyPlane {
public:
    virtual ~IPolicyPlane() = default;

    virtual PolicyDecisionBatch selectCandidates(
        const std::vector<SignalCandidate>& candidates,
        const PolicyContext& context
    ) = 0;
};

} // namespace v2
} // namespace autolife

