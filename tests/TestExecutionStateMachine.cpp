#include "core/execution/OrderLifecycleStateMachine.h"

#include <cassert>
#include <iostream>

using autolife::OrderStatus;
using autolife::core::execution::OrderLifecycleStateMachine;

int main() {
    {
        auto r = OrderLifecycleStateMachine::transition("submitted", 0.0, 1.0, 0.0, 1.0);
        assert(r.status == OrderStatus::SUBMITTED);
        assert(!r.terminal);
        assert(r.filled_volume == 0.0);
    }

    {
        auto r = OrderLifecycleStateMachine::transition("trade", 0.0, 2.0, 0.5, 1.5);
        assert(r.status == OrderStatus::PARTIALLY_FILLED);
        assert(!r.terminal);
        assert(r.filled_volume > 0.49 && r.filled_volume < 0.51);
    }

    {
        auto r = OrderLifecycleStateMachine::transition("done", 0.0, 1.0, 0.0, 0.0);
        assert(r.status == OrderStatus::FILLED);
        assert(r.terminal);
        assert(r.filled_volume == 1.0);
    }

    {
        auto r = OrderLifecycleStateMachine::transition("cancelled", 0.2, 1.0, 0.2, 0.8);
        assert(r.status == OrderStatus::CANCELLED);
        assert(r.terminal);
    }

    {
        auto r = OrderLifecycleStateMachine::transition("rejected", 0.0, 1.0, 0.0, 1.0);
        assert(r.status == OrderStatus::REJECTED);
        assert(r.terminal);
    }

    std::cout << "[TEST] ExecutionStateMachine PASSED\n";
    return 0;
}

