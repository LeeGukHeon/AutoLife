#pragma once

#include <optional>

#include "v2/model/KernelTypes.h"

namespace autolife {
namespace v2 {

class ILearningStateStore {
public:
    virtual ~ILearningStateStore() = default;

    virtual std::optional<LearningStateSnapshot> load() = 0;
    virtual bool save(const LearningStateSnapshot& snapshot) = 0;
};

} // namespace v2
} // namespace autolife

