#pragma once

#include "core/contracts/IExecutionPlane.h"
#include "execution/OrderManager.h"

namespace autolife {
namespace core {

class LegacyExecutionPlaneAdapter : public IExecutionPlane {
public:
    explicit LegacyExecutionPlaneAdapter(autolife::execution::OrderManager& order_manager);

    bool submit(const ExecutionRequest& request) override;
    bool cancel(const std::string& order_id) override;
    void poll() override;
    std::vector<ExecutionUpdate> drainUpdates() override;

private:
    autolife::execution::OrderManager& order_manager_;
};

} // namespace core
} // namespace autolife
