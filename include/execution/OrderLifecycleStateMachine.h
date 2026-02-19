#pragma once

#include <string>

#include "common/Types.h"

namespace autolife {
namespace core {
namespace execution {

struct OrderLifecycleTransitionResult {
    OrderStatus status = OrderStatus::SUBMITTED;
    double filled_volume = 0.0;
    bool terminal = false;
};

class OrderLifecycleStateMachine {
public:
    static OrderLifecycleTransitionResult transition(
        const std::string& event,
        double current_filled_volume,
        double order_volume,
        double executed_volume = 0.0,
        double remaining_volume = 0.0
    );
};

} // namespace execution
} // namespace core
} // namespace autolife

