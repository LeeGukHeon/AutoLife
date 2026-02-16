#pragma once

#include "risk/RiskManager.h"
#include "v2/contracts/IRiskPlane.h"

namespace autolife {
namespace v2 {

class LegacyRiskPlaneAdapter : public IRiskPlane {
public:
    explicit LegacyRiskPlaneAdapter(risk::RiskManager& risk_manager);

    RiskCheck validateEntry(
        const ExecutionIntent& intent,
        const SignalCandidate& candidate,
        const RiskContext& context
    ) override;

    RiskCheck validateExit(
        const ExecutionIntent& intent,
        const PositionSnapshot& position,
        const RiskContext& context
    ) override;

private:
    risk::RiskManager& risk_manager_;
};

} // namespace v2
} // namespace autolife

