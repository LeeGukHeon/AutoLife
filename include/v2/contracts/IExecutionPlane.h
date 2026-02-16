#pragma once

#include <string>
#include <vector>

#include "v2/model/KernelTypes.h"

namespace autolife {
namespace v2 {

class IExecutionPlane {
public:
    virtual ~IExecutionPlane() = default;

    virtual bool submit(const ExecutionIntent& intent) = 0;
    virtual bool cancel(const std::string& order_id) = 0;
    virtual void poll() = 0;
    virtual std::vector<ExecutionEvent> drainEvents() = 0;
};

} // namespace v2
} // namespace autolife

