#include "core/adapters/LegacyRiskCompliancePlaneAdapter.h"

namespace autolife {
namespace core {

LegacyRiskCompliancePlaneAdapter::LegacyRiskCompliancePlaneAdapter(
    risk::RiskManager& risk_manager,
    const engine::EngineConfig& config
)
    : risk_manager_(risk_manager)
    , config_(config) {}

PreTradeCheck LegacyRiskCompliancePlaneAdapter::validateEntry(
    const ExecutionRequest& request,
    const strategy::Signal& signal
) {
    if (request.market.empty() || request.price <= 0.0 || request.volume <= 0.0) {
        return {false, "invalid_request"};
    }

    if (signal.position_size <= 0.0) {
        return {false, "invalid_position_size"};
    }

    const bool allowed = risk_manager_.canEnterPosition(
        request.market,
        request.price,
        signal.position_size,
        signal.strategy_name
    );

    if (!allowed) {
        return {false, "risk_rejected"};
    }

    (void)config_;
    return {true, "ok"};
}

PreTradeCheck LegacyRiskCompliancePlaneAdapter::validateExit(
    const std::string& market,
    const risk::Position& position,
    double exit_price
) {
    if (market.empty() || exit_price <= 0.0) {
        return {false, "invalid_exit_request"};
    }

    if (position.quantity <= 0.0) {
        return {false, "empty_position"};
    }

    return {true, "ok"};
}

} // namespace core
} // namespace autolife

