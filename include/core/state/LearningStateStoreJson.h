#pragma once

#include <filesystem>
#include <optional>

#include "core/contracts/ILearningStateStore.h"

namespace autolife {
namespace core {

class LearningStateStoreJson : public ILearningStateStore {
public:
    explicit LearningStateStoreJson(std::filesystem::path file_path);

    std::optional<LearningStateSnapshot> load() override;
    bool save(const LearningStateSnapshot& snapshot) override;

private:
    std::filesystem::path file_path_;
};

} // namespace core
} // namespace autolife

