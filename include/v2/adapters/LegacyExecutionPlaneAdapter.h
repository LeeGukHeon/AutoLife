#pragma once

#include "execution/OrderManager.h"
#include "v2/contracts/IExecutionPlane.h"

namespace autolife {
namespace v2 {

class LegacyExecutionPlaneAdapter : public IExecutionPlane {
public:
    explicit LegacyExecutionPlaneAdapter(execution::OrderManager& order_manager);

    bool submit(const ExecutionIntent& intent) override;
    bool cancel(const std::string& order_id) override;
    void poll() override;
    std::vector<ExecutionEvent> drainEvents() override;

private:
    execution::OrderManager& order_manager_;
};

} // namespace v2
} // namespace autolife

