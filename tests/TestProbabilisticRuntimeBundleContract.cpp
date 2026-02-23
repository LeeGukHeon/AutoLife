#include "analytics/ProbabilisticRuntimeModel.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

namespace {

bool writeJson(const std::filesystem::path& path, const nlohmann::json& payload) {
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        return false;
    }
    out << payload.dump(2);
    return static_cast<bool>(out);
}

nlohmann::json makeMinimalV2Bundle() {
    return nlohmann::json{
        {"version", "probabilistic_runtime_bundle_v2_draft"},
        {"pipeline_version", "v2"},
        {"feature_contract_version", "v2_draft"},
        {"runtime_bundle_contract_version", "v2_draft"},
        {"feature_columns", nlohmann::json::array({"ret_1m"})},
        {"feature_transform_contract", nlohmann::json::object()},
        {"global_fallback_enabled", true},
        {"prefer_default_model", true},
        {"default_model",
            {
                {"selected_fold_id", 1},
                {"h1_model",
                    {
                        {"linear", {{"coef", nlohmann::json::array({0.1})}, {"intercept", 0.0}}},
                        {"calibration", {{"a", 1.0}, {"b", 0.0}}},
                    }},
                {"h5_model",
                    {
                        {"linear", {{"coef", nlohmann::json::array({0.2})}, {"intercept", 0.0}}},
                        {"calibration", {{"a", 1.0}, {"b", 0.0}}},
                        {"selection_threshold", 0.6},
                        {"edge_profile",
                            {
                                {"sample_count", 1},
                                {"win_count", 1},
                                {"loss_count", 0},
                                {"neutral_count", 0},
                                {"win_mean_edge_bps", 10.0},
                                {"loss_mean_edge_bps", -10.0},
                                {"neutral_mean_edge_bps", 0.0},
                            }},
                    }},
            }},
        {"markets", nlohmann::json::array()},
    };
}

bool expectLoadFail(
    const std::filesystem::path& path,
    const std::string& expected_error
) {
    autolife::analytics::ProbabilisticRuntimeModel model;
    std::string err;
    const bool ok = model.loadFromFile(path.string(), &err);
    if (ok) {
        std::cerr << "expected failure, but load succeeded: " << path.string() << "\n";
        return false;
    }
    if (err != expected_error) {
        std::cerr << "unexpected error: expected=" << expected_error << " actual=" << err << "\n";
        return false;
    }
    return true;
}

bool expectLoadPass(const std::filesystem::path& path) {
    autolife::analytics::ProbabilisticRuntimeModel model;
    std::string err;
    const bool ok = model.loadFromFile(path.string(), &err);
    if (!ok) {
        std::cerr << "expected pass, but load failed: " << err << "\n";
        return false;
    }
    if (!model.isLoaded()) {
        std::cerr << "model must be loaded when bundle load passes\n";
        return false;
    }
    return true;
}

} // namespace

int main() {
    const auto temp_dir = std::filesystem::temp_directory_path() / "autolife_prob_bundle_contract_tests";
    std::error_code ec;
    std::filesystem::create_directories(temp_dir, ec);
    if (ec) {
        std::cerr << "failed to create temp directory: " << temp_dir.string() << "\n";
        return 1;
    }

    // Case 1: v2 must include explicit pipeline_version.
    {
        auto payload = makeMinimalV2Bundle();
        payload.erase("pipeline_version");
        const auto path = temp_dir / "bundle_missing_pipeline.json";
        if (!writeJson(path, payload)) {
            std::cerr << "failed to write test json: " << path.string() << "\n";
            return 1;
        }
        if (!expectLoadFail(path, "missing_pipeline_version_for_v2")) {
            return 1;
        }
    }

    // Case 2: v2 must include explicit feature contract version.
    {
        auto payload = makeMinimalV2Bundle();
        payload.erase("feature_contract_version");
        const auto path = temp_dir / "bundle_missing_feature_contract.json";
        if (!writeJson(path, payload)) {
            std::cerr << "failed to write test json: " << path.string() << "\n";
            return 1;
        }
        if (!expectLoadFail(path, "missing_feature_contract_version_for_v2")) {
            return 1;
        }
    }

    // Case 3: v2 must include explicit runtime bundle contract version.
    {
        auto payload = makeMinimalV2Bundle();
        payload.erase("runtime_bundle_contract_version");
        const auto path = temp_dir / "bundle_missing_runtime_contract.json";
        if (!writeJson(path, payload)) {
            std::cerr << "failed to write test json: " << path.string() << "\n";
            return 1;
        }
        if (!expectLoadFail(path, "missing_runtime_bundle_contract_version_for_v2")) {
            return 1;
        }
    }

    // Case 4: valid minimal v2 bundle loads.
    {
        const auto payload = makeMinimalV2Bundle();
        const auto path = temp_dir / "bundle_valid_v2.json";
        if (!writeJson(path, payload)) {
            std::cerr << "failed to write test json: " << path.string() << "\n";
            return 1;
        }
        if (!expectLoadPass(path)) {
            return 1;
        }
    }

    std::cout << "Probabilistic runtime bundle contract test passed\n";
    return 0;
}
