#include "state/LearningStateStoreJson.h"

#include "common/Logger.h"

#include <fstream>
#include <system_error>

namespace autolife {
namespace core {

LearningStateStoreJson::LearningStateStoreJson(std::filesystem::path file_path)
    : file_path_(std::move(file_path)) {}

bool LearningStateStoreJson::migrateLegacySchemaToCurrent(nlohmann::json& raw) {
    if (!raw.is_object()) {
        return false;
    }

    const int schema_version = raw.value("schema_version", 0);
    if (schema_version == kCurrentSchemaVersion) {
        return true;
    }
    if (schema_version > kCurrentSchemaVersion) {
        return false;
    }

    // Legacy/unversioned payload migration:
    // - promote root-level adaptive params into policy_params
    // - normalize required fields and stamp current schema version
    nlohmann::json policy = nlohmann::json::object();
    if (raw.contains("policy_params") && raw["policy_params"].is_object()) {
        policy = raw["policy_params"];
    }

    constexpr const char* kPolicyKeys[] = {
        "market_hostility_ewma"
    };
    for (const char* key : kPolicyKeys) {
        if (raw.contains(key)) {
            policy[key] = raw[key];
        }
    }
    raw["policy_params"] = std::move(policy);

    if (!raw.contains("saved_at_ms") || !raw["saved_at_ms"].is_number_integer()) {
        raw["saved_at_ms"] = 0LL;
    }
    if (!raw.contains("bucket_stats") || !raw["bucket_stats"].is_array()) {
        raw["bucket_stats"] = nlohmann::json::array();
    }
    if (!raw.contains("rollback_point") || !raw["rollback_point"].is_object()) {
        nlohmann::json rollback = nlohmann::json::object();
        const auto& migrated_policy = raw["policy_params"];
        for (const char* key : kPolicyKeys) {
            if (migrated_policy.contains(key)) {
                rollback[key] = migrated_policy[key];
            }
        }
        raw["rollback_point"] = std::move(rollback);
    }

    raw["schema_version"] = kCurrentSchemaVersion;
    return true;
}

bool LearningStateStoreJson::validateCurrentSchema(const nlohmann::json& raw) {
    if (!raw.is_object()) {
        return false;
    }

    if (!raw.contains("schema_version") || !raw["schema_version"].is_number_integer()) {
        return false;
    }
    if (raw["schema_version"].get<int>() != kCurrentSchemaVersion) {
        return false;
    }
    if (!raw.contains("saved_at_ms") || !raw["saved_at_ms"].is_number_integer()) {
        return false;
    }
    if (!raw.contains("policy_params") || !raw["policy_params"].is_object()) {
        return false;
    }
    if (!raw.contains("bucket_stats") || !raw["bucket_stats"].is_array()) {
        return false;
    }
    if (!raw.contains("rollback_point") || !raw["rollback_point"].is_object()) {
        return false;
    }
    return true;
}

std::optional<LearningStateSnapshot> LearningStateStoreJson::load() {
    if (!std::filesystem::exists(file_path_)) {
        return std::nullopt;
    }

    std::ifstream in(file_path_, std::ios::binary);
    if (!in.is_open()) {
        return std::nullopt;
    }

    nlohmann::json raw;
    try {
        in >> raw;
    } catch (const std::exception& e) {
        LOG_WARN("Learning state parse failed: path={}, error={}", file_path_.string(), e.what());
        return std::nullopt;
    }

    const int original_schema_version = raw.value("schema_version", 0);
    if (!migrateLegacySchemaToCurrent(raw)) {
        LOG_WARN(
            "Learning state schema not supported: path={}, schema_version={}, supported={}",
            file_path_.string(),
            original_schema_version,
            kCurrentSchemaVersion
        );
        return std::nullopt;
    }
    if (!validateCurrentSchema(raw)) {
        LOG_WARN("Learning state schema validation failed: path={}", file_path_.string());
        return std::nullopt;
    }

    LearningStateSnapshot snapshot;
    snapshot.schema_version = raw["schema_version"].get<int>();
    snapshot.saved_at_ms = raw["saved_at_ms"].get<long long>();
    snapshot.policy_params = raw["policy_params"];
    snapshot.bucket_stats = raw["bucket_stats"];
    snapshot.rollback_point = raw["rollback_point"];

    if (original_schema_version != kCurrentSchemaVersion) {
        if (!save(snapshot)) {
            LOG_WARN("Learning state migration write-back failed: path={}", file_path_.string());
        } else {
            LOG_INFO(
                "Learning state migrated to schema v{}: path={}",
                kCurrentSchemaVersion,
                file_path_.string()
            );
        }
    }

    return snapshot;
}

bool LearningStateStoreJson::save(const LearningStateSnapshot& snapshot) {
    const nlohmann::json safe_policy =
        snapshot.policy_params.is_object() ? snapshot.policy_params : nlohmann::json::object();
    const nlohmann::json safe_bucket_stats =
        snapshot.bucket_stats.is_array() ? snapshot.bucket_stats : nlohmann::json::array();
    const nlohmann::json safe_rollback =
        snapshot.rollback_point.is_object() ? snapshot.rollback_point : nlohmann::json::object();

    nlohmann::json raw;
    raw["schema_version"] = kCurrentSchemaVersion;
    raw["saved_at_ms"] = snapshot.saved_at_ms;
    raw["policy_params"] = safe_policy;
    raw["bucket_stats"] = safe_bucket_stats;
    raw["rollback_point"] = safe_rollback;

    if (file_path_.has_parent_path()) {
        std::filesystem::create_directories(file_path_.parent_path());
    }

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
        std::error_code rm_ec;
        std::filesystem::remove(tmp_path, rm_ec);
        return false;
    }

    std::filesystem::remove(tmp_path, ec);
    return true;
}

} // namespace core
} // namespace autolife

