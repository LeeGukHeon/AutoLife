#pragma once

#include "common/Types.h"
#include <string>

namespace autolife {
namespace execution {

struct ExchangeOrderStateResult {
    OrderStatus status = OrderStatus::SUBMITTED;
    double filled_volume = 0.0;
    bool terminal = false;
};

class OrderStateMapper {
public:
    static ExchangeOrderStateResult map(
        const std::string& exchange_state,
        double current_filled_volume,
        double order_volume,
        double executed_volume,
        double remaining_volume
    );
};

} // namespace execution
} // namespace autolife
