#pragma once

#include <optional>

#include <nlohmann/json.hpp>

namespace autolife {
namespace core {

struct LearningStateSnapshot {
    int schema_version = 1;
    long long saved_at_ms = 0;
    nlohmann::json policy_params;
    nlohmann::json bucket_stats;
    nlohmann::json rollback_point;
};

class ILearningStateStore {
public:
    virtual ~ILearningStateStore() = default;

    virtual std::optional<LearningStateSnapshot> load() = 0;
    virtual bool save(const LearningStateSnapshot& snapshot) = 0;
};

} // namespace core
} // namespace autolife

