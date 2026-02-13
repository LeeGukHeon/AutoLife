#include "core/adapters/LegacyExecutionPlaneAdapter.h"
#include "core/execution/ExecutionUpdateSchema.h"

namespace autolife {
namespace core {

LegacyExecutionPlaneAdapter::LegacyExecutionPlaneAdapter(autolife::execution::OrderManager& order_manager)
    : order_manager_(order_manager) {}

bool LegacyExecutionPlaneAdapter::submit(const ExecutionRequest& request) {
    return order_manager_.submitOrder(
        request.market,
        request.side,
        request.price,
        request.volume,
        request.strategy_name,
        request.stop_loss,
        request.take_profit_1,
        request.take_profit_2,
        request.breakeven_trigger,
        request.trailing_start
    );
}

bool LegacyExecutionPlaneAdapter::cancel(const std::string& order_id) {
    return order_manager_.cancelOrder(order_id);
}

void LegacyExecutionPlaneAdapter::poll() {
    order_manager_.monitorOrders();
}

std::vector<ExecutionUpdate> LegacyExecutionPlaneAdapter::drainUpdates() {
    std::vector<ExecutionUpdate> updates;
    auto filled = order_manager_.getFilledOrders();
    updates.reserve(filled.size());
    for (const auto& order : filled) {
        const bool fully_filled = (order.volume > 0.0) && (order.filled_volume >= order.volume - 1e-8);
        const OrderStatus normalized_status = fully_filled ? OrderStatus::FILLED : OrderStatus::PARTIALLY_FILLED;
        const std::string event = fully_filled ? "filled" : "partial_fill";
        const bool terminal = fully_filled ||
            order.status == OrderStatus::CANCELLED ||
            order.status == OrderStatus::REJECTED;

        updates.push_back(execution::makeExecutionUpdate(
            "live_drain",
            event,
            order.order_id,
            order.market,
            order.side,
            normalized_status,
            order.filled_volume,
            order.volume,
            order.price,
            order.strategy_name,
            terminal,
            order.last_state_sync_ms
        ));
    }
    return updates;
}

} // namespace core
} // namespace autolife
