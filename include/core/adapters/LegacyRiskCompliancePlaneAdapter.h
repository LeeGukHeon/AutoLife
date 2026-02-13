#pragma once

#include "core/contracts/IRiskCompliancePlane.h"
#include "engine/EngineConfig.h"
#include "risk/RiskManager.h"

namespace autolife {
namespace core {

class LegacyRiskCompliancePlaneAdapter : public IRiskCompliancePlane {
public:
    LegacyRiskCompliancePlaneAdapter(
        risk::RiskManager& risk_manager,
        const engine::EngineConfig& config
    );

    PreTradeCheck validateEntry(
        const ExecutionRequest& request,
        const strategy::Signal& signal
    ) override;

    PreTradeCheck validateExit(
        const std::string& market,
        const risk::Position& position,
        double exit_price
    ) override;

private:
    risk::RiskManager& risk_manager_;
    const engine::EngineConfig& config_;
};

} // namespace core
} // namespace autolife

