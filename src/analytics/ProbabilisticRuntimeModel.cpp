#include "analytics/ProbabilisticRuntimeModel.h"
#include "common/Logger.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>
#include <string_view>

#include <nlohmann/json.hpp>
#include <openssl/sha.h>

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

std::string trimCopy(const std::string& value) {
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(begin, end - begin);
}

bool splitKeyValueLine(const std::string& line, std::string& out_key, std::string& out_value) {
    const auto pos = line.find('=');
    if (pos == std::string::npos) {
        return false;
    }
    out_key = trimCopy(line.substr(0, pos));
    out_value = trimCopy(line.substr(pos + 1));
    return !out_key.empty();
}

bool parseIntList(const std::string& text, std::vector<int>& out_values) {
    out_values.clear();
    std::istringstream iss(text);
    std::string token;
    while (iss >> token) {
        try {
            const int value = std::stoi(token);
            out_values.push_back(value);
        } catch (...) {
            return false;
        }
    }
    return !out_values.empty();
}

bool parseDoubleList(const std::string& text, std::vector<double>& out_values) {
    out_values.clear();
    std::istringstream iss(text);
    std::string token;
    while (iss >> token) {
        try {
            const double value = std::stod(token);
            out_values.push_back(value);
        } catch (...) {
            return false;
        }
    }
    return !out_values.empty();
}

std::string sha256HexOfFile(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) {
        return {};
    }
    SHA256_CTX ctx;
    if (SHA256_Init(&ctx) != 1) {
        return {};
    }
    std::array<char, 64 * 1024> buffer{};
    while (ifs.good()) {
        ifs.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize read_size = ifs.gcount();
        if (read_size <= 0) {
            break;
        }
        if (SHA256_Update(&ctx, buffer.data(), static_cast<std::size_t>(read_size)) != 1) {
            return {};
        }
    }
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    if (SHA256_Final(digest.data(), &ctx) != 1) {
        return {};
    }
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (unsigned char b : digest) {
        oss << std::setw(2) << static_cast<int>(b);
    }
    return oss.str();
}

bool parseLightGbmTextModel(
    const std::string& model_path,
    std::size_t feature_count,
    ProbabilisticRuntimeModel::LgbmModel& out_model,
    std::string* error_message
) {
    out_model = ProbabilisticRuntimeModel::LgbmModel{};
    std::ifstream ifs(model_path);
    if (!ifs.is_open()) {
        if (error_message != nullptr) {
            *error_message = "lgbm_model_open_failed";
        }
        return false;
    }

    ProbabilisticRuntimeModel::LgbmTree* active_tree = nullptr;
    std::string line;
    while (std::getline(ifs, line)) {
        const std::string trimmed = trimCopy(line);
        if (trimmed.empty()) {
            continue;
        }
        if (trimmed.rfind("Tree=", 0) == 0) {
            out_model.trees.emplace_back();
            active_tree = &out_model.trees.back();
            continue;
        }

        std::string key;
        std::string value;
        if (!splitKeyValueLine(trimmed, key, value)) {
            continue;
        }

        if (active_tree == nullptr) {
            if (key == "max_feature_idx") {
                try {
                    out_model.max_feature_idx = std::stoi(value);
                } catch (...) {
                    out_model.max_feature_idx = -1;
                }
                continue;
            }
            if (key == "average_output") {
                out_model.average_output = (value == "1" || value == "true" || value == "True");
                continue;
            }
            if (key == "objective") {
                const auto pos = value.find("sigmoid:");
                if (pos != std::string::npos) {
                    const std::string suffix = trimCopy(value.substr(pos + 8));
                    try {
                        const double parsed = std::stod(suffix);
                        if (std::isfinite(parsed) && parsed > 0.0) {
                            out_model.sigmoid = parsed;
                        }
                    } catch (...) {
                    }
                }
                continue;
            }
            continue;
        }

        if (key == "split_feature") {
            if (!parseIntList(value, active_tree->split_feature)) {
                if (error_message != nullptr) {
                    *error_message = "lgbm_split_feature_parse_failed";
                }
                return false;
            }
            continue;
        }
        if (key == "threshold") {
            if (!parseDoubleList(value, active_tree->threshold)) {
                if (error_message != nullptr) {
                    *error_message = "lgbm_threshold_parse_failed";
                }
                return false;
            }
            continue;
        }
        if (key == "decision_type") {
            if (!parseIntList(value, active_tree->decision_type)) {
                if (error_message != nullptr) {
                    *error_message = "lgbm_decision_type_parse_failed";
                }
                return false;
            }
            continue;
        }
        if (key == "left_child") {
            if (!parseIntList(value, active_tree->left_child)) {
                if (error_message != nullptr) {
                    *error_message = "lgbm_left_child_parse_failed";
                }
                return false;
            }
            continue;
        }
        if (key == "right_child") {
            if (!parseIntList(value, active_tree->right_child)) {
                if (error_message != nullptr) {
                    *error_message = "lgbm_right_child_parse_failed";
                }
                return false;
            }
            continue;
        }
        if (key == "leaf_value") {
            if (!parseDoubleList(value, active_tree->leaf_value)) {
                if (error_message != nullptr) {
                    *error_message = "lgbm_leaf_value_parse_failed";
                }
                return false;
            }
            continue;
        }
        if (key == "shrinkage") {
            try {
                active_tree->shrinkage = std::stod(value);
            } catch (...) {
                active_tree->shrinkage = 1.0;
            }
            if (!std::isfinite(active_tree->shrinkage)) {
                active_tree->shrinkage = 1.0;
            }
            continue;
        }
    }

    if (out_model.trees.empty()) {
        if (error_message != nullptr) {
            *error_message = "lgbm_model_has_no_trees";
        }
        return false;
    }

    if (out_model.max_feature_idx >= 0 &&
        static_cast<std::size_t>(out_model.max_feature_idx + 1) > feature_count) {
        if (error_message != nullptr) {
            *error_message = "lgbm_model_feature_count_mismatch";
        }
        return false;
    }

    for (const auto& tree : out_model.trees) {
        const std::size_t split_count = tree.split_feature.size();
        if (split_count == 0U) {
            if (tree.leaf_value.empty()) {
                if (error_message != nullptr) {
                    *error_message = "lgbm_tree_leaf_missing";
                }
                return false;
            }
            continue;
        }
        if (tree.threshold.size() != split_count ||
            tree.left_child.size() != split_count ||
            tree.right_child.size() != split_count) {
            if (error_message != nullptr) {
                *error_message = "lgbm_tree_split_array_mismatch";
            }
            return false;
        }
        if (!tree.decision_type.empty() && tree.decision_type.size() != split_count) {
            if (error_message != nullptr) {
                *error_message = "lgbm_tree_decision_type_mismatch";
            }
            return false;
        }
        if (tree.leaf_value.empty()) {
            if (error_message != nullptr) {
                *error_message = "lgbm_tree_leaf_missing";
            }
            return false;
        }
    }

    return true;
}

double inferLightGbmProb(
    const ProbabilisticRuntimeModel::LgbmModel& model,
    const std::vector<double>& transformed_features
) {
    double raw_score = 0.0;
    for (const auto& tree : model.trees) {
        if (tree.split_feature.empty()) {
            if (!tree.leaf_value.empty()) {
                raw_score += (tree.shrinkage * tree.leaf_value.front());
            }
            continue;
        }
        int node_index = 0;
        constexpr int kMaxDepthGuard = 8192;
        int guard = 0;
        while (guard < kMaxDepthGuard) {
            ++guard;
            if (node_index < 0 ||
                static_cast<std::size_t>(node_index) >= tree.split_feature.size()) {
                break;
            }
            const int feature_index = tree.split_feature[static_cast<std::size_t>(node_index)];
            const double threshold = tree.threshold[static_cast<std::size_t>(node_index)];
            double feature_value = 0.0;
            bool feature_valid = false;
            if (feature_index >= 0 &&
                static_cast<std::size_t>(feature_index) < transformed_features.size()) {
                feature_value = transformed_features[static_cast<std::size_t>(feature_index)];
                feature_valid = std::isfinite(feature_value);
            }
            // Numeric split path: for missing feature values, default to left branch.
            const bool go_left = (!feature_valid) || (feature_value <= threshold);
            const int next = go_left
                                 ? tree.left_child[static_cast<std::size_t>(node_index)]
                                 : tree.right_child[static_cast<std::size_t>(node_index)];
            if (next < 0) {
                const int leaf_index = (-next) - 1;
                if (leaf_index >= 0 &&
                    static_cast<std::size_t>(leaf_index) < tree.leaf_value.size()) {
                    raw_score += (tree.shrinkage * tree.leaf_value[static_cast<std::size_t>(leaf_index)]);
                }
                break;
            }
            node_index = next;
        }
    }
    if (model.average_output && !model.trees.empty()) {
        raw_score /= static_cast<double>(model.trees.size());
    }
    return clipProb(sigmoid(raw_score * model.sigmoid));
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
    const double base_bps = estimateRuntimeBaseCostBps(base_cfg, transformed_features);
    if (!phase3_policy.phase3_cost_tail_enabled || !phase3_policy.cost_model.enabled) {
        out.mode = "mean_mode";
        out.entry_bps = base_bps * 0.5;
        out.exit_bps = base_bps * 0.5;
        out.mean_total_bps = base_bps;
        out.tail_total_bps = base_bps;
        out.used_total_bps = base_bps;
        return out;
    }

    const auto& p = phase3_policy.cost_model;
    out.mode = normalizeCostMode(p.mode);
    out.entry_bps = std::max(0.0, (base_bps * p.entry_multiplier) + p.entry_add_bps);
    out.exit_bps = std::max(0.0, (base_bps * p.exit_multiplier) + p.exit_add_bps);
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

std::string normalizeRegimeBudgetKeyForConfig(const std::string& raw_key) {
    const std::string key = lowerCopy(raw_key);
    if (key == "ranging" || key == "range") {
        return "RANGING";
    }
    if (key == "trending_up" || key == "trending-up" || key == "trending up" || key == "up") {
        return "TRENDING_UP";
    }
    if (key == "trending_down" || key == "trending-down" || key == "trending down" || key == "down") {
        return "TRENDING_DOWN";
    }
    if (key == "high_volatility" || key == "high-volatility" || key == "high volatility" ||
        key == "volatile" || key == "hostile") {
        return "HIGH_VOLATILITY";
    }
    if (key == "unknown") {
        return "UNKNOWN";
    }
    if (key == "any" || key == "default") {
        return "ANY";
    }
    return std::string();
}

bool parseOptionalBool(const nlohmann::json& node, bool fallback = false) {
    if (node.is_boolean()) {
        return node.get<bool>();
    }
    if (node.is_number_integer()) {
        return node.get<int>() != 0;
    }
    if (node.is_number_float()) {
        return std::fabs(node.get<double>()) > 1e-12;
    }
    if (node.is_string()) {
        const std::string raw = lowerCopy(node.get<std::string>());
        if (raw == "true" || raw == "1" || raw == "on" || raw == "yes") {
            return true;
        }
        if (raw == "false" || raw == "0" || raw == "off" || raw == "no") {
            return false;
        }
    }
    return fallback;
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
    phase4_policy_ = Phase4Policy{};
    runtime_semantics_state_ = RuntimeSemanticsState{};
    ev_calibration_buckets_.clear();
    prob_model_backend_ = "sgd";
    lgbm_model_path_resolved_.clear();
    lgbm_model_sha256_expected_.clear();
    lgbm_model_sha256_loaded_.clear();
    lgbm_model_loaded_ = false;
    lgbm_h5_calib_a_ = 1.0;
    lgbm_h5_calib_b_ = 0.0;
    lgbm_model_ = LgbmModel{};

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

    {
        lgbm_ev_affine_enabled_ = false;
        lgbm_ev_affine_scale_ = 1.0;
        lgbm_ev_affine_shift_ = 0.0;
        std::string backend = lowerCopy(root.value("prob_model_backend", std::string("sgd")));
        if (backend != "lgbm") {
            backend = "sgd";
        }
        prob_model_backend_ = backend;
        if (backend == "lgbm") {
            lgbm_model_sha256_expected_ =
                lowerCopy(trimCopy(root.value("lgbm_model_sha256", std::string{})));
            const auto lgbm_calib_node = root.value("lgbm_h5_calibration", nlohmann::json::object());
            if (lgbm_calib_node.is_object()) {
                const double a = lgbm_calib_node.value("a", 1.0);
                const double b = lgbm_calib_node.value("b", 0.0);
                if (std::isfinite(a)) {
                    lgbm_h5_calib_a_ = a;
                }
                if (std::isfinite(b)) {
                    lgbm_h5_calib_b_ = b;
                }
            }
        }
        const auto lgbm_ev_affine_node = root.value("lgbm_ev_affine", nlohmann::json::object());
        if (lgbm_ev_affine_node.is_object()) {
            lgbm_ev_affine_enabled_ = lgbm_ev_affine_node.value("enabled", false);
            const double scale = lgbm_ev_affine_node.value("scale", 1.0);
            const double shift = lgbm_ev_affine_node.value("shift", 0.0);
            if (std::isfinite(scale) && std::fabs(scale) <= 1000.0) {
                lgbm_ev_affine_scale_ = scale;
            }
            if (std::isfinite(shift) && std::fabs(shift) <= 100000.0) {
                lgbm_ev_affine_shift_ = shift;
            }
        }
    }

    std::string edge_semantics = lowerCopy(root.value("edge_semantics", std::string("net")));
    if (edge_semantics != "gross") {
        edge_semantics = "net";
    }
    runtime_semantics_state_.edge_semantics = edge_semantics;

    const auto cost_model_node = root.value("cost_model", nlohmann::json::object());
    runtime_semantics_state_.root_cost_model_enabled_configured =
        bool(cost_model_node.is_object() && cost_model_node.value("enabled", false));
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
    const auto phase3_ev_calibration_node = phase3_root.value("ev_calibration", nlohmann::json::object());
    const auto phase3_cost_node = phase3_root.value("cost_model", nlohmann::json::object());
    const auto phase3_blend_node = phase3_root.value("adaptive_ev_blend", nlohmann::json::object());
    const auto phase3_ops_node = phase3_root.value("operations_control", nlohmann::json::object());
    const auto phase3_primary_minimums_node = phase3_root.value("primary_minimums", nlohmann::json::object());
    const auto phase3_primary_priority_node = phase3_root.value("primary_priority", nlohmann::json::object());
    const auto phase3_primary_decision_profile_node =
        phase3_root.value("primary_decision_profile", nlohmann::json::object());
    const auto phase3_diag_node = phase3_root.value("diagnostics_v2", nlohmann::json::object());
    const auto phase3_execution_guard_node =
        phase3_root.value("execution_guard", nlohmann::json::object());
    const auto phase3_liq_vol_gate_node =
        phase3_execution_guard_node.value("liq_vol_gate", nlohmann::json::object());
    const auto phase3_foundation_structure_gate_node =
        phase3_root.value("foundation_structure_gate", nlohmann::json::object());
    const auto phase3_bear_rebound_guard_node =
        phase3_root.value("bear_rebound_guard", nlohmann::json::object());
    const auto phase3_risk_node = phase3_root.value("risk", nlohmann::json::object());
    const auto phase3_exit_node = phase3_root.value("exit", nlohmann::json::object());
    const auto phase3_regime_entry_disable_node =
        phase3_root.value("regime_entry_disable", nlohmann::json::object());

    phase3_policy_.phase3_ev_calibration_enabled = phase3_ev_calibration_node.value("enabled", false);
    phase3_policy_.phase3_cost_tail_enabled = phase3_cost_node.value("enabled", false);
    runtime_semantics_state_.phase3_cost_model_enabled_configured =
        bool(phase3_cost_node.is_object() && phase3_cost_node.value("enabled", false));
    phase3_policy_.phase3_adaptive_ev_blend_enabled = phase3_blend_node.value("enabled", false);
    phase3_policy_.phase3_diagnostics_v2_enabled = phase3_diag_node.value("enabled", false);

    phase3_policy_.liq_vol_gate = Phase3LiquidityVolumeGatePolicy{};
    {
        std::string mode = lowerCopy(phase3_liq_vol_gate_node.value("mode", std::string("static")));
        if (mode != "quantile_dynamic") {
            mode = "static";
        }
        bool enabled = phase3_liq_vol_gate_node.value("enabled", mode == "quantile_dynamic");
        std::string low_conf_action = lowerCopy(
            phase3_liq_vol_gate_node.value("low_conf_action", std::string("hold"))
        );
        if (low_conf_action != "fallback_static") {
            low_conf_action = "hold";
        }
        phase3_policy_.liq_vol_gate.mode = mode;
        phase3_policy_.liq_vol_gate.enabled = enabled && mode == "quantile_dynamic";
        phase3_policy_.liq_vol_gate.window_minutes = std::clamp(
            phase3_liq_vol_gate_node.value("window_minutes", 60),
            1,
            24 * 60
        );
        phase3_policy_.liq_vol_gate.quantile_q = parseCostParam(
            phase3_liq_vol_gate_node,
            "quantile_q",
            0.20,
            0.0,
            1.0
        );
        phase3_policy_.liq_vol_gate.min_samples_required = std::clamp(
            phase3_liq_vol_gate_node.value("min_samples_required", 30),
            1,
            200000
        );
        phase3_policy_.liq_vol_gate.low_conf_action = low_conf_action;
        if (!phase3_policy_.liq_vol_gate.enabled) {
            phase3_policy_.liq_vol_gate.mode = "static";
        }
    }

    phase3_policy_.foundation_structure_gate = Phase3FoundationStructureGatePolicy{};
    {
        std::string mode = lowerCopy(
            phase3_foundation_structure_gate_node.value("mode", std::string("static"))
        );
        if (mode != "trend_only_relax") {
            mode = "static";
        }
        const bool enabled =
            phase3_foundation_structure_gate_node.value("enabled", mode == "trend_only_relax");
        phase3_policy_.foundation_structure_gate.mode = mode;
        phase3_policy_.foundation_structure_gate.enabled = enabled && mode == "trend_only_relax";
        phase3_policy_.foundation_structure_gate.relax_delta = parseCostParam(
            phase3_foundation_structure_gate_node,
            "relax_delta",
            0.0,
            0.0,
            1.0
        );
        if (!phase3_policy_.foundation_structure_gate.enabled) {
            phase3_policy_.foundation_structure_gate.mode = "static";
            phase3_policy_.foundation_structure_gate.relax_delta = 0.0;
        }
    }

    phase3_policy_.bear_rebound_guard = Phase3BearReboundGuardPolicy{};
    {
        std::string mode = lowerCopy(
            phase3_bear_rebound_guard_node.value("mode", std::string("static"))
        );
        if (mode != "quantile_dynamic") {
            mode = "static";
        }
        const bool enabled =
            phase3_bear_rebound_guard_node.value("enabled", mode == "quantile_dynamic");
        std::string low_conf_action = lowerCopy(
            phase3_bear_rebound_guard_node.value("low_conf_action", std::string("hold"))
        );
        if (low_conf_action != "fallback_static") {
            low_conf_action = "hold";
        }
        phase3_policy_.bear_rebound_guard.mode = mode;
        phase3_policy_.bear_rebound_guard.enabled = enabled && mode == "quantile_dynamic";
        phase3_policy_.bear_rebound_guard.window_minutes = std::clamp(
            phase3_bear_rebound_guard_node.value("window_minutes", 60),
            1,
            24 * 60
        );
        phase3_policy_.bear_rebound_guard.quantile_q = parseCostParam(
            phase3_bear_rebound_guard_node,
            "quantile_q",
            0.20,
            0.0,
            1.0
        );
        phase3_policy_.bear_rebound_guard.min_samples_required = std::clamp(
            phase3_bear_rebound_guard_node.value("min_samples_required", 30),
            1,
            200000
        );
        phase3_policy_.bear_rebound_guard.low_conf_action = low_conf_action;
        phase3_policy_.bear_rebound_guard.static_threshold = parseCostParam(
            phase3_bear_rebound_guard_node,
            "static_threshold",
            1.0,
            0.0,
            10.0
        );
        if (!phase3_policy_.bear_rebound_guard.enabled) {
            phase3_policy_.bear_rebound_guard.mode = "static";
        }
    }

    phase3_policy_.risk = Phase3StopLossRiskPolicy{};
    {
        const bool enabled = phase3_risk_node.value("enabled", false);
        phase3_policy_.risk.enabled = enabled;
        phase3_policy_.risk.stop_loss_trending_multiplier = parseCostParam(
            phase3_risk_node,
            "stop_loss_trending_multiplier",
            1.0,
            0.0,
            10.0
        );
    }

    phase3_policy_.exit = Phase3ExitPolicy{};
    {
        std::string strategy_exit_mode = lowerCopy(
            phase3_exit_node.value("strategy_exit_mode", std::string("enforce"))
        );
        if (strategy_exit_mode != "observe_only" &&
            strategy_exit_mode != "clamp_to_stop" &&
            strategy_exit_mode != "disabled") {
            strategy_exit_mode = "enforce";
        }
        phase3_policy_.exit.strategy_exit_mode = strategy_exit_mode;
        phase3_policy_.exit.be_after_partial_tp_delay_sec = std::clamp(
            phase3_exit_node.value("be_after_partial_tp_delay_sec", 0),
            0,
            24 * 3600
        );
        phase3_policy_.exit.tp_distance_trending_multiplier = parseCostParam(
            phase3_exit_node,
            "tp_distance_trending_multiplier",
            1.0,
            0.0,
            10.0
        );
    }

    phase3_policy_.disable_ranging_entry = phase3_root.value("disable_ranging_entry", false);
    phase3_policy_.regime_entry_disable.clear();
    if (phase3_regime_entry_disable_node.is_object()) {
        for (auto it = phase3_regime_entry_disable_node.begin();
             it != phase3_regime_entry_disable_node.end();
             ++it) {
            const std::string normalized_key = normalizeRegimeBudgetKeyForConfig(it.key());
            if (normalized_key.empty()) {
                continue;
            }
            phase3_policy_.regime_entry_disable[normalized_key] = parseOptionalBool(it.value(), false);
        }
    }
    if (phase3_policy_.disable_ranging_entry) {
        phase3_policy_.regime_entry_disable["RANGING"] = true;
    }
    phase3_policy_.regime_entry_disable_enabled = false;
    for (const auto& kv : phase3_policy_.regime_entry_disable) {
        if (kv.second) {
            phase3_policy_.regime_entry_disable_enabled = true;
            break;
        }
    }

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

    // NET semantics guardrail:
    // edge is already net in label/runtime semantics, so runtime cost subtraction path must stay OFF.
    if (runtime_semantics_state_.edge_semantics == "net") {
        const bool configured_root = runtime_semantics_state_.root_cost_model_enabled_configured;
        const bool configured_phase3 = runtime_semantics_state_.phase3_cost_model_enabled_configured;
        if (configured_root || configured_phase3) {
            runtime_semantics_state_.guard_violation = true;
            runtime_semantics_state_.guard_forced_off = true;
            runtime_semantics_state_.guard_action = "force_off";
            cost_model_.enabled = false;
            phase3_policy_.phase3_cost_tail_enabled = false;
            phase3_policy_.cost_model.enabled = false;
        }
    }
    runtime_semantics_state_.root_cost_model_enabled_effective = bool(cost_model_.enabled);
    runtime_semantics_state_.phase3_cost_model_enabled_effective =
        bool(phase3_policy_.phase3_cost_tail_enabled && phase3_policy_.cost_model.enabled);

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
    phase3_policy_.adaptive_ev_blend.high_confidence_threshold =
        parseCostParam(phase3_blend_node, "high_confidence_threshold", 0.72, 0.0, 1.0);
    phase3_policy_.adaptive_ev_blend.high_confidence_bonus =
        parseCostParam(phase3_blend_node, "high_confidence_bonus", 0.05, -1.0, 1.0);
    phase3_policy_.adaptive_ev_blend.low_confidence_penalty =
        parseCostParam(phase3_blend_node, "low_confidence_penalty", 0.10, -1.0, 1.0);
    phase3_policy_.adaptive_ev_blend.cost_penalty =
        parseCostParam(phase3_blend_node, "cost_penalty", 0.06, -1.0, 1.0);

    phase3_policy_.operations_control.enabled = phase3_ops_node.value("enabled", false);
    phase3_policy_.operations_control.mode = lowerCopy(
        phase3_ops_node.value("mode", std::string("manual"))
    );
    phase3_policy_.operations_control.required_ev_offset_min = parseCostParam(
        phase3_ops_node,
        "required_ev_offset_min",
        -0.0030,
        -0.20,
        0.20
    );
    phase3_policy_.operations_control.required_ev_offset_max = parseCostParam(
        phase3_ops_node,
        "required_ev_offset_max",
        0.0030,
        -0.20,
        0.20
    );
    if (phase3_policy_.operations_control.required_ev_offset_max <
        phase3_policy_.operations_control.required_ev_offset_min) {
        std::swap(
            phase3_policy_.operations_control.required_ev_offset_min,
            phase3_policy_.operations_control.required_ev_offset_max
        );
    }
    phase3_policy_.operations_control.required_ev_offset = parseCostParam(
        phase3_ops_node,
        "required_ev_offset",
        0.0,
        phase3_policy_.operations_control.required_ev_offset_min,
        phase3_policy_.operations_control.required_ev_offset_max
    );
    phase3_policy_.operations_control.required_ev_offset_trending_add = parseCostParam(
        phase3_ops_node,
        "required_ev_offset_trending_add",
        0.0,
        -0.20,
        0.20
    );
    phase3_policy_.operations_control.k_margin_scale_min = parseCostParam(
        phase3_ops_node,
        "k_margin_scale_min",
        0.50,
        0.0,
        10.0
    );
    phase3_policy_.operations_control.k_margin_scale_max = parseCostParam(
        phase3_ops_node,
        "k_margin_scale_max",
        2.00,
        0.0,
        10.0
    );
    if (phase3_policy_.operations_control.k_margin_scale_max <
        phase3_policy_.operations_control.k_margin_scale_min) {
        std::swap(
            phase3_policy_.operations_control.k_margin_scale_min,
            phase3_policy_.operations_control.k_margin_scale_max
        );
    }
    phase3_policy_.operations_control.k_margin_scale = parseCostParam(
        phase3_ops_node,
        "k_margin_scale",
        1.0,
        phase3_policy_.operations_control.k_margin_scale_min,
        phase3_policy_.operations_control.k_margin_scale_max
    );
    phase3_policy_.operations_control.ev_blend_scale_min = parseCostParam(
        phase3_ops_node,
        "ev_blend_scale_min",
        0.50,
        0.0,
        10.0
    );
    phase3_policy_.operations_control.ev_blend_scale_max = parseCostParam(
        phase3_ops_node,
        "ev_blend_scale_max",
        1.50,
        0.0,
        10.0
    );
    if (phase3_policy_.operations_control.ev_blend_scale_max <
        phase3_policy_.operations_control.ev_blend_scale_min) {
        std::swap(
            phase3_policy_.operations_control.ev_blend_scale_min,
            phase3_policy_.operations_control.ev_blend_scale_max
        );
    }
    phase3_policy_.operations_control.ev_blend_scale = parseCostParam(
        phase3_ops_node,
        "ev_blend_scale",
        1.0,
        phase3_policy_.operations_control.ev_blend_scale_min,
        phase3_policy_.operations_control.ev_blend_scale_max
    );
    phase3_policy_.operations_control.max_step_per_update = parseCostParam(
        phase3_ops_node,
        "max_step_per_update",
        0.05,
        0.0,
        1.0
    );
    phase3_policy_.operations_control.min_update_interval_sec = std::clamp(
        phase3_ops_node.value("min_update_interval_sec", 3600),
        0,
        86400 * 30
    );

    phase3_policy_.primary_minimums.enabled = phase3_primary_minimums_node.value("enabled", false);
    phase3_policy_.primary_minimums.min_h5_calibrated = parseCostParam(
        phase3_primary_minimums_node,
        "min_h5_calibrated",
        0.48,
        0.0,
        1.0
    );
    phase3_policy_.primary_minimums.min_h5_margin = parseCostParam(
        phase3_primary_minimums_node,
        "min_h5_margin",
        -0.03,
        -0.50,
        0.20
    );
    phase3_policy_.primary_minimums.min_liquidity_score = parseCostParam(
        phase3_primary_minimums_node,
        "min_liquidity_score",
        42.0,
        0.0,
        100.0
    );
    phase3_policy_.primary_minimums.min_signal_strength = parseCostParam(
        phase3_primary_minimums_node,
        "min_signal_strength",
        0.34,
        0.0,
        1.0
    );
    phase3_policy_.primary_minimums.hostile_add_h5_calibrated = parseCostParam(
        phase3_primary_minimums_node,
        "hostile_add_h5_calibrated",
        0.06,
        -1.0,
        1.0
    );
    phase3_policy_.primary_minimums.hostile_add_h5_margin = parseCostParam(
        phase3_primary_minimums_node,
        "hostile_add_h5_margin",
        0.03,
        -1.0,
        1.0
    );
    phase3_policy_.primary_minimums.hostile_add_liquidity_score = parseCostParam(
        phase3_primary_minimums_node,
        "hostile_add_liquidity_score",
        10.0,
        -100.0,
        100.0
    );
    phase3_policy_.primary_minimums.hostile_add_signal_strength = parseCostParam(
        phase3_primary_minimums_node,
        "hostile_add_signal_strength",
        0.08,
        -1.0,
        1.0
    );
    phase3_policy_.primary_minimums.ranging_add_h5_calibrated = parseCostParam(
        phase3_primary_minimums_node,
        "ranging_add_h5_calibrated",
        0.01,
        -1.0,
        1.0
    );
    phase3_policy_.primary_minimums.ranging_add_h5_margin = parseCostParam(
        phase3_primary_minimums_node,
        "ranging_add_h5_margin",
        0.005,
        -1.0,
        1.0
    );
    phase3_policy_.primary_minimums.ranging_add_liquidity_score = parseCostParam(
        phase3_primary_minimums_node,
        "ranging_add_liquidity_score",
        2.0,
        -100.0,
        100.0
    );
    phase3_policy_.primary_minimums.ranging_add_signal_strength = parseCostParam(
        phase3_primary_minimums_node,
        "ranging_add_signal_strength",
        0.02,
        -1.0,
        1.0
    );
    phase3_policy_.primary_minimums.trending_up_add_h5_calibrated = parseCostParam(
        phase3_primary_minimums_node,
        "trending_up_add_h5_calibrated",
        -0.02,
        -1.0,
        1.0
    );
    phase3_policy_.primary_minimums.trending_up_add_h5_margin = parseCostParam(
        phase3_primary_minimums_node,
        "trending_up_add_h5_margin",
        -0.01,
        -1.0,
        1.0
    );
    phase3_policy_.primary_minimums.trending_up_add_liquidity_score = parseCostParam(
        phase3_primary_minimums_node,
        "trending_up_add_liquidity_score",
        -5.0,
        -100.0,
        100.0
    );
    phase3_policy_.primary_minimums.trending_up_add_signal_strength = parseCostParam(
        phase3_primary_minimums_node,
        "trending_up_add_signal_strength",
        -0.03,
        -1.0,
        1.0
    );
    phase3_policy_.primary_minimums.regime_volatile_add_signal_strength = parseCostParam(
        phase3_primary_minimums_node,
        "regime_volatile_add_signal_strength",
        0.03,
        -1.0,
        1.0
    );
    phase3_policy_.primary_minimums.regime_hostile_add_liquidity_score = parseCostParam(
        phase3_primary_minimums_node,
        "regime_hostile_add_liquidity_score",
        8.0,
        -100.0,
        100.0
    );
    phase3_policy_.primary_minimums.regime_hostile_add_signal_strength = parseCostParam(
        phase3_primary_minimums_node,
        "regime_hostile_add_signal_strength",
        0.08,
        -1.0,
        1.0
    );
    phase3_policy_.primary_minimums.clamp_h5_margin_min = parseCostParam(
        phase3_primary_minimums_node,
        "clamp_h5_margin_min",
        -0.50,
        -1.0,
        1.0
    );
    phase3_policy_.primary_minimums.clamp_h5_margin_max = parseCostParam(
        phase3_primary_minimums_node,
        "clamp_h5_margin_max",
        0.20,
        -1.0,
        1.0
    );
    if (phase3_policy_.primary_minimums.clamp_h5_margin_max <
        phase3_policy_.primary_minimums.clamp_h5_margin_min) {
        std::swap(
            phase3_policy_.primary_minimums.clamp_h5_margin_min,
            phase3_policy_.primary_minimums.clamp_h5_margin_max
        );
    }
    phase3_policy_.primary_minimums.clamp_liquidity_min = parseCostParam(
        phase3_primary_minimums_node,
        "clamp_liquidity_min",
        0.0,
        0.0,
        100.0
    );
    phase3_policy_.primary_minimums.clamp_liquidity_max = parseCostParam(
        phase3_primary_minimums_node,
        "clamp_liquidity_max",
        100.0,
        0.0,
        100.0
    );
    if (phase3_policy_.primary_minimums.clamp_liquidity_max <
        phase3_policy_.primary_minimums.clamp_liquidity_min) {
        std::swap(
            phase3_policy_.primary_minimums.clamp_liquidity_min,
            phase3_policy_.primary_minimums.clamp_liquidity_max
        );
    }
    phase3_policy_.primary_minimums.clamp_strength_min = parseCostParam(
        phase3_primary_minimums_node,
        "clamp_strength_min",
        0.0,
        0.0,
        1.0
    );
    phase3_policy_.primary_minimums.clamp_strength_max = parseCostParam(
        phase3_primary_minimums_node,
        "clamp_strength_max",
        1.0,
        0.0,
        1.0
    );
    if (phase3_policy_.primary_minimums.clamp_strength_max <
        phase3_policy_.primary_minimums.clamp_strength_min) {
        std::swap(
            phase3_policy_.primary_minimums.clamp_strength_min,
            phase3_policy_.primary_minimums.clamp_strength_max
        );
    }

    phase3_policy_.primary_priority.enabled = phase3_primary_priority_node.value("enabled", false);
    phase3_policy_.primary_priority.margin_score_shift = parseCostParam(
        phase3_primary_priority_node,
        "margin_score_shift",
        0.10,
        -1.0,
        2.0
    );
    phase3_policy_.primary_priority.margin_score_scale = parseCostParam(
        phase3_primary_priority_node,
        "margin_score_scale",
        0.20,
        1e-6,
        10.0
    );
    phase3_policy_.primary_priority.edge_score_shift = parseCostParam(
        phase3_primary_priority_node,
        "edge_score_shift",
        0.0005,
        -1.0,
        1.0
    );
    phase3_policy_.primary_priority.edge_score_scale = parseCostParam(
        phase3_primary_priority_node,
        "edge_score_scale",
        0.0025,
        1e-6,
        1.0
    );
    phase3_policy_.primary_priority.prob_weight = parseCostParam(
        phase3_primary_priority_node,
        "prob_weight",
        0.50,
        -10.0,
        10.0
    );
    phase3_policy_.primary_priority.margin_weight = parseCostParam(
        phase3_primary_priority_node,
        "margin_weight",
        0.22,
        -10.0,
        10.0
    );
    phase3_policy_.primary_priority.liquidity_weight = parseCostParam(
        phase3_primary_priority_node,
        "liquidity_weight",
        0.10,
        -10.0,
        10.0
    );
    phase3_policy_.primary_priority.strength_weight = parseCostParam(
        phase3_primary_priority_node,
        "strength_weight",
        0.10,
        -10.0,
        10.0
    );
    phase3_policy_.primary_priority.edge_weight = parseCostParam(
        phase3_primary_priority_node,
        "edge_weight",
        0.08,
        -10.0,
        10.0
    );
    phase3_policy_.primary_priority.hostile_prob_weight = parseCostParam(
        phase3_primary_priority_node,
        "hostile_prob_weight",
        0.54,
        -10.0,
        10.0
    );
    phase3_policy_.primary_priority.hostile_margin_weight = parseCostParam(
        phase3_primary_priority_node,
        "hostile_margin_weight",
        0.22,
        -10.0,
        10.0
    );
    phase3_policy_.primary_priority.hostile_liquidity_weight = parseCostParam(
        phase3_primary_priority_node,
        "hostile_liquidity_weight",
        0.11,
        -10.0,
        10.0
    );
    phase3_policy_.primary_priority.hostile_strength_weight = parseCostParam(
        phase3_primary_priority_node,
        "hostile_strength_weight",
        0.09,
        -10.0,
        10.0
    );
    phase3_policy_.primary_priority.hostile_edge_weight = parseCostParam(
        phase3_primary_priority_node,
        "hostile_edge_weight",
        0.04,
        -10.0,
        10.0
    );
    phase3_policy_.primary_priority.strong_buy_bonus = parseCostParam(
        phase3_primary_priority_node,
        "strong_buy_bonus",
        0.02,
        -1.0,
        1.0
    );
    phase3_policy_.primary_priority.margin_bonus_scale = parseCostParam(
        phase3_primary_priority_node,
        "margin_bonus_scale",
        0.08,
        -10.0,
        10.0
    );
    phase3_policy_.primary_priority.margin_bonus_cap = parseCostParam(
        phase3_primary_priority_node,
        "margin_bonus_cap",
        0.03,
        0.0,
        1.0
    );
    phase3_policy_.primary_priority.range_penalty = parseCostParam(
        phase3_primary_priority_node,
        "range_penalty",
        0.11,
        0.0,
        1.0
    );
    phase3_policy_.primary_priority.range_bonus = parseCostParam(
        phase3_primary_priority_node,
        "range_bonus",
        0.03,
        -1.0,
        1.0
    );
    phase3_policy_.primary_priority.range_penalty_strength_floor = parseCostParam(
        phase3_primary_priority_node,
        "range_penalty_strength_floor",
        0.50,
        0.0,
        1.0
    );
    phase3_policy_.primary_priority.range_penalty_prob_floor = parseCostParam(
        phase3_primary_priority_node,
        "range_penalty_prob_floor",
        0.54,
        0.0,
        1.0
    );
    phase3_policy_.primary_priority.range_bonus_prob_floor = parseCostParam(
        phase3_primary_priority_node,
        "range_bonus_prob_floor",
        0.57,
        0.0,
        1.0
    );
    phase3_policy_.primary_priority.uptrend_bonus = parseCostParam(
        phase3_primary_priority_node,
        "uptrend_bonus",
        0.03,
        -1.0,
        1.0
    );
    phase3_policy_.primary_priority.uptrend_bonus_prob_floor = parseCostParam(
        phase3_primary_priority_node,
        "uptrend_bonus_prob_floor",
        0.52,
        0.0,
        1.0
    );

    phase3_policy_.primary_decision_profile.enabled =
        phase3_primary_decision_profile_node.value("enabled", false);
    phase3_policy_.primary_decision_profile.confidence_prob_shift = parseCostParam(
        phase3_primary_decision_profile_node,
        "confidence_prob_shift",
        0.50,
        -1.0,
        2.0
    );
    phase3_policy_.primary_decision_profile.confidence_prob_scale = parseCostParam(
        phase3_primary_decision_profile_node,
        "confidence_prob_scale",
        0.20,
        1e-6,
        10.0
    );
    phase3_policy_.primary_decision_profile.confidence_margin_shift = parseCostParam(
        phase3_primary_decision_profile_node,
        "confidence_margin_shift",
        0.01,
        -1.0,
        2.0
    );
    phase3_policy_.primary_decision_profile.confidence_margin_scale = parseCostParam(
        phase3_primary_decision_profile_node,
        "confidence_margin_scale",
        0.08,
        1e-6,
        10.0
    );
    phase3_policy_.primary_decision_profile.confidence_prob_weight = parseCostParam(
        phase3_primary_decision_profile_node,
        "confidence_prob_weight",
        0.65,
        0.0,
        10.0
    );
    phase3_policy_.primary_decision_profile.confidence_margin_weight = parseCostParam(
        phase3_primary_decision_profile_node,
        "confidence_margin_weight",
        0.35,
        0.0,
        10.0
    );
    phase3_policy_.primary_decision_profile.target_strength_base = parseCostParam(
        phase3_primary_decision_profile_node,
        "target_strength_base",
        0.22,
        -10.0,
        10.0
    );
    phase3_policy_.primary_decision_profile.target_strength_prob_weight = parseCostParam(
        phase3_primary_decision_profile_node,
        "target_strength_prob_weight",
        0.60,
        -20.0,
        20.0
    );
    phase3_policy_.primary_decision_profile.target_strength_margin_weight = parseCostParam(
        phase3_primary_decision_profile_node,
        "target_strength_margin_weight",
        1.80,
        -20.0,
        20.0
    );
    phase3_policy_.primary_decision_profile.target_strength_min_hostile = parseCostParam(
        phase3_primary_decision_profile_node,
        "target_strength_min_hostile",
        0.26,
        0.0,
        1.0
    );
    phase3_policy_.primary_decision_profile.target_strength_min_calm = parseCostParam(
        phase3_primary_decision_profile_node,
        "target_strength_min_calm",
        0.18,
        0.0,
        1.0
    );
    phase3_policy_.primary_decision_profile.target_strength_max = parseCostParam(
        phase3_primary_decision_profile_node,
        "target_strength_max",
        0.98,
        0.0,
        1.0
    );
    phase3_policy_.primary_decision_profile.strength_blend_old_weight = parseCostParam(
        phase3_primary_decision_profile_node,
        "strength_blend_old_weight",
        0.25,
        0.0,
        1.0
    );
    phase3_policy_.primary_decision_profile.strength_blend_target_weight = parseCostParam(
        phase3_primary_decision_profile_node,
        "strength_blend_target_weight",
        0.75,
        0.0,
        1.0
    );
    phase3_policy_.primary_decision_profile.target_filter_base = parseCostParam(
        phase3_primary_decision_profile_node,
        "target_filter_base",
        0.42,
        -10.0,
        10.0
    );
    phase3_policy_.primary_decision_profile.target_filter_prob_weight = parseCostParam(
        phase3_primary_decision_profile_node,
        "target_filter_prob_weight",
        0.40,
        -20.0,
        20.0
    );
    phase3_policy_.primary_decision_profile.target_filter_margin_weight = parseCostParam(
        phase3_primary_decision_profile_node,
        "target_filter_margin_weight",
        0.70,
        -20.0,
        20.0
    );
    phase3_policy_.primary_decision_profile.target_filter_min = parseCostParam(
        phase3_primary_decision_profile_node,
        "target_filter_min",
        0.20,
        0.0,
        1.0
    );
    phase3_policy_.primary_decision_profile.target_filter_max = parseCostParam(
        phase3_primary_decision_profile_node,
        "target_filter_max",
        0.95,
        0.0,
        1.0
    );
    phase3_policy_.primary_decision_profile.filter_blend_old_weight = parseCostParam(
        phase3_primary_decision_profile_node,
        "filter_blend_old_weight",
        0.30,
        0.0,
        1.0
    );
    phase3_policy_.primary_decision_profile.filter_blend_target_weight = parseCostParam(
        phase3_primary_decision_profile_node,
        "filter_blend_target_weight",
        0.70,
        0.0,
        1.0
    );
    if (phase3_policy_.primary_decision_profile.target_strength_max <
        phase3_policy_.primary_decision_profile.target_strength_min_hostile) {
        phase3_policy_.primary_decision_profile.target_strength_max =
            phase3_policy_.primary_decision_profile.target_strength_min_hostile;
    }
    if (phase3_policy_.primary_decision_profile.target_strength_max <
        phase3_policy_.primary_decision_profile.target_strength_min_calm) {
        phase3_policy_.primary_decision_profile.target_strength_max =
            phase3_policy_.primary_decision_profile.target_strength_min_calm;
    }
    if (phase3_policy_.primary_decision_profile.target_filter_max <
        phase3_policy_.primary_decision_profile.target_filter_min) {
        std::swap(
            phase3_policy_.primary_decision_profile.target_filter_min,
            phase3_policy_.primary_decision_profile.target_filter_max
        );
    }
    phase3_policy_.primary_decision_profile.implied_win_margin_weight = parseCostParam(
        phase3_primary_decision_profile_node,
        "implied_win_margin_weight",
        0.45,
        -20.0,
        20.0
    );
    phase3_policy_.primary_decision_profile.implied_win_threshold_gap_weight = parseCostParam(
        phase3_primary_decision_profile_node,
        "implied_win_threshold_gap_weight",
        0.35,
        -20.0,
        20.0
    );
    phase3_policy_.primary_decision_profile.implied_win_min_hostile = parseCostParam(
        phase3_primary_decision_profile_node,
        "implied_win_min_hostile",
        0.44,
        0.0,
        1.0
    );
    phase3_policy_.primary_decision_profile.implied_win_min_calm = parseCostParam(
        phase3_primary_decision_profile_node,
        "implied_win_min_calm",
        0.40,
        0.0,
        1.0
    );
    phase3_policy_.primary_decision_profile.implied_win_max = parseCostParam(
        phase3_primary_decision_profile_node,
        "implied_win_max",
        0.86,
        0.0,
        1.0
    );
    if (phase3_policy_.primary_decision_profile.implied_win_max <
        phase3_policy_.primary_decision_profile.implied_win_min_hostile) {
        phase3_policy_.primary_decision_profile.implied_win_max =
            phase3_policy_.primary_decision_profile.implied_win_min_hostile;
    }
    if (phase3_policy_.primary_decision_profile.implied_win_max <
        phase3_policy_.primary_decision_profile.implied_win_min_calm) {
        phase3_policy_.primary_decision_profile.implied_win_max =
            phase3_policy_.primary_decision_profile.implied_win_min_calm;
    }
    phase3_policy_.primary_decision_profile.probabilistic_edge_floor_margin_weight = parseCostParam(
        phase3_primary_decision_profile_node,
        "probabilistic_edge_floor_margin_weight",
        0.0012,
        -1.0,
        1.0
    );
    phase3_policy_.primary_decision_profile.probabilistic_edge_floor_prob_weight = parseCostParam(
        phase3_primary_decision_profile_node,
        "probabilistic_edge_floor_prob_weight",
        0.0010,
        -1.0,
        1.0
    );
    phase3_policy_.primary_decision_profile.probabilistic_edge_floor_prob_center = parseCostParam(
        phase3_primary_decision_profile_node,
        "probabilistic_edge_floor_prob_center",
        0.50,
        0.0,
        1.0
    );
    phase3_policy_.primary_decision_profile.probabilistic_edge_floor_penalty_hostile = parseCostParam(
        phase3_primary_decision_profile_node,
        "probabilistic_edge_floor_penalty_hostile",
        0.00045,
        -1.0,
        1.0
    );
    phase3_policy_.primary_decision_profile.probabilistic_edge_floor_penalty_calm = parseCostParam(
        phase3_primary_decision_profile_node,
        "probabilistic_edge_floor_penalty_calm",
        0.00030,
        -1.0,
        1.0
    );
    phase3_policy_.primary_decision_profile.current_risk_min = parseCostParam(
        phase3_primary_decision_profile_node,
        "current_risk_min",
        0.0010,
        0.0,
        1.0
    );
    phase3_policy_.primary_decision_profile.current_risk_max = parseCostParam(
        phase3_primary_decision_profile_node,
        "current_risk_max",
        0.0500,
        0.0,
        1.0
    );
    if (phase3_policy_.primary_decision_profile.current_risk_max <
        phase3_policy_.primary_decision_profile.current_risk_min) {
        std::swap(
            phase3_policy_.primary_decision_profile.current_risk_min,
            phase3_policy_.primary_decision_profile.current_risk_max
        );
    }
    phase3_policy_.primary_decision_profile.target_risk_base_hostile = parseCostParam(
        phase3_primary_decision_profile_node,
        "target_risk_base_hostile",
        0.0026,
        0.0,
        1.0
    );
    phase3_policy_.primary_decision_profile.target_risk_base_calm = parseCostParam(
        phase3_primary_decision_profile_node,
        "target_risk_base_calm",
        0.0028,
        0.0,
        1.0
    );
    phase3_policy_.primary_decision_profile.target_risk_confidence_scale_hostile = parseCostParam(
        phase3_primary_decision_profile_node,
        "target_risk_confidence_scale_hostile",
        0.0036,
        0.0,
        1.0
    );
    phase3_policy_.primary_decision_profile.target_risk_confidence_scale_calm = parseCostParam(
        phase3_primary_decision_profile_node,
        "target_risk_confidence_scale_calm",
        0.0048,
        0.0,
        1.0
    );
    phase3_policy_.primary_decision_profile.fragility_target_risk_multiplier = parseCostParam(
        phase3_primary_decision_profile_node,
        "fragility_target_risk_multiplier",
        0.78,
        0.0,
        2.0
    );
    phase3_policy_.primary_decision_profile.fragility_negative_margin_threshold = parseCostParam(
        phase3_primary_decision_profile_node,
        "fragility_negative_margin_threshold",
        -0.006,
        -1.0,
        1.0
    );
    phase3_policy_.primary_decision_profile.fragility_negative_margin_target_risk_multiplier = parseCostParam(
        phase3_primary_decision_profile_node,
        "fragility_negative_margin_target_risk_multiplier",
        0.90,
        0.0,
        2.0
    );
    phase3_policy_.primary_decision_profile.target_risk_min_hostile = parseCostParam(
        phase3_primary_decision_profile_node,
        "target_risk_min_hostile",
        0.0022,
        0.0,
        1.0
    );
    phase3_policy_.primary_decision_profile.target_risk_min_calm = parseCostParam(
        phase3_primary_decision_profile_node,
        "target_risk_min_calm",
        0.0024,
        0.0,
        1.0
    );
    phase3_policy_.primary_decision_profile.target_risk_max_hostile = parseCostParam(
        phase3_primary_decision_profile_node,
        "target_risk_max_hostile",
        0.0075,
        0.0,
        1.0
    );
    phase3_policy_.primary_decision_profile.target_risk_max_calm = parseCostParam(
        phase3_primary_decision_profile_node,
        "target_risk_max_calm",
        0.0105,
        0.0,
        1.0
    );
    phase3_policy_.primary_decision_profile.blended_risk_old_weight = parseCostParam(
        phase3_primary_decision_profile_node,
        "blended_risk_old_weight",
        0.35,
        0.0,
        1.0
    );
    phase3_policy_.primary_decision_profile.blended_risk_target_weight = parseCostParam(
        phase3_primary_decision_profile_node,
        "blended_risk_target_weight",
        0.65,
        0.0,
        1.0
    );
    phase3_policy_.primary_decision_profile.blended_risk_min_hostile = parseCostParam(
        phase3_primary_decision_profile_node,
        "blended_risk_min_hostile",
        0.0022,
        0.0,
        1.0
    );
    phase3_policy_.primary_decision_profile.blended_risk_min_calm = parseCostParam(
        phase3_primary_decision_profile_node,
        "blended_risk_min_calm",
        0.0024,
        0.0,
        1.0
    );
    phase3_policy_.primary_decision_profile.blended_risk_max_hostile = parseCostParam(
        phase3_primary_decision_profile_node,
        "blended_risk_max_hostile",
        0.0080,
        0.0,
        1.0
    );
    phase3_policy_.primary_decision_profile.blended_risk_max_calm = parseCostParam(
        phase3_primary_decision_profile_node,
        "blended_risk_max_calm",
        0.0120,
        0.0,
        1.0
    );
    phase3_policy_.primary_decision_profile.rr_base_hostile = parseCostParam(
        phase3_primary_decision_profile_node,
        "rr_base_hostile",
        1.10,
        0.0,
        10.0
    );
    phase3_policy_.primary_decision_profile.rr_base_calm = parseCostParam(
        phase3_primary_decision_profile_node,
        "rr_base_calm",
        1.20,
        0.0,
        10.0
    );
    phase3_policy_.primary_decision_profile.rr_confidence_weight_hostile = parseCostParam(
        phase3_primary_decision_profile_node,
        "rr_confidence_weight_hostile",
        1.00,
        0.0,
        10.0
    );
    phase3_policy_.primary_decision_profile.rr_confidence_weight_calm = parseCostParam(
        phase3_primary_decision_profile_node,
        "rr_confidence_weight_calm",
        1.30,
        0.0,
        10.0
    );
    phase3_policy_.primary_decision_profile.rr_margin_positive_weight_hostile = parseCostParam(
        phase3_primary_decision_profile_node,
        "rr_margin_positive_weight_hostile",
        2.0,
        0.0,
        20.0
    );
    phase3_policy_.primary_decision_profile.rr_margin_positive_weight_calm = parseCostParam(
        phase3_primary_decision_profile_node,
        "rr_margin_positive_weight_calm",
        2.8,
        0.0,
        20.0
    );
    phase3_policy_.primary_decision_profile.fragility_rr_bonus = parseCostParam(
        phase3_primary_decision_profile_node,
        "fragility_rr_bonus",
        0.18,
        -10.0,
        10.0
    );
    phase3_policy_.primary_decision_profile.rr_min_hostile = parseCostParam(
        phase3_primary_decision_profile_node,
        "rr_min_hostile",
        1.05,
        0.0,
        10.0
    );
    phase3_policy_.primary_decision_profile.rr_min_calm = parseCostParam(
        phase3_primary_decision_profile_node,
        "rr_min_calm",
        1.10,
        0.0,
        10.0
    );
    phase3_policy_.primary_decision_profile.rr_max_hostile = parseCostParam(
        phase3_primary_decision_profile_node,
        "rr_max_hostile",
        2.40,
        0.0,
        10.0
    );
    phase3_policy_.primary_decision_profile.rr_max_calm = parseCostParam(
        phase3_primary_decision_profile_node,
        "rr_max_calm",
        3.20,
        0.0,
        10.0
    );
    phase3_policy_.primary_decision_profile.tp1_rr_min = parseCostParam(
        phase3_primary_decision_profile_node,
        "tp1_rr_min",
        1.0,
        0.0,
        10.0
    );
    phase3_policy_.primary_decision_profile.tp1_rr_multiplier = parseCostParam(
        phase3_primary_decision_profile_node,
        "tp1_rr_multiplier",
        0.55,
        0.0,
        10.0
    );
    phase3_policy_.primary_decision_profile.breakeven_mult_fragility = parseCostParam(
        phase3_primary_decision_profile_node,
        "breakeven_mult_fragility",
        0.48,
        0.0,
        10.0
    );
    phase3_policy_.primary_decision_profile.breakeven_mult_default = parseCostParam(
        phase3_primary_decision_profile_node,
        "breakeven_mult_default",
        0.70,
        0.0,
        10.0
    );
    phase3_policy_.primary_decision_profile.trailing_mult_fragility = parseCostParam(
        phase3_primary_decision_profile_node,
        "trailing_mult_fragility",
        0.82,
        0.0,
        10.0
    );
    phase3_policy_.primary_decision_profile.trailing_mult_default = parseCostParam(
        phase3_primary_decision_profile_node,
        "trailing_mult_default",
        1.10,
        0.0,
        10.0
    );
    phase3_policy_.primary_decision_profile.size_base = parseCostParam(
        phase3_primary_decision_profile_node,
        "size_base",
        0.42,
        0.0,
        10.0
    );
    phase3_policy_.primary_decision_profile.size_confidence_weight = parseCostParam(
        phase3_primary_decision_profile_node,
        "size_confidence_weight",
        0.80,
        0.0,
        10.0
    );
    phase3_policy_.primary_decision_profile.size_margin_positive_weight = parseCostParam(
        phase3_primary_decision_profile_node,
        "size_margin_positive_weight",
        1.20,
        0.0,
        20.0
    );
    phase3_policy_.primary_decision_profile.size_negative_margin_multiplier = parseCostParam(
        phase3_primary_decision_profile_node,
        "size_negative_margin_multiplier",
        0.86,
        0.0,
        10.0
    );
    phase3_policy_.primary_decision_profile.size_hostile_multiplier = parseCostParam(
        phase3_primary_decision_profile_node,
        "size_hostile_multiplier",
        0.88,
        0.0,
        10.0
    );
    phase3_policy_.primary_decision_profile.size_min = parseCostParam(
        phase3_primary_decision_profile_node,
        "size_min",
        0.30,
        0.0,
        10.0
    );
    phase3_policy_.primary_decision_profile.size_max = parseCostParam(
        phase3_primary_decision_profile_node,
        "size_max",
        1.35,
        0.0,
        10.0
    );
    if (phase3_policy_.primary_decision_profile.size_max <
        phase3_policy_.primary_decision_profile.size_min) {
        std::swap(
            phase3_policy_.primary_decision_profile.size_min,
            phase3_policy_.primary_decision_profile.size_max
        );
    }
    phase3_policy_.primary_decision_profile.score_margin_boost_cap = parseCostParam(
        phase3_primary_decision_profile_node,
        "score_margin_boost_cap",
        0.12,
        0.0,
        1.0
    );
    phase3_policy_.primary_decision_profile.filter_margin_boost_scale = parseCostParam(
        phase3_primary_decision_profile_node,
        "filter_margin_boost_scale",
        0.10,
        -10.0,
        10.0
    );
    phase3_policy_.primary_decision_profile.edge_clip_abs_pct = parseCostParam(
        phase3_primary_decision_profile_node,
        "edge_clip_abs_pct",
        0.05,
        0.0,
        1.0
    );
    phase3_policy_.primary_decision_profile.primary_nudge_base = parseCostParam(
        phase3_primary_decision_profile_node,
        "primary_nudge_base",
        0.20,
        -10.0,
        10.0
    );
    phase3_policy_.primary_decision_profile.primary_nudge_blend_scale = parseCostParam(
        phase3_primary_decision_profile_node,
        "primary_nudge_blend_scale",
        0.25,
        -10.0,
        10.0
    );
    phase3_policy_.primary_decision_profile.primary_filter_nudge_base = parseCostParam(
        phase3_primary_decision_profile_node,
        "primary_filter_nudge_base",
        0.10,
        -10.0,
        10.0
    );
    phase3_policy_.primary_decision_profile.primary_filter_nudge_blend_scale = parseCostParam(
        phase3_primary_decision_profile_node,
        "primary_filter_nudge_blend_scale",
        0.10,
        -10.0,
        10.0
    );

        phase3_policy_.diagnostics_v2.enabled = phase3_policy_.phase3_diagnostics_v2_enabled;

    const auto phase4_root = root.value("phase4", nlohmann::json::object());
    const auto phase4_portfolio_allocator_node =
        phase4_root.value("portfolio_allocator", nlohmann::json::object());
    const auto phase4_correlation_control_node =
        phase4_root.value("correlation_control", nlohmann::json::object());
    const auto phase4_risk_budget_node =
        phase4_root.value("risk_budget", nlohmann::json::object());
    const auto phase4_drawdown_governor_node =
        phase4_root.value("drawdown_governor", nlohmann::json::object());
    const auto phase4_execution_aware_sizing_node =
        phase4_root.value("execution_aware_sizing", nlohmann::json::object());
    const auto phase4_diagnostics_node =
        phase4_root.value("portfolio_diagnostics", nlohmann::json::object());

    phase4_policy_.portfolio_allocator.enabled =
        phase4_portfolio_allocator_node.value("enabled", false);
    phase4_policy_.portfolio_allocator.top_k = static_cast<int>(std::llround(parseCostParam(
        phase4_portfolio_allocator_node,
        "top_k",
        1.0,
        1.0,
        256.0
    )));
    phase4_policy_.portfolio_allocator.min_score = parseCostParam(
        phase4_portfolio_allocator_node,
        "min_score",
        -1.0e6,
        -1.0e6,
        1.0e6
    );
    phase4_policy_.portfolio_allocator.lambda_tail = parseCostParam(
        phase4_portfolio_allocator_node,
        "lambda_tail",
        1.0,
        0.0,
        100.0
    );
    phase4_policy_.portfolio_allocator.lambda_cost = parseCostParam(
        phase4_portfolio_allocator_node,
        "lambda_cost",
        1.0,
        0.0,
        100.0
    );
    phase4_policy_.portfolio_allocator.lambda_uncertainty = parseCostParam(
        phase4_portfolio_allocator_node,
        "lambda_uncertainty",
        1.0,
        0.0,
        100.0
    );
    phase4_policy_.portfolio_allocator.lambda_margin = parseCostParam(
        phase4_portfolio_allocator_node,
        "lambda_margin",
        1.0,
        0.0,
        100.0
    );
    phase4_policy_.portfolio_allocator.uncertainty_prob_weight = parseCostParam(
        phase4_portfolio_allocator_node,
        "uncertainty_prob_weight",
        0.50,
        0.0,
        100.0
    );
    phase4_policy_.portfolio_allocator.uncertainty_ev_weight = parseCostParam(
        phase4_portfolio_allocator_node,
        "uncertainty_ev_weight",
        0.50,
        0.0,
        100.0
    );
    const double uncertainty_weight_sum =
        phase4_policy_.portfolio_allocator.uncertainty_prob_weight +
        phase4_policy_.portfolio_allocator.uncertainty_ev_weight;
    if (uncertainty_weight_sum > 1e-9) {
        phase4_policy_.portfolio_allocator.uncertainty_prob_weight /=
            uncertainty_weight_sum;
        phase4_policy_.portfolio_allocator.uncertainty_ev_weight /=
            uncertainty_weight_sum;
    } else {
        phase4_policy_.portfolio_allocator.uncertainty_prob_weight = 0.50;
        phase4_policy_.portfolio_allocator.uncertainty_ev_weight = 0.50;
    }

    phase4_policy_.phase4_portfolio_allocator_enabled =
        phase4_policy_.portfolio_allocator.enabled;
    phase4_policy_.correlation_control.enabled =
        phase4_correlation_control_node.value("enabled", false);
    phase4_policy_.correlation_control.default_cluster_cap = parseCostParam(
        phase4_correlation_control_node,
        "default_cluster_cap",
        1.0,
        0.0,
        10.0
    );
    phase4_policy_.correlation_control.penalty_weight = parseCostParam(
        phase4_correlation_control_node,
        "penalty_weight",
        0.0,
        0.0,
        100.0
    );
    phase4_policy_.correlation_control.penalty_utilization_trigger = parseCostParam(
        phase4_correlation_control_node,
        "penalty_utilization_trigger",
        0.85,
        0.0,
        1.0
    );
    phase4_policy_.correlation_control.penalty_reject_threshold = parseCostParam(
        phase4_correlation_control_node,
        "penalty_reject_threshold",
        1.0e9,
        0.0,
        1.0e9
    );
    phase4_policy_.correlation_control.cluster_cap_reallocate_in_allocator =
        phase4_correlation_control_node.value("cluster_cap_reallocate_in_allocator", false);
    phase4_policy_.correlation_control.market_cluster_map.clear();
    const auto cluster_map_node = phase4_correlation_control_node.value(
        "market_cluster_map",
        nlohmann::json::object()
    );
    if (cluster_map_node.is_object()) {
        for (auto it = cluster_map_node.begin(); it != cluster_map_node.end(); ++it) {
            if (!it.value().is_string()) {
                continue;
            }
            const std::string market = it.key();
            const std::string cluster_id = it.value().get<std::string>();
            if (market.empty() || cluster_id.empty()) {
                continue;
            }
            phase4_policy_.correlation_control.market_cluster_map[market] = cluster_id;
        }
    }
    phase4_policy_.correlation_control.cluster_caps.clear();
    const auto cluster_caps_node = phase4_correlation_control_node.value(
        "cluster_caps",
        nlohmann::json::object()
    );
    if (cluster_caps_node.is_object()) {
        for (auto it = cluster_caps_node.begin(); it != cluster_caps_node.end(); ++it) {
            if (!it.value().is_number()) {
                continue;
            }
            const std::string cluster_id = it.key();
            if (cluster_id.empty()) {
                continue;
            }
            phase4_policy_.correlation_control.cluster_caps[cluster_id] = std::clamp(
                it.value().get<double>(),
                0.0,
                10.0
            );
        }
    }
    phase4_policy_.phase4_correlation_control_enabled =
        phase4_policy_.correlation_control.enabled;
    phase4_policy_.risk_budget.enabled =
        phase4_risk_budget_node.value("enabled", false);
    phase4_policy_.risk_budget.per_market_cap = parseCostParam(
        phase4_risk_budget_node,
        "per_market_cap",
        1.0,
        0.0,
        1.0
    );
    phase4_policy_.risk_budget.gross_cap = parseCostParam(
        phase4_risk_budget_node,
        "gross_cap",
        1.0,
        0.0,
        10.0
    );
    phase4_policy_.risk_budget.risk_budget_cap = parseCostParam(
        phase4_risk_budget_node,
        "risk_budget_cap",
        1.0,
        0.0,
        10.0
    );
    phase4_policy_.risk_budget.risk_proxy_stop_pct = parseCostParam(
        phase4_risk_budget_node,
        "risk_proxy_stop_pct",
        0.03,
        0.0,
        1.0
    );
    phase4_policy_.risk_budget.regime_budget_multipliers.clear();
    const auto regime_budget_multipliers_node =
        phase4_risk_budget_node.value("regime_budget_multipliers", nlohmann::json::object());
    if (regime_budget_multipliers_node.is_object()) {
        for (auto it = regime_budget_multipliers_node.begin();
             it != regime_budget_multipliers_node.end();
             ++it) {
            if (!it.value().is_number()) {
                continue;
            }
            const std::string normalized_key = normalizeRegimeBudgetKeyForConfig(it.key());
            if (normalized_key.empty()) {
                continue;
            }
            const double multiplier = std::clamp(it.value().get<double>(), 0.0, 1.0);
            if (std::isfinite(multiplier)) {
                phase4_policy_.risk_budget.regime_budget_multipliers[normalized_key] = multiplier;
            }
        }
    }
    phase4_policy_.phase4_risk_budget_enabled =
        phase4_policy_.risk_budget.enabled;
    phase4_policy_.drawdown_governor.enabled =
        phase4_drawdown_governor_node.value("enabled", false);
    phase4_policy_.drawdown_governor.dd_threshold_soft = parseCostParam(
        phase4_drawdown_governor_node,
        "dd_threshold_soft",
        0.05,
        0.0,
        1.0
    );
    phase4_policy_.drawdown_governor.dd_threshold_hard = parseCostParam(
        phase4_drawdown_governor_node,
        "dd_threshold_hard",
        0.10,
        0.0,
        1.0
    );
    if (phase4_policy_.drawdown_governor.dd_threshold_hard <
        phase4_policy_.drawdown_governor.dd_threshold_soft) {
        std::swap(
            phase4_policy_.drawdown_governor.dd_threshold_soft,
            phase4_policy_.drawdown_governor.dd_threshold_hard
        );
    }
    phase4_policy_.drawdown_governor.budget_multiplier_soft = parseCostParam(
        phase4_drawdown_governor_node,
        "budget_multiplier_soft",
        0.70,
        0.0,
        1.0
    );
    phase4_policy_.drawdown_governor.budget_multiplier_hard = parseCostParam(
        phase4_drawdown_governor_node,
        "budget_multiplier_hard",
        0.40,
        0.0,
        1.0
    );
    if (phase4_policy_.drawdown_governor.budget_multiplier_hard >
        phase4_policy_.drawdown_governor.budget_multiplier_soft) {
        std::swap(
            phase4_policy_.drawdown_governor.budget_multiplier_soft,
            phase4_policy_.drawdown_governor.budget_multiplier_hard
        );
    }
    phase4_policy_.phase4_drawdown_governor_enabled =
        phase4_policy_.drawdown_governor.enabled;
    phase4_policy_.execution_aware_sizing.enabled =
        phase4_execution_aware_sizing_node.value("enabled", false);
    phase4_policy_.execution_aware_sizing.liquidity_low_threshold = parseCostParam(
        phase4_execution_aware_sizing_node,
        "liquidity_low_threshold",
        40.0,
        0.0,
        100.0
    );
    phase4_policy_.execution_aware_sizing.liquidity_mid_threshold = parseCostParam(
        phase4_execution_aware_sizing_node,
        "liquidity_mid_threshold",
        65.0,
        0.0,
        100.0
    );
    if (phase4_policy_.execution_aware_sizing.liquidity_mid_threshold <
        phase4_policy_.execution_aware_sizing.liquidity_low_threshold) {
        std::swap(
            phase4_policy_.execution_aware_sizing.liquidity_low_threshold,
            phase4_policy_.execution_aware_sizing.liquidity_mid_threshold
        );
    }
    phase4_policy_.execution_aware_sizing.liquidity_low_size_multiplier = parseCostParam(
        phase4_execution_aware_sizing_node,
        "liquidity_low_size_multiplier",
        0.50,
        0.0,
        1.0
    );
    phase4_policy_.execution_aware_sizing.liquidity_mid_size_multiplier = parseCostParam(
        phase4_execution_aware_sizing_node,
        "liquidity_mid_size_multiplier",
        0.75,
        0.0,
        1.0
    );
    phase4_policy_.execution_aware_sizing.liquidity_high_size_multiplier = parseCostParam(
        phase4_execution_aware_sizing_node,
        "liquidity_high_size_multiplier",
        1.00,
        0.0,
        1.0
    );
    phase4_policy_.execution_aware_sizing.tail_cost_soft_pct = parseCostParam(
        phase4_execution_aware_sizing_node,
        "tail_cost_soft_pct",
        0.0015,
        0.0,
        1.0
    );
    phase4_policy_.execution_aware_sizing.tail_cost_hard_pct = parseCostParam(
        phase4_execution_aware_sizing_node,
        "tail_cost_hard_pct",
        0.0030,
        0.0,
        1.0
    );
    if (phase4_policy_.execution_aware_sizing.tail_cost_hard_pct <
        phase4_policy_.execution_aware_sizing.tail_cost_soft_pct) {
        std::swap(
            phase4_policy_.execution_aware_sizing.tail_cost_soft_pct,
            phase4_policy_.execution_aware_sizing.tail_cost_hard_pct
        );
    }
    phase4_policy_.execution_aware_sizing.tail_soft_multiplier = parseCostParam(
        phase4_execution_aware_sizing_node,
        "tail_soft_multiplier",
        0.80,
        0.0,
        1.0
    );
    phase4_policy_.execution_aware_sizing.tail_hard_multiplier = parseCostParam(
        phase4_execution_aware_sizing_node,
        "tail_hard_multiplier",
        0.60,
        0.0,
        1.0
    );
    if (phase4_policy_.execution_aware_sizing.tail_hard_multiplier >
        phase4_policy_.execution_aware_sizing.tail_soft_multiplier) {
        std::swap(
            phase4_policy_.execution_aware_sizing.tail_soft_multiplier,
            phase4_policy_.execution_aware_sizing.tail_hard_multiplier
        );
    }
    phase4_policy_.execution_aware_sizing.min_position_size = parseCostParam(
        phase4_execution_aware_sizing_node,
        "min_position_size",
        0.01,
        0.0,
        1.0
    );
    phase4_policy_.phase4_execution_aware_sizing_enabled =
        phase4_policy_.execution_aware_sizing.enabled;
    phase4_policy_.phase4_portfolio_diagnostics_enabled =
        phase4_diagnostics_node.value("enabled", false);

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

    if (prob_model_backend_ == "lgbm") {
        std::string lgbm_model_path = trimCopy(root.value("lgbm_model_path", std::string{}));
        if (lgbm_model_path.empty()) {
            if (error_message != nullptr) {
                *error_message = "lgbm_backend_missing_model_path";
            }
            return false;
        }
        std::filesystem::path model_path_value = std::filesystem::path(lgbm_model_path);
        if (!model_path_value.is_absolute()) {
            std::filesystem::path bundle_path_value = std::filesystem::path(path);
            model_path_value = bundle_path_value.parent_path() / model_path_value;
        }
        model_path_value = model_path_value.lexically_normal();
        lgbm_model_path_resolved_ = model_path_value.string();

        std::string lgbm_parse_error;
        if (!parseLightGbmTextModel(
                lgbm_model_path_resolved_,
                feature_columns_.size(),
                lgbm_model_,
                &lgbm_parse_error)) {
            if (error_message != nullptr) {
                *error_message = lgbm_parse_error.empty() ? "lgbm_model_parse_failed" : lgbm_parse_error;
            }
            return false;
        }
        lgbm_model_sha256_loaded_ = lowerCopy(sha256HexOfFile(lgbm_model_path_resolved_));
        if (!lgbm_model_sha256_expected_.empty() && !lgbm_model_sha256_loaded_.empty() &&
            lgbm_model_sha256_loaded_ != lgbm_model_sha256_expected_) {
            if (error_message != nullptr) {
                *error_message = "lgbm_model_sha256_mismatch";
            }
            return false;
        }
        lgbm_model_loaded_ = true;
    }

    loaded_ = has_default_entry_ || !markets_.empty();
    if (loaded_) {
        const bool lgbm_backend = (prob_model_backend_ == "lgbm");
        const std::string calibration_mode = lgbm_backend ? "platt" : "off";
        LOG_INFO(
            "probabilistic_runtime_provenance "
            "prob_model_backend_effective={} "
            "lgbm_model_path={} "
            "lgbm_model_sha256={} "
            "calibration_mode={} "
            "calibration_a={} "
            "calibration_b={} "
            "ev_affine_enabled={} "
            "ev_affine_scale={} "
            "ev_affine_shift={}",
            prob_model_backend_,
            lgbm_backend ? lgbm_model_path_resolved_ : std::string(""),
            lgbm_backend ? lgbm_model_sha256_loaded_ : std::string(""),
            calibration_mode,
            lgbm_backend ? lgbm_h5_calib_a_ : 0.0,
            lgbm_backend ? lgbm_h5_calib_b_ : 0.0,
            lgbm_backend && lgbm_ev_affine_enabled_,
            lgbm_ev_affine_scale_,
            lgbm_ev_affine_shift_
        );
    }
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

    const double h1_raw_primary =
        clipProb(sigmoid(linearScore(entry->h1.coef, entry->h1.intercept, transformed_features)));
    double h5_raw_primary =
        clipProb(sigmoid(linearScore(entry->h5.coef, entry->h5.intercept, transformed_features)));
    const double h1_cal_primary = calibrateProb(h1_raw_primary, entry->h1.calib_a, entry->h1.calib_b);
    if (prob_model_backend_ == "lgbm") {
        if (!lgbm_model_loaded_) {
            return false;
        }
        h5_raw_primary = inferLightGbmProb(lgbm_model_, transformed_features);
    }
    const double h5_cal_primary =
        (prob_model_backend_ == "lgbm")
            ? calibrateProb(h5_raw_primary, lgbm_h5_calib_a_, lgbm_h5_calib_b_)
            : calibrateProb(h5_raw_primary, entry->h5.calib_a, entry->h5.calib_b);

    double h1_mean = h1_cal_primary;
    double h5_mean = h5_cal_primary;
    double h1_std = 0.0;
    double h5_std = 0.0;
    int ensemble_member_count = 1;
    if (prob_model_backend_ != "lgbm" && entry->ensemble_members.size() >= 2) {
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
    out.phase3_ev_calibration_enabled = phase3_policy_.phase3_ev_calibration_enabled;
    out.phase3_cost_tail_enabled = phase3_policy_.phase3_cost_tail_enabled;
    out.phase3_adaptive_ev_blend_enabled = phase3_policy_.phase3_adaptive_ev_blend_enabled;
    out.phase3_diagnostics_v2_enabled = phase3_policy_.phase3_diagnostics_v2_enabled;
    out.edge_semantics = runtime_semantics_state_.edge_semantics;
    out.root_cost_model_enabled_configured = runtime_semantics_state_.root_cost_model_enabled_configured;
    out.phase3_cost_model_enabled_configured = runtime_semantics_state_.phase3_cost_model_enabled_configured;
    out.root_cost_model_enabled_effective = runtime_semantics_state_.root_cost_model_enabled_effective;
    out.phase3_cost_model_enabled_effective = runtime_semantics_state_.phase3_cost_model_enabled_effective;
    out.edge_semantics_guard_violation = runtime_semantics_state_.guard_violation;
    out.edge_semantics_guard_forced_off = runtime_semantics_state_.guard_forced_off;
    out.edge_semantics_guard_action = runtime_semantics_state_.guard_action;
    out.phase4_portfolio_allocator_enabled = phase4_policy_.phase4_portfolio_allocator_enabled;
    out.phase4_correlation_control_enabled = phase4_policy_.phase4_correlation_control_enabled;
    out.phase4_risk_budget_enabled = phase4_policy_.phase4_risk_budget_enabled;
    out.phase4_drawdown_governor_enabled = phase4_policy_.phase4_drawdown_governor_enabled;
    out.phase4_execution_aware_sizing_enabled = phase4_policy_.phase4_execution_aware_sizing_enabled;
    out.phase4_portfolio_diagnostics_enabled = phase4_policy_.phase4_portfolio_diagnostics_enabled;

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
    double expected_edge_calibrated_raw_bps = calibration.calibrated_bps;
    if (!std::isfinite(expected_edge_calibrated_raw_bps)) {
        expected_edge_calibrated_raw_bps = expected_edge_raw_bps;
    }
    bool lgbm_ev_affine_applied = false;
    double expected_edge_calibrated_bps = expected_edge_calibrated_raw_bps;
    const bool lgbm_ev_affine_enabled_effective =
        (prob_model_backend_ == "lgbm") && lgbm_ev_affine_enabled_;
    if (lgbm_ev_affine_enabled_effective) {
        const double corrected =
            (expected_edge_calibrated_raw_bps * lgbm_ev_affine_scale_) + lgbm_ev_affine_shift_;
        if (std::isfinite(corrected)) {
            expected_edge_calibrated_bps = corrected;
            lgbm_ev_affine_applied = true;
        }
    }
    expected_edge_calibrated_bps = std::clamp(expected_edge_calibrated_bps, -5000.0, 5000.0);
    out.expected_edge_calibrated_raw_bps = expected_edge_calibrated_raw_bps;
    out.expected_edge_calibrated_bps = expected_edge_calibrated_bps;
    out.expected_edge_calibrated_corrected_bps = expected_edge_calibrated_bps;
    out.expected_edge_before_cost_bps = expected_edge_calibrated_bps;
    out.ev_calibration_applied = calibration.applied;
    out.ev_confidence = std::clamp(calibration.confidence, 0.0, 1.0);
    out.lgbm_ev_affine_enabled = lgbm_ev_affine_enabled_effective;
    out.lgbm_ev_affine_applied = lgbm_ev_affine_applied;
    out.lgbm_ev_affine_scale = lgbm_ev_affine_scale_;
    out.lgbm_ev_affine_shift = lgbm_ev_affine_shift_;

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
