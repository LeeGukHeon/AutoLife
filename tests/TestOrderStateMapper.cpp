#include "execution/OrderStateMapper.h"

#include <cassert>
#include <iostream>

using autolife::OrderStatus;
using autolife::execution::OrderStateMapper;

int main() {
    {
        auto r = OrderStateMapper::map("done", 0.0, 1.0, 0.0, 0.0);
        assert(r.status == OrderStatus::FILLED);
        assert(r.terminal);
        assert(r.filled_volume == 1.0);
    }

    {
        auto r = OrderStateMapper::map("trade", 0.0, 2.0, 0.4, 1.6);
        assert(r.status == OrderStatus::PARTIALLY_FILLED);
        assert(!r.terminal);
        assert(r.filled_volume > 0.39 && r.filled_volume < 0.41);
    }

    {
        auto r = OrderStateMapper::map("cancel", 0.2, 1.0, 0.2, 0.8);
        assert(r.status == OrderStatus::CANCELLED);
        assert(r.terminal);
    }

    {
        auto r = OrderStateMapper::map("prevented", 0.0, 1.0, 0.0, 1.0);
        assert(r.status == OrderStatus::REJECTED);
        assert(r.terminal);
    }

    std::cout << "[TEST] OrderStateMapper PASSED\n";
    return 0;
}
