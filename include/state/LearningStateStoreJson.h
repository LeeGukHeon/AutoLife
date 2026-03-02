#pragma once

#include <filesystem>
#include <optional>

#include "state/ILearningStateStore.h"

namespace autolife {
namespace core {

class LearningStateStoreJson : public ILearningStateStore {
public:
    static constexpr int kCurrentSchemaVersion = 1;

    explicit LearningStateStoreJson(std::filesystem::path file_path);

    std::optional<LearningStateSnapshot> load() override;
    bool save(const LearningStateSnapshot& snapshot) override;

private:
    static bool migrateSchemaToCurrent(nlohmann::json& raw);
    static bool validateCurrentSchema(const nlohmann::json& raw);

    std::filesystem::path file_path_;
};

} // namespace core
} // namespace autolife

