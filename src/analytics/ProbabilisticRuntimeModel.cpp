#include "analytics/ProbabilisticRuntimeModel.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <numeric>
#include <string_view>

#include <nlohmann/json.hpp>

namespace autolife {
namespace analytics {

namespace {

double clipProb(double v) {
    constexpr double kEps = 1e-6;
    if (!std::isfinite(v)) {
        return 0.5;
    }
    return std::clamp(v, kEps, 1.0 - kEps);
}

double sigmoid(double z) {
    if (!std::isfinite(z)) {
        return 0.5;
    }
    const double clamped = std::clamp(z, -40.0, 40.0);
    return 1.0 / (1.0 + std::exp(-clamped));
}

double linearScore(const std::vector<double>& coef, double intercept, const std::vector<double>& x) {
    if (coef.size() != x.size()) {
        return intercept;
    }
    const double dot = std::inner_product(coef.begin(), coef.end(), x.begin(), 0.0);
    return dot + intercept;
}

double calibrateProb(double p, double a, double b) {
    const double p_clip = clipProb(p);
    const double logit = std::log(p_clip / (1.0 - p_clip));
    return clipProb(sigmoid((a * logit) + b));
}

bool parseHead(const nlohmann::json& node, ProbabilisticRuntimeModel::LinearHead& out, bool with_threshold) {
    if (!node.is_object()) {
        return false;
    }
    if (!node.contains("linear") || !node["linear"].is_object()) {
        return false;
    }
    const auto& linear = node["linear"];
    if (!linear.contains("coef") || !linear["coef"].is_array()) {
        return false;
    }
    out.coef.clear();
    out.coef.reserve(linear["coef"].size());
    for (const auto& v : linear["coef"]) {
        out.coef.push_back(v.get<double>());
    }
    out.intercept = linear.value("intercept", 0.0);

    const auto calib = node.value("calibration", nlohmann::json::object());
    out.calib_a = calib.value("a", 1.0);
    out.calib_b = calib.value("b", 0.0);
    if (with_threshold) {
        out.threshold = node.value("selection_threshold", 0.6);
        out.has_edge_regressor = false;
        out.edge_coef.clear();
        out.edge_intercept = 0.0;
        out.edge_clip_abs_bps = 250.0;

        const auto edge_reg = node.value("edge_regressor", nlohmann::json::object());
        if (edge_reg.is_object()) {
            const auto linear_reg = edge_reg.value("linear", nlohmann::json::object());
            if (linear_reg.is_object() && linear_reg.contains("coef") && linear_reg["coef"].is_array()) {
                for (const auto& v : linear_reg["coef"]) {
                    out.edge_coef.push_back(v.get<double>());
                }
                if (!out.edge_coef.empty()) {
                    out.has_edge_regressor = true;
                    out.edge_intercept = linear_reg.value("intercept", 0.0);
                    double clip_abs = edge_reg.value("clip_abs_bps", 250.0);
                    if (!std::isfinite(clip_abs)) {
                        clip_abs = 250.0;
                    }
                    out.edge_clip_abs_bps = std::clamp(clip_abs, 10.0, 5000.0);
                } else {
                    out.edge_coef.clear();
                }
            }
        }
        const auto edge_profile = node.value("edge_profile", nlohmann::json::object());
        out.edge_win_mean_bps = edge_profile.value("win_mean_edge_bps", 0.0);
        out.edge_loss_mean_bps = edge_profile.value("loss_mean_edge_bps", 0.0);
        out.edge_neutral_mean_bps = edge_profile.value("neutral_mean_edge_bps", 0.0);
    }
    return !out.coef.empty();
}

double meanOrDefault(const std::vector<double>& values, double fallback) {
    if (values.empty()) {
        return fallback;
    }
    double sum = 0.0;
    for (double v : values) {
        sum += v;
    }
    return sum / static_cast<double>(values.size());
}

double stdOrZero(const std::vector<double>& values, double mean) {
    if (values.size() < 2) {
        return 0.0;
    }
    double acc = 0.0;
    for (double v : values) {
        const double d = v - mean;
        acc += (d * d);
    }
    const double var = acc / static_cast<double>(values.size());
    return std::sqrt(std::max(0.0, var));
}

void parseEnsembleMembers(
    const nlohmann::json& parent_node,
    std::size_t feature_count,
    std::vector<ProbabilisticRuntimeModel::MarketEntry::EnsembleMember>& out_members
) {
    out_members.clear();
    const auto ensemble_members = parent_node.value("ensemble_members", nlohmann::json::array());
    if (!ensemble_members.is_array()) {
        return;
    }
    for (const auto& node : ensemble_members) {
        if (!node.is_object()) {
            continue;
        }
        ProbabilisticRuntimeModel::MarketEntry::EnsembleMember member;
        member.member_index = node.value("member_index", static_cast<int>(out_members.size()));
        if (!parseHead(node.value("h1_model", nlohmann::json::object()), member.h1, false)) {
            continue;
        }
        if (!parseHead(node.value("h5_model", nlohmann::json::object()), member.h5, true)) {
            continue;
        }
        if (member.h1.coef.size() != feature_count || member.h5.coef.size() != feature_count) {
            continue;
        }
        if (member.h5.has_edge_regressor && member.h5.edge_coef.size() != feature_count) {
            member.h5.has_edge_regressor = false;
            member.h5.edge_coef.clear();
        }
        out_members.push_back(std::move(member));
    }
}

int findFeatureIndex(const std::vector<std::string>& cols, const std::string& key) {
    for (std::size_t i = 0; i < cols.size(); ++i) {
        if (cols[i] == key) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

double parseCostParam(
    const nlohmann::json& node,
    const char* key,
    double fallback,
    double min_v,
    double max_v
) {
    double out = fallback;
    if (node.is_object() && node.contains(key)) {
        try {
            out = node.at(key).get<double>();
        } catch (...) {
            out = fallback;
        }
    }
    if (!std::isfinite(out)) {
        out = fallback;
    }
    return std::clamp(out, min_v, max_v);
}

std::string lowerCopy(std::string s);

bool featureIndexValid(const std::vector<double>& transformed_features, int idx) {
    return idx >= 0 && static_cast<std::size_t>(idx) < transformed_features.size();
}

double transformedAtrPctRaw(
    const ProbabilisticRuntimeModel::RuntimeCostModel& cfg,
    const std::vector<double>& transformed_features
) {
    if (!featureIndexValid(transformed_features, cfg.atr_pct_idx)) {
        return 0.0;
    }
    return std::max(0.0, transformed_features[static_cast<std::size_t>(cfg.atr_pct_idx)] / 100.0);
}

double transformedBbWidthRaw(
    const ProbabilisticRuntimeModel::RuntimeCostModel& cfg,
    const std::vector<double>& transformed_features
) {
    if (!featureIndexValid(transformed_features, cfg.bb_width_idx)) {
        return 0.0;
    }
    return std::max(0.0, transformed_features[static_cast<std::size_t>(cfg.bb_width_idx)] / 100.0);
}

double transformedLiquidityRatio(
    const ProbabilisticRuntimeModel::RuntimeCostModel& cfg,
    const std::vector<double>& transformed_features
) {
    if (!featureIndexValid(transformed_features, cfg.vol_ratio_idx) ||
        !featureIndexValid(transformed_features, cfg.notional_ratio_idx)) {
        return 1.0;
    }
    const double vol_ratio = std::max(
        1e-9,
        std::exp(transformed_features[static_cast<std::size_t>(cfg.vol_ratio_idx)])
    );
    const double notional_ratio = std::max(
        1e-9,
        std::exp(transformed_features[static_cast<std::size_t>(cfg.notional_ratio_idx)])
    );
    return std::sqrt(std::max(1e-9, vol_ratio * notional_ratio));
}

double transformedVolatilityBps(
    const ProbabilisticRuntimeModel::RuntimeCostModel& cfg,
    const std::vector<double>& transformed_features
) {
    return transformedAtrPctRaw(cfg, transformed_features) * 10000.0;
}

double estimateRuntimeBaseCostBps(
    const ProbabilisticRuntimeModel::RuntimeCostModel& cfg,
    const std::vector<double>& transformed_features
) {
    if (!cfg.enabled) {
        return 0.0;
    }
    if (cfg.atr_pct_idx < 0 || cfg.bb_width_idx < 0 || cfg.vol_ratio_idx < 0 || cfg.notional_ratio_idx < 0) {
        return 0.0;
    }
    if (!featureIndexValid(transformed_features, cfg.atr_pct_idx) ||
        !featureIndexValid(transformed_features, cfg.bb_width_idx) ||
        !featureIndexValid(transformed_features, cfg.vol_ratio_idx) ||
        !featureIndexValid(transformed_features, cfg.notional_ratio_idx)) {
        return 0.0;
    }

    // Runtime uses transformed features:
    // atr_pct/bb_width were multiplied by 100 in transform, ratios were log-transformed.
    const double atr_pct_raw = transformedAtrPctRaw(cfg, transformed_features);
    const double bb_width_raw = transformedBbWidthRaw(cfg, transformed_features);
    const double liquidity_ratio = transformedLiquidityRatio(cfg, transformed_features);

    const double atr_bps = atr_pct_raw * 10000.0;
    const double range_bps = bb_width_raw * 10000.0;
    const double vol_component = atr_bps / std::max(1e-9, cfg.volatility_norm_bps);
    const double range_component = range_bps / std::max(1e-9, cfg.range_norm_bps);
    const double illiquidity = std::min(cfg.liquidity_penalty_cap, cfg.liquidity_ref_ratio / liquidity_ratio);

    const double cost_bps =
        cfg.fee_floor_bps +
        (cfg.volatility_weight * vol_component) +
        (cfg.range_weight * range_component) +
        (cfg.liquidity_weight * illiquidity);
    return std::clamp(cost_bps, cfg.fee_floor_bps, cfg.cost_cap_bps);
}

std::string normalizeCostMode(std::string mode) {
    mode = lowerCopy(std::move(mode));
    if (mode == "tail" || mode == "tail_mode") {
        return "tail_mode";
    }
    if (mode == "hybrid" || mode == "hybrid_mode") {
        return "hybrid_mode";
    }
    return "mean_mode";
}

struct RuntimeCostBreakdown {
    std::string mode = "mean_mode";
    double entry_bps = 0.0;
    double exit_bps = 0.0;
    double mean_total_bps = 0.0;
    double tail_total_bps = 0.0;
    double used_total_bps = 0.0;
};

RuntimeCostBreakdown estimateRuntimeCostBreakdown(
    const ProbabilisticRuntimeModel::RuntimeCostModel& base_cfg,
    const ProbabilisticRuntimeModel::Phase3Policy& phase3_policy,
    const std::vector<double>& transformed_features
) {
    RuntimeCostBreakdown out;
    const double legacy_base_bps = estimateRuntimeBaseCostBps(base_cfg, transformed_features);
    if (!phase3_policy.phase3_cost_tail_enabled || !phase3_policy.cost_model.enabled) {
        out.mode = "mean_mode";
        out.entry_bps = legacy_base_bps * 0.5;
        out.exit_bps = legacy_base_bps * 0.5;
        out.mean_total_bps = legacy_base_bps;
        out.tail_total_bps = legacy_base_bps;
        out.used_total_bps = legacy_base_bps;
        return out;
    }

    const auto& p = phase3_policy.cost_model;
    out.mode = normalizeCostMode(p.mode);
    out.entry_bps = std::max(0.0, (legacy_base_bps * p.entry_multiplier) + p.entry_add_bps);
    out.exit_bps = std::max(0.0, (legacy_base_bps * p.exit_multiplier) + p.exit_add_bps);
    out.mean_total_bps = std::max(0.0, out.entry_bps + out.exit_bps);
    out.tail_total_bps = std::max(
        out.mean_total_bps,
        (out.mean_total_bps * (1.0 + std::max(0.0, p.tail_markup_ratio))) + std::max(0.0, p.tail_add_bps)
    );

    if (out.mode == "tail_mode") {
        out.used_total_bps = out.tail_total_bps;
    } else if (out.mode == "hybrid_mode") {
        const double lambda = std::clamp(p.hybrid_lambda, 0.0, 1.0);
        out.used_total_bps = out.mean_total_bps + (lambda * (out.tail_total_bps - out.mean_total_bps));
    } else {
        out.used_total_bps = out.mean_total_bps;
    }
    const double max_cap = std::max(base_cfg.fee_floor_bps, base_cfg.cost_cap_bps);
    out.entry_bps = std::clamp(out.entry_bps, 0.0, max_cap);
    out.exit_bps = std::clamp(out.exit_bps, 0.0, max_cap);
    out.mean_total_bps = std::clamp(out.mean_total_bps, 0.0, max_cap);
    out.tail_total_bps = std::clamp(out.tail_total_bps, 0.0, max_cap);
    out.used_total_bps = std::clamp(out.used_total_bps, 0.0, max_cap);
    return out;
}

bool inClosedRange(double value, double min_v, double max_v) {
    if (!std::isfinite(value)) {
        return false;
    }
    const bool has_min = std::isfinite(min_v) && min_v >= 0.0;
    const bool has_max = std::isfinite(max_v) && max_v >= 0.0;
    if (has_min && value < min_v) {
        return false;
    }
    if (has_max && value > max_v) {
        return false;
    }
    return true;
}

double quantileMapBps(
    double value_bps,
    const std::vector<std::pair<double, double>>& table,
    bool* out_ood
) {
    if (out_ood != nullptr) {
        *out_ood = false;
    }
    if (table.size() < 2) {
        return value_bps;
    }
    if (value_bps <= table.front().first) {
        if (out_ood != nullptr) {
            *out_ood = true;
        }
        return table.front().second;
    }
    if (value_bps >= table.back().first) {
        if (out_ood != nullptr) {
            *out_ood = true;
        }
        return table.back().second;
    }
    for (std::size_t i = 1; i < table.size(); ++i) {
        const auto& lo = table[i - 1];
        const auto& hi = table[i];
        if (value_bps < hi.first) {
            const double span = std::max(1e-9, hi.first - lo.first);
            const double t = (value_bps - lo.first) / span;
            return lo.second + ((hi.second - lo.second) * t);
        }
    }
    return value_bps;
}

const ProbabilisticRuntimeModel::EvCalibrationBucket* selectCalibrationBucket(
    const std::vector<ProbabilisticRuntimeModel::EvCalibrationBucket>& buckets,
    double volatility_bps,
    double liquidity_ratio
) {
    const ProbabilisticRuntimeModel::EvCalibrationBucket* best = nullptr;
    int best_specificity = -1;
    for (const auto& bucket : buckets) {
        if (!inClosedRange(volatility_bps, bucket.vol_bps_min, bucket.vol_bps_max)) {
            continue;
        }
        if (!inClosedRange(liquidity_ratio, bucket.liquidity_ratio_min, bucket.liquidity_ratio_max)) {
            continue;
        }
        int specificity = 0;
        if (bucket.vol_bps_min >= 0.0 || bucket.vol_bps_max >= 0.0) {
            specificity += 1;
        }
        if (bucket.liquidity_ratio_min >= 0.0 || bucket.liquidity_ratio_max >= 0.0) {
            specificity += 1;
        }
        if (specificity > best_specificity) {
            best_specificity = specificity;
            best = &bucket;
        }
    }
    return best;
}

struct EvCalibrationResult {
    double calibrated_bps = 0.0;
    double confidence = 1.0;
    bool applied = false;
};

EvCalibrationResult calibrateExpectedEdgeBps(
    double raw_bps,
    const ProbabilisticRuntimeModel::Phase3Policy& phase3_policy,
    const std::vector<ProbabilisticRuntimeModel::EvCalibrationBucket>& buckets,
    const ProbabilisticRuntimeModel::RuntimeCostModel& cost_model,
    const std::vector<double>& transformed_features
) {
    EvCalibrationResult out;
    out.calibrated_bps = raw_bps;
    out.confidence = 1.0;

    if (!phase3_policy.phase3_ev_calibration_enabled || !phase3_policy.ev_calibration.enabled) {
        return out;
    }

    const double vol_bps = transformedVolatilityBps(cost_model, transformed_features);
    const double liq_ratio = transformedLiquidityRatio(cost_model, transformed_features);
    const auto* bucket = selectCalibrationBucket(buckets, vol_bps, liq_ratio);
    if (bucket == nullptr) {
        out.applied = true;
        out.confidence = std::clamp(
            phase3_policy.ev_calibration.default_confidence * 0.60,
            phase3_policy.ev_calibration.min_confidence,
            1.0
        );
        return out;
    }

    out.applied = true;
    double calibrated = (raw_bps * bucket->slope) + bucket->intercept_bps;
    bool quantile_ood = false;
    if (phase3_policy.ev_calibration.use_quantile_map && bucket->quantile_map.size() >= 2) {
        calibrated = quantileMapBps(calibrated, bucket->quantile_map, &quantile_ood);
    }
    out.calibrated_bps = calibrated;

    double confidence = phase3_policy.ev_calibration.default_confidence;
    if (bucket->confidence >= 0.0) {
        confidence = bucket->confidence;
    } else if (bucket->sample_size > 0) {
        confidence = std::clamp(
            static_cast<double>(bucket->sample_size) /
                static_cast<double>(std::max(1, phase3_policy.ev_calibration.min_bucket_samples)),
            phase3_policy.ev_calibration.min_confidence,
            1.0
        );
    }
    if (quantile_ood) {
        confidence -= std::max(0.0, phase3_policy.ev_calibration.ood_penalty);
    }
    out.confidence = std::clamp(
        confidence,
        phase3_policy.ev_calibration.min_confidence,
        1.0
    );
    return out;
}

std::string lowerCopy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

bool validateBundleVersionAndContracts(
    const nlohmann::json& root,
    std::string* error_message
) {
    const std::string bundle_version = root.value("version", std::string{});
    const std::string expected_pipeline = "v2";
    if (bundle_version != "probabilistic_runtime_bundle_v2_draft") {
        if (error_message != nullptr) {
            *error_message = "unsupported_bundle_version";
        }
        return false;
    }

    if (!root.contains("pipeline_version") || !root["pipeline_version"].is_string()) {
        if (error_message != nullptr) {
            *error_message = "missing_pipeline_version_for_v2";
        }
        return false;
    }

    std::string declared_pipeline = root.value("pipeline_version", expected_pipeline);
    declared_pipeline = lowerCopy(declared_pipeline);
    if (declared_pipeline != expected_pipeline) {
        if (error_message != nullptr) {
            *error_message = "bundle_pipeline_version_mismatch";
        }
        return false;
    }

    if (!root.contains("feature_contract_version") || !root["feature_contract_version"].is_string()) {
        if (error_message != nullptr) {
            *error_message = "missing_feature_contract_version_for_v2";
        }
        return false;
    }
    if (!root.contains("runtime_bundle_contract_version") ||
        !root["runtime_bundle_contract_version"].is_string()) {
        if (error_message != nullptr) {
            *error_message = "missing_runtime_bundle_contract_version_for_v2";
        }
        return false;
    }

    const std::string feature_contract_version =
        lowerCopy(root.value("feature_contract_version", std::string{}));
    const std::string runtime_contract_version =
        lowerCopy(root.value("runtime_bundle_contract_version", std::string{}));
    if (feature_contract_version != "v2_draft") {
        if (error_message != nullptr) {
            *error_message = "unsupported_feature_contract_version";
        }
        return false;
    }
    if (runtime_contract_version != "v2_draft") {
        if (error_message != nullptr) {
            *error_message = "unsupported_runtime_bundle_contract_version";
        }
        return false;
    }
    return true;
}

} // namespace

bool ProbabilisticRuntimeModel::loadFromFile(const std::string& path, std::string* error_message) {
    loaded_ = false;
    markets_.clear();
    default_entry_ = MarketEntry{};
    has_default_entry_ = false;
    prefer_default_entry_ = false;
    feature_columns_.clear();
    cost_model_ = RuntimeCostModel{};
    phase3_policy_ = Phase3Policy{};
    ev_calibration_buckets_.clear();

    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        if (error_message != nullptr) {
            *error_message = "failed_to_open_bundle";
        }
        return false;
    }

    nlohmann::json root;
    try {
        in >> root;
    } catch (const std::exception& e) {
        if (error_message != nullptr) {
            *error_message = std::string("json_parse_error: ") + e.what();
        }
        return false;
    }
    if (!root.is_object()) {
        if (error_message != nullptr) {
            *error_message = "bundle_root_not_object";
        }
        return false;
    }
    if (!validateBundleVersionAndContracts(root, error_message)) {
        return false;
    }

    if (!root.contains("feature_columns") || !root["feature_columns"].is_array()) {
        if (error_message != nullptr) {
            *error_message = "feature_columns_missing";
        }
        return false;
    }
    for (const auto& v : root["feature_columns"]) {
        if (!v.is_string()) {
            continue;
        }
        feature_columns_.push_back(v.get<std::string>());
    }
    if (feature_columns_.empty()) {
        if (error_message != nullptr) {
            *error_message = "feature_columns_empty";
        }
        return false;
    }

    const auto cost_model_node = root.value("cost_model", nlohmann::json::object());
    if (cost_model_node.is_object()) {
        cost_model_.enabled = cost_model_node.value("enabled", false);
        cost_model_.fee_floor_bps = parseCostParam(cost_model_node, "fee_floor_bps", 6.0, 0.0, 2000.0);
        cost_model_.volatility_weight = parseCostParam(cost_model_node, "volatility_weight", 3.0, 0.0, 100.0);
        cost_model_.range_weight = parseCostParam(cost_model_node, "range_weight", 1.5, 0.0, 100.0);
        cost_model_.liquidity_weight = parseCostParam(cost_model_node, "liquidity_weight", 2.5, 0.0, 100.0);
        cost_model_.volatility_norm_bps = parseCostParam(cost_model_node, "volatility_norm_bps", 50.0, 1e-6, 100000.0);
        cost_model_.range_norm_bps = parseCostParam(cost_model_node, "range_norm_bps", 80.0, 1e-6, 100000.0);
        cost_model_.liquidity_ref_ratio = parseCostParam(cost_model_node, "liquidity_ref_ratio", 1.0, 1e-6, 100000.0);
        cost_model_.liquidity_penalty_cap = parseCostParam(cost_model_node, "liquidity_penalty_cap", 8.0, 1.0, 100000.0);
        cost_model_.cost_cap_bps = parseCostParam(
            cost_model_node,
            "cost_cap_bps",
            200.0,
            cost_model_.fee_floor_bps,
            100000.0
        );
    }
    cost_model_.atr_pct_idx = findFeatureIndex(feature_columns_, "atr_pct_14");
    cost_model_.bb_width_idx = findFeatureIndex(feature_columns_, "bb_width_20");
    cost_model_.vol_ratio_idx = findFeatureIndex(feature_columns_, "vol_ratio_20");
    cost_model_.notional_ratio_idx = findFeatureIndex(feature_columns_, "notional_ratio_20");

    const auto phase3_root = root.value("phase3", nlohmann::json::object());
    const auto phase3_frontier_node = phase3_root.value("frontier", nlohmann::json::object());
    const auto phase3_ev_calibration_node = phase3_root.value("ev_calibration", nlohmann::json::object());
    const auto phase3_cost_node = phase3_root.value("cost_model", nlohmann::json::object());
    const auto phase3_blend_node = phase3_root.value("adaptive_ev_blend", nlohmann::json::object());
    const auto phase3_diag_node = phase3_root.value("diagnostics_v2", nlohmann::json::object());

    const auto parsePhase3Flag = [&](const char* key, bool fallback) {
        if (root.contains(key)) {
            return root.value(key, fallback);
        }
        if (phase3_root.contains(key)) {
            return phase3_root.value(key, fallback);
        }
        return fallback;
    };

    phase3_policy_.phase3_frontier_enabled = parsePhase3Flag(
        "phase3_frontier_enabled",
        phase3_frontier_node.value("enabled", false)
    );
    phase3_policy_.phase3_ev_calibration_enabled = parsePhase3Flag(
        "phase3_ev_calibration_enabled",
        phase3_ev_calibration_node.value("enabled", false)
    );
    phase3_policy_.phase3_cost_tail_enabled = parsePhase3Flag(
        "phase3_cost_tail_enabled",
        phase3_cost_node.value("enabled", false)
    );
    phase3_policy_.phase3_adaptive_ev_blend_enabled = parsePhase3Flag(
        "phase3_adaptive_ev_blend_enabled",
        phase3_blend_node.value("enabled", false)
    );
    phase3_policy_.phase3_diagnostics_v2_enabled = parsePhase3Flag(
        "phase3_diagnostics_v2_enabled",
        phase3_diag_node.value("enabled", false)
    );

    phase3_policy_.frontier.enabled = phase3_policy_.phase3_frontier_enabled;
    phase3_policy_.frontier.k_margin = parseCostParam(phase3_frontier_node, "k_margin", 0.0, -100.0, 100.0);
    phase3_policy_.frontier.k_uncertainty = parseCostParam(phase3_frontier_node, "k_uncertainty", 0.0, -100.0, 100.0);
    phase3_policy_.frontier.k_cost_tail = parseCostParam(phase3_frontier_node, "k_cost_tail", 0.0, -100.0, 100.0);
    phase3_policy_.frontier.min_required_ev = parseCostParam(
        phase3_frontier_node,
        "min_required_ev",
        -0.0002,
        -0.20,
        0.20
    );
    phase3_policy_.frontier.max_required_ev = parseCostParam(
        phase3_frontier_node,
        "max_required_ev",
        0.0050,
        -0.20,
        0.20
    );
    if (phase3_policy_.frontier.max_required_ev < phase3_policy_.frontier.min_required_ev) {
        std::swap(phase3_policy_.frontier.min_required_ev, phase3_policy_.frontier.max_required_ev);
    }
    phase3_policy_.frontier.margin_floor = parseCostParam(
        phase3_frontier_node,
        "margin_floor",
        -1.0,
        -1.0,
        1.0
    );
    phase3_policy_.frontier.ev_confidence_floor = parseCostParam(
        phase3_frontier_node,
        "ev_confidence_floor",
        0.0,
        0.0,
        1.0
    );
    phase3_policy_.frontier.ev_confidence_penalty = parseCostParam(
        phase3_frontier_node,
        "ev_confidence_penalty",
        0.0,
        0.0,
        1.0
    );
    phase3_policy_.frontier.cost_tail_penalty = parseCostParam(
        phase3_frontier_node,
        "cost_tail_penalty",
        0.0,
        0.0,
        1.0
    );
    phase3_policy_.frontier.cost_tail_reject_threshold_pct = parseCostParam(
        phase3_frontier_node,
        "cost_tail_reject_threshold_pct",
        1.0,
        0.0,
        1.0
    );

    phase3_policy_.ev_calibration.enabled = phase3_policy_.phase3_ev_calibration_enabled;
    phase3_policy_.ev_calibration.use_quantile_map =
        phase3_ev_calibration_node.value("use_quantile_map", false);
    phase3_policy_.ev_calibration.min_bucket_samples = std::clamp(
        phase3_ev_calibration_node.value("min_bucket_samples", 64),
        1,
        1000000
    );
    phase3_policy_.ev_calibration.default_confidence = parseCostParam(
        phase3_ev_calibration_node,
        "default_confidence",
        1.0,
        0.0,
        1.0
    );
    phase3_policy_.ev_calibration.min_confidence = parseCostParam(
        phase3_ev_calibration_node,
        "min_confidence",
        0.10,
        0.0,
        1.0
    );
    phase3_policy_.ev_calibration.ood_penalty = parseCostParam(
        phase3_ev_calibration_node,
        "ood_penalty",
        0.10,
        0.0,
        1.0
    );
    if (phase3_policy_.ev_calibration.default_confidence < phase3_policy_.ev_calibration.min_confidence) {
        phase3_policy_.ev_calibration.default_confidence = phase3_policy_.ev_calibration.min_confidence;
    }
    const auto buckets_node = phase3_ev_calibration_node.value("buckets", nlohmann::json::array());
    if (buckets_node.is_array()) {
        for (const auto& raw_bucket : buckets_node) {
            if (!raw_bucket.is_object()) {
                continue;
            }
            EvCalibrationBucket bucket;
            bucket.name = raw_bucket.value("name", std::string("default"));
            bucket.regime = raw_bucket.value("regime", std::string("ANY"));
            bucket.volatility_bucket = raw_bucket.value("volatility_bucket", std::string("ANY"));
            bucket.liquidity_bucket = raw_bucket.value("liquidity_bucket", std::string("ANY"));
            bucket.slope = parseCostParam(raw_bucket, "slope", 1.0, -10.0, 10.0);
            bucket.intercept_bps = parseCostParam(raw_bucket, "intercept_bps", 0.0, -20000.0, 20000.0);
            bucket.sample_size = std::clamp(raw_bucket.value("sample_size", 0), 0, 100000000);
            bucket.confidence = parseCostParam(raw_bucket, "confidence", -1.0, -1.0, 1.0);
            bucket.vol_bps_min = parseCostParam(raw_bucket, "vol_bps_min", -1.0, -1.0, 1000000.0);
            bucket.vol_bps_max = parseCostParam(raw_bucket, "vol_bps_max", -1.0, -1.0, 1000000.0);
            bucket.liquidity_ratio_min = parseCostParam(raw_bucket, "liquidity_ratio_min", -1.0, -1.0, 1000000.0);
            bucket.liquidity_ratio_max = parseCostParam(raw_bucket, "liquidity_ratio_max", -1.0, -1.0, 1000000.0);

            const auto quantile_map = raw_bucket.value("quantile_map", nlohmann::json::array());
            if (quantile_map.is_array()) {
                for (const auto& q : quantile_map) {
                    double x_bps = 0.0;
                    double y_bps = 0.0;
                    bool valid = false;
                    try {
                        if (q.is_array() && q.size() >= 2) {
                            x_bps = q[0].get<double>();
                            y_bps = q[1].get<double>();
                            valid = std::isfinite(x_bps) && std::isfinite(y_bps);
                        } else if (q.is_object()) {
                            x_bps = q.value("x_bps", std::numeric_limits<double>::quiet_NaN());
                            y_bps = q.value("y_bps", std::numeric_limits<double>::quiet_NaN());
                            valid = std::isfinite(x_bps) && std::isfinite(y_bps);
                        }
                    } catch (...) {
                        valid = false;
                    }
                    if (valid) {
                        bucket.quantile_map.emplace_back(x_bps, y_bps);
                    }
                }
            }
            if (!bucket.quantile_map.empty()) {
                std::sort(
                    bucket.quantile_map.begin(),
                    bucket.quantile_map.end(),
                    [](const auto& lhs, const auto& rhs) {
                        return lhs.first < rhs.first;
                    }
                );
                double monotone_floor = bucket.quantile_map.front().second;
                for (auto& item : bucket.quantile_map) {
                    item.second = std::max(item.second, monotone_floor);
                    monotone_floor = item.second;
                }
            }
            ev_calibration_buckets_.push_back(std::move(bucket));
        }
    }

    phase3_policy_.cost_model.enabled = phase3_policy_.phase3_cost_tail_enabled;
    phase3_policy_.cost_model.mode = normalizeCostMode(
        phase3_cost_node.value("mode", std::string("mean_mode"))
    );
    phase3_policy_.cost_model.entry_multiplier = parseCostParam(
        phase3_cost_node,
        "entry_multiplier",
        0.50,
        0.0,
        100.0
    );
    phase3_policy_.cost_model.exit_multiplier = parseCostParam(
        phase3_cost_node,
        "exit_multiplier",
        0.50,
        0.0,
        100.0
    );
    phase3_policy_.cost_model.entry_add_bps = parseCostParam(
        phase3_cost_node,
        "entry_add_bps",
        0.0,
        0.0,
        100000.0
    );
    phase3_policy_.cost_model.exit_add_bps = parseCostParam(
        phase3_cost_node,
        "exit_add_bps",
        0.0,
        0.0,
        100000.0
    );
    phase3_policy_.cost_model.tail_markup_ratio = parseCostParam(
        phase3_cost_node,
        "tail_markup_ratio",
        0.35,
        0.0,
        100.0
    );
    phase3_policy_.cost_model.tail_add_bps = parseCostParam(
        phase3_cost_node,
        "tail_add_bps",
        0.0,
        0.0,
        100000.0
    );
    phase3_policy_.cost_model.hybrid_lambda = parseCostParam(
        phase3_cost_node,
        "hybrid_lambda",
        0.50,
        0.0,
        1.0
    );

    phase3_policy_.adaptive_ev_blend.enabled = phase3_policy_.phase3_adaptive_ev_blend_enabled;
    phase3_policy_.adaptive_ev_blend.min = parseCostParam(phase3_blend_node, "min", 0.05, 0.0, 1.0);
    phase3_policy_.adaptive_ev_blend.max = parseCostParam(phase3_blend_node, "max", 0.40, 0.0, 1.0);
    if (phase3_policy_.adaptive_ev_blend.max < phase3_policy_.adaptive_ev_blend.min) {
        std::swap(phase3_policy_.adaptive_ev_blend.min, phase3_policy_.adaptive_ev_blend.max);
    }
    phase3_policy_.adaptive_ev_blend.base = parseCostParam(
        phase3_blend_node,
        "base",
        0.20,
        phase3_policy_.adaptive_ev_blend.min,
        phase3_policy_.adaptive_ev_blend.max
    );
    phase3_policy_.adaptive_ev_blend.trend_bonus =
        parseCostParam(phase3_blend_node, "trend_bonus", 0.08, -1.0, 1.0);
    phase3_policy_.adaptive_ev_blend.ranging_penalty =
        parseCostParam(phase3_blend_node, "ranging_penalty", 0.06, -1.0, 1.0);
    phase3_policy_.adaptive_ev_blend.hostile_penalty =
        parseCostParam(phase3_blend_node, "hostile_penalty", 0.08, -1.0, 1.0);
    phase3_policy_.adaptive_ev_blend.high_confidence_bonus =
        parseCostParam(phase3_blend_node, "high_confidence_bonus", 0.05, -1.0, 1.0);
    phase3_policy_.adaptive_ev_blend.low_confidence_penalty =
        parseCostParam(phase3_blend_node, "low_confidence_penalty", 0.10, -1.0, 1.0);
    phase3_policy_.adaptive_ev_blend.cost_penalty =
        parseCostParam(phase3_blend_node, "cost_penalty", 0.06, -1.0, 1.0);

    phase3_policy_.diagnostics_v2.enabled = phase3_policy_.phase3_diagnostics_v2_enabled;

    prefer_default_entry_ = root.value("prefer_default_model", false);

    const auto default_model = root.value("default_model", nlohmann::json{});
    if (default_model.is_object()) {
        MarketEntry entry;
        entry.selected_fold_id = default_model.value("selected_fold_id", 0);
        if (parseHead(default_model.value("h1_model", nlohmann::json::object()), entry.h1, false) &&
            parseHead(default_model.value("h5_model", nlohmann::json::object()), entry.h5, true) &&
            entry.h1.coef.size() == feature_columns_.size() &&
            entry.h5.coef.size() == feature_columns_.size()) {
            if (entry.h5.has_edge_regressor && entry.h5.edge_coef.size() != feature_columns_.size()) {
                entry.h5.has_edge_regressor = false;
                entry.h5.edge_coef.clear();
            }
            parseEnsembleMembers(default_model, feature_columns_.size(), entry.ensemble_members);
            default_entry_ = std::move(entry);
            has_default_entry_ = true;
        }
    }

    const auto markets = root.value("markets", nlohmann::json::array());
    if (markets.is_array()) {
        for (const auto& m : markets) {
            if (!m.is_object()) {
                continue;
            }
            const std::string market = m.value("market", "");
            if (market.empty()) {
                continue;
            }

            MarketEntry entry;
            entry.selected_fold_id = m.value("selected_fold_id", 0);
            if (!parseHead(m.value("h1_model", nlohmann::json::object()), entry.h1, false)) {
                continue;
            }
            if (!parseHead(m.value("h5_model", nlohmann::json::object()), entry.h5, true)) {
                continue;
            }
            if (entry.h1.coef.size() != feature_columns_.size() ||
                entry.h5.coef.size() != feature_columns_.size()) {
                continue;
            }
            if (entry.h5.has_edge_regressor && entry.h5.edge_coef.size() != feature_columns_.size()) {
                entry.h5.has_edge_regressor = false;
                entry.h5.edge_coef.clear();
            }
            parseEnsembleMembers(m, feature_columns_.size(), entry.ensemble_members);
            markets_.insert_or_assign(market, std::move(entry));
        }
    }

    loaded_ = has_default_entry_ || !markets_.empty();
    if (!loaded_ && error_message != nullptr) {
        *error_message = "no_valid_model_entries";
    }
    return loaded_;
}

bool ProbabilisticRuntimeModel::hasMarket(const std::string& market) const {
    return markets_.find(market) != markets_.end();
}

bool ProbabilisticRuntimeModel::supportsMarket(const std::string& market) const {
    return has_default_entry_ || hasMarket(market);
}

bool ProbabilisticRuntimeModel::infer(
    const std::string& market,
    const std::vector<double>& transformed_features,
    ProbabilisticInference& out
) const {
    if (transformed_features.size() != feature_columns_.size()) {
        return false;
    }
    out = ProbabilisticInference{};

    const MarketEntry* entry = nullptr;
    if (prefer_default_entry_ && has_default_entry_) {
        entry = &default_entry_;
    } else {
        const auto it = markets_.find(market);
        if (it != markets_.end()) {
            entry = &it->second;
        } else if (has_default_entry_) {
            entry = &default_entry_;
        }
    }
    if (entry == nullptr) {
        return false;
    }

    const double h1_raw_primary = clipProb(sigmoid(linearScore(entry->h1.coef, entry->h1.intercept, transformed_features)));
    const double h5_raw_primary = clipProb(sigmoid(linearScore(entry->h5.coef, entry->h5.intercept, transformed_features)));
    const double h1_cal_primary = calibrateProb(h1_raw_primary, entry->h1.calib_a, entry->h1.calib_b);
    const double h5_cal_primary = calibrateProb(h5_raw_primary, entry->h5.calib_a, entry->h5.calib_b);

    double h1_mean = h1_cal_primary;
    double h5_mean = h5_cal_primary;
    double h1_std = 0.0;
    double h5_std = 0.0;
    int ensemble_member_count = 1;
    if (entry->ensemble_members.size() >= 2) {
        std::vector<double> h1_probs;
        std::vector<double> h5_probs;
        h1_probs.reserve(entry->ensemble_members.size());
        h5_probs.reserve(entry->ensemble_members.size());
        for (const auto& member : entry->ensemble_members) {
            if (member.h1.coef.size() != transformed_features.size() ||
                member.h5.coef.size() != transformed_features.size()) {
                continue;
            }
            const double m_h1_raw = clipProb(sigmoid(linearScore(member.h1.coef, member.h1.intercept, transformed_features)));
            const double m_h5_raw = clipProb(sigmoid(linearScore(member.h5.coef, member.h5.intercept, transformed_features)));
            h1_probs.push_back(calibrateProb(m_h1_raw, member.h1.calib_a, member.h1.calib_b));
            h5_probs.push_back(calibrateProb(m_h5_raw, member.h5.calib_a, member.h5.calib_b));
        }
        if (h5_probs.size() >= 2 && h1_probs.size() == h5_probs.size()) {
            h1_mean = clipProb(meanOrDefault(h1_probs, h1_cal_primary));
            h5_mean = clipProb(meanOrDefault(h5_probs, h5_cal_primary));
            h1_std = stdOrZero(h1_probs, h1_mean);
            h5_std = stdOrZero(h5_probs, h5_mean);
            ensemble_member_count = static_cast<int>(h5_probs.size());
        }
    }

    out.prob_h1_raw = h1_raw_primary;
    out.prob_h1_calibrated = h1_cal_primary;
    out.prob_h1_mean = h1_mean;
    out.prob_h1_std = h1_std;
    out.prob_h5_raw = h5_raw_primary;
    out.prob_h5_calibrated = h5_cal_primary;
    out.prob_h5_mean = h5_mean;
    out.prob_h5_std = h5_std;
    out.ensemble_member_count = ensemble_member_count;
    out.selection_threshold_h5 = entry->h5.threshold;
    out.phase3_frontier_enabled = phase3_policy_.phase3_frontier_enabled;
    out.phase3_ev_calibration_enabled = phase3_policy_.phase3_ev_calibration_enabled;
    out.phase3_cost_tail_enabled = phase3_policy_.phase3_cost_tail_enabled;
    out.phase3_adaptive_ev_blend_enabled = phase3_policy_.phase3_adaptive_ev_blend_enabled;
    out.phase3_diagnostics_v2_enabled = phase3_policy_.phase3_diagnostics_v2_enabled;

    const double fallback_expected_edge_bps =
        (h5_cal_primary * entry->h5.edge_win_mean_bps) +
        ((1.0 - h5_cal_primary) * entry->h5.edge_loss_mean_bps);
    double expected_edge_raw_bps = fallback_expected_edge_bps;
    bool edge_regressor_used = false;
    if (entry->h5.has_edge_regressor && entry->h5.edge_coef.size() == transformed_features.size()) {
        const double reg_edge = linearScore(entry->h5.edge_coef, entry->h5.edge_intercept, transformed_features);
        if (std::isfinite(reg_edge)) {
            expected_edge_raw_bps = std::clamp(
                reg_edge,
                -entry->h5.edge_clip_abs_bps,
                entry->h5.edge_clip_abs_bps
            );
            edge_regressor_used = true;
        }
    }
    out.expected_edge_raw_bps = expected_edge_raw_bps;
    out.edge_regressor_used = edge_regressor_used;

    const EvCalibrationResult calibration = calibrateExpectedEdgeBps(
        expected_edge_raw_bps,
        phase3_policy_,
        ev_calibration_buckets_,
        cost_model_,
        transformed_features
    );
    double expected_edge_calibrated_bps = calibration.calibrated_bps;
    if (!std::isfinite(expected_edge_calibrated_bps)) {
        expected_edge_calibrated_bps = expected_edge_raw_bps;
    }
    expected_edge_calibrated_bps = std::clamp(expected_edge_calibrated_bps, -5000.0, 5000.0);
    out.expected_edge_calibrated_bps = expected_edge_calibrated_bps;
    out.expected_edge_before_cost_bps = expected_edge_calibrated_bps;
    out.ev_calibration_applied = calibration.applied;
    out.ev_confidence = std::clamp(calibration.confidence, 0.0, 1.0);

    const RuntimeCostBreakdown cost_breakdown = estimateRuntimeCostBreakdown(
        cost_model_,
        phase3_policy_,
        transformed_features
    );
    out.entry_cost_bps_estimate = cost_breakdown.entry_bps;
    out.exit_cost_bps_estimate = cost_breakdown.exit_bps;
    out.tail_cost_bps_estimate = cost_breakdown.tail_total_bps;
    out.cost_used_bps_estimate = cost_breakdown.used_total_bps;
    out.cost_mode = cost_breakdown.mode;
    out.cost_bps_estimate = cost_breakdown.used_total_bps;
    out.expected_edge_bps = expected_edge_calibrated_bps - out.cost_bps_estimate;
    out.expected_edge_pct = out.expected_edge_bps / 10000.0;
    out.select_h5 = (h5_cal_primary >= entry->h5.threshold);
    return true;
}

} // namespace analytics
} // namespace autolife
