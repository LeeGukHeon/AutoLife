#include "core/state/LearningStateStoreJson.h"

#include <fstream>
#include <system_error>

namespace autolife {
namespace core {

LearningStateStoreJson::LearningStateStoreJson(std::filesystem::path file_path)
    : file_path_(std::move(file_path)) {}

std::optional<LearningStateSnapshot> LearningStateStoreJson::load() {
    if (!std::filesystem::exists(file_path_)) {
        return std::nullopt;
    }

    std::ifstream in(file_path_, std::ios::binary);
    if (!in.is_open()) {
        return std::nullopt;
    }

    nlohmann::json raw;
    in >> raw;

    LearningStateSnapshot snapshot;
    snapshot.schema_version = raw.value("schema_version", 1);
    snapshot.saved_at_ms = raw.value("saved_at_ms", 0LL);
    snapshot.policy_params = raw.value("policy_params", nlohmann::json::object());
    snapshot.bucket_stats = raw.value("bucket_stats", nlohmann::json::array());
    snapshot.rollback_point = raw.value("rollback_point", nlohmann::json::object());
    return snapshot;
}

bool LearningStateStoreJson::save(const LearningStateSnapshot& snapshot) {
    nlohmann::json raw;
    raw["schema_version"] = snapshot.schema_version;
    raw["saved_at_ms"] = snapshot.saved_at_ms;
    raw["policy_params"] = snapshot.policy_params;
    raw["bucket_stats"] = snapshot.bucket_stats;
    raw["rollback_point"] = snapshot.rollback_point;

    std::filesystem::create_directories(file_path_.parent_path());

    auto tmp_path = file_path_;
    tmp_path += ".tmp";

    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            return false;
        }
        out << raw.dump(2);
    }

    std::error_code ec;
    std::filesystem::rename(tmp_path, file_path_, ec);
    if (!ec) {
        return true;
    }

    // Windows can fail rename over existing file; fallback to copy+remove.
    ec.clear();
    std::filesystem::copy_file(
        tmp_path,
        file_path_,
        std::filesystem::copy_options::overwrite_existing,
        ec
    );
    if (ec) {
        return false;
    }

    std::filesystem::remove(tmp_path, ec);
    return true;
}

} // namespace core
} // namespace autolife

