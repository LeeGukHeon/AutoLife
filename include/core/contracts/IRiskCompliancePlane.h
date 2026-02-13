#pragma once

#include <string>

#include "core/model/PlaneTypes.h"
#include "risk/RiskManager.h"
#include "strategy/IStrategy.h"

namespace autolife {
namespace core {

class IRiskCompliancePlane {
public:
    virtual ~IRiskCompliancePlane() = default;

    virtual PreTradeCheck validateEntry(
        const ExecutionRequest& request,
        const strategy::Signal& signal
    ) = 0;

    virtual PreTradeCheck validateExit(
        const std::string& market,
        const risk::Position& position,
        double exit_price
    ) = 0;
};

} // namespace core
} // namespace autolife

