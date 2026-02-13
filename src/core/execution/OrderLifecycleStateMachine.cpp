#include "core/execution/OrderLifecycleStateMachine.h"

#include <algorithm>
#include <cctype>

namespace autolife {
namespace core {
namespace execution {

namespace {
std::string normalizeEvent(std::string event) {
    std::transform(event.begin(), event.end(), event.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return event;
}
} // namespace

OrderLifecycleTransitionResult OrderLifecycleStateMachine::transition(
    const std::string& event,
    double current_filled_volume,
    double order_volume,
    double executed_volume,
    double remaining_volume
) {
    OrderLifecycleTransitionResult result;
    result.filled_volume = current_filled_volume;
    if (executed_volume > 0.0) {
        result.filled_volume = std::max(result.filled_volume, executed_volume);
    }
    if (remaining_volume > 0.0 && order_volume > remaining_volume) {
        result.filled_volume = std::max(result.filled_volume, order_volume - remaining_volume);
    }

    const std::string normalized_event = normalizeEvent(event);

    if (normalized_event == "filled" || normalized_event == "done") {
        result.status = OrderStatus::FILLED;
        result.filled_volume = (result.filled_volume > 0.0) ? result.filled_volume : order_volume;
        result.terminal = true;
        return result;
    }

    if (normalized_event == "cancel" || normalized_event == "cancelled") {
        result.status = OrderStatus::CANCELLED;
        result.terminal = true;
        return result;
    }

    if (normalized_event == "rejected" || normalized_event == "reject" || normalized_event == "prevented") {
        result.status = OrderStatus::REJECTED;
        result.terminal = true;
        return result;
    }

    if (normalized_event == "partially_filled" || normalized_event == "partial_fill" ||
        normalized_event == "wait" || normalized_event == "watch" || normalized_event == "trade") {
        if (result.filled_volume >= order_volume - 1e-8) {
            result.status = OrderStatus::FILLED;
            result.terminal = true;
        } else if (result.filled_volume > 0.0) {
            result.status = OrderStatus::PARTIALLY_FILLED;
        } else {
            result.status = OrderStatus::SUBMITTED;
        }
        return result;
    }

    if (normalized_event == "submitted" || normalized_event == "pending" || normalized_event == "new") {
        result.status = (result.filled_volume > 0.0) ? OrderStatus::PARTIALLY_FILLED : OrderStatus::SUBMITTED;
        return result;
    }

    result.status = (result.filled_volume > 0.0) ? OrderStatus::PARTIALLY_FILLED : OrderStatus::SUBMITTED;
    return result;
}

} // namespace execution
} // namespace core
} // namespace autolife

