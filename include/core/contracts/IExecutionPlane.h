#pragma once

#include <string>
#include <vector>

#include "core/model/PlaneTypes.h"

namespace autolife {
namespace core {

class IExecutionPlane {
public:
    virtual ~IExecutionPlane() = default;

    virtual bool submit(const ExecutionRequest& request) = 0;
    virtual bool cancel(const std::string& order_id) = 0;
    virtual void poll() = 0;
    virtual std::vector<ExecutionUpdate> drainUpdates() = 0;
};

} // namespace core
} // namespace autolife

