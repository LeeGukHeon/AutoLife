#pragma once

#include "v2/model/KernelTypes.h"

namespace autolife {
namespace v2 {

class IRiskPlane {
public:
    virtual ~IRiskPlane() = default;

    virtual RiskCheck validateEntry(
        const ExecutionIntent& intent,
        const SignalCandidate& candidate,
        const RiskContext& context
    ) = 0;

    virtual RiskCheck validateExit(
        const ExecutionIntent& intent,
        const PositionSnapshot& position,
        const RiskContext& context
    ) = 0;
};

} // namespace v2
} // namespace autolife

