#include "v2/adapters/LegacyExecutionPlaneAdapter.h"

#include <cmath>
#include <utility>

namespace autolife {
namespace v2 {
namespace {

OrderSide toLegacySide(ExecutionSide side) {
    return (side == ExecutionSide::SELL) ? OrderSide::SELL : OrderSide::BUY;
}

ExecutionStatus fromLegacyStatus(OrderStatus status) {
    switch (status) {
        case OrderStatus::PENDING: return ExecutionStatus::PENDING;
        case OrderStatus::SUBMITTED: return ExecutionStatus::SUBMITTED;
        case OrderStatus::PARTIALLY_FILLED: return ExecutionStatus::PARTIALLY_FILLED;
        case OrderStatus::FILLED: return ExecutionStatus::FILLED;
        case OrderStatus::CANCELLED: return ExecutionStatus::CANCELLED;
        case OrderStatus::REJECTED:
        default:
            return ExecutionStatus::REJECTED;
    }
}

} // namespace

LegacyExecutionPlaneAdapter::LegacyExecutionPlaneAdapter(execution::OrderManager& order_manager)
    : order_manager_(order_manager) {}

bool LegacyExecutionPlaneAdapter::submit(const ExecutionIntent& intent) {
    if (intent.market.empty() || intent.limit_price <= 0.0) {
        return false;
    }

    double volume = intent.order_volume;
    if (volume <= 0.0 && intent.order_notional_krw > 0.0 && intent.limit_price > 0.0) {
        volume = intent.order_notional_krw / intent.limit_price;
    }
    if (!std::isfinite(volume) || volume <= 0.0) {
        return false;
    }

    return order_manager_.submitOrder(
        intent.market,
        toLegacySide(intent.side),
        intent.limit_price,
        volume,
        intent.strategy_id,
        intent.stop_loss,
        intent.take_profit_1,
        intent.take_profit_2,
        0.0,
        0.0
    );
}

bool LegacyExecutionPlaneAdapter::cancel(const std::string& order_id) {
    return order_manager_.cancelOrder(order_id);
}

void LegacyExecutionPlaneAdapter::poll() {
    order_manager_.monitorOrders();
}

std::vector<ExecutionEvent> LegacyExecutionPlaneAdapter::drainEvents() {
    std::vector<ExecutionEvent> out;
    auto filled_orders = order_manager_.getFilledOrders();
    out.reserve(filled_orders.size());

    for (const auto& order : filled_orders) {
        ExecutionEvent event;
        event.event_id = order.order_id + ":drain";
        event.intent_id = order.order_id;
        event.order_id = order.order_id;
        event.market = order.market;
        event.side = (order.side == OrderSide::SELL) ? ExecutionSide::SELL : ExecutionSide::BUY;
        event.status = fromLegacyStatus(order.status);
        event.filled_volume = order.filled_volume;
        event.order_volume = order.volume;
        event.avg_price = order.price;
        event.terminal =
            (event.status == ExecutionStatus::FILLED) ||
            (event.status == ExecutionStatus::CANCELLED) ||
            (event.status == ExecutionStatus::REJECTED);
        event.source = "legacy_order_manager_drain";
        event.reason_code = event.terminal ? "terminal" : "update";
        event.ts_ms = order.last_state_sync_ms;
        out.push_back(std::move(event));
    }

    return out;
}

} // namespace v2
} // namespace autolife
