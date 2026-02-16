#include "v2/adapters/LegacyRiskPlaneAdapter.h"

#include <algorithm>

namespace autolife {
namespace v2 {

LegacyRiskPlaneAdapter::LegacyRiskPlaneAdapter(risk::RiskManager& risk_manager)
    : risk_manager_(risk_manager) {}

RiskCheck LegacyRiskPlaneAdapter::validateEntry(
    const ExecutionIntent& intent,
    const SignalCandidate& candidate,
    const RiskContext& context
) {
    RiskCheck out;
    if (intent.market.empty() || intent.limit_price <= 0.0) {
        out.allowed = false;
        out.reason_code = "invalid_entry_request";
        return out;
    }

    double position_fraction = candidate.position_fraction;
    if (position_fraction <= 0.0 &&
        intent.order_notional_krw > 0.0 &&
        context.available_capital_krw > 1e-9) {
        position_fraction = intent.order_notional_krw / context.available_capital_krw;
    }
    position_fraction = std::clamp(position_fraction, 0.0, 1.0);
    out.max_position_fraction = position_fraction;
    out.suggested_order_krw = intent.order_notional_krw;

    const bool allowed = risk_manager_.canEnterPosition(
        intent.market,
        intent.limit_price,
        position_fraction,
        candidate.strategy_id
    );
    out.allowed = allowed;
    out.reason_code = allowed ? "ok" : "risk_rejected";
    return out;
}

RiskCheck LegacyRiskPlaneAdapter::validateExit(
    const ExecutionIntent& intent,
    const PositionSnapshot& position,
    const RiskContext& context
) {
    (void)context;

    RiskCheck out;
    if (intent.market.empty() || position.market.empty()) {
        out.allowed = false;
        out.reason_code = "invalid_exit_request";
        return out;
    }
    if (position.quantity <= 0.0) {
        out.allowed = false;
        out.reason_code = "empty_position";
        return out;
    }
    out.allowed = true;
    out.reason_code = "ok";
    out.max_position_fraction = 1.0;
    out.suggested_order_krw = 0.0;
    return out;
}

} // namespace v2
} // namespace autolife

