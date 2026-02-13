#include "execution/OrderStateMapper.h"

#include "core/execution/OrderLifecycleStateMachine.h"

namespace autolife {
namespace execution {

ExchangeOrderStateResult OrderStateMapper::map(
    const std::string& exchange_state,
    double current_filled_volume,
    double order_volume,
    double executed_volume,
    double remaining_volume
) {
    const auto transitioned = core::execution::OrderLifecycleStateMachine::transition(
        exchange_state,
        current_filled_volume,
        order_volume,
        executed_volume,
        remaining_volume
    );

    ExchangeOrderStateResult result;
    result.status = transitioned.status;
    result.filled_volume = transitioned.filled_volume;
    result.terminal = transitioned.terminal;
    return result;
}

} // namespace execution
} // namespace autolife
