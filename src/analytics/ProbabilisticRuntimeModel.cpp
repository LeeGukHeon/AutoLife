#include "analytics/ProbabilisticRuntimeModel.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <numeric>

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

double estimateRuntimeCostBps(
    const ProbabilisticRuntimeModel::RuntimeCostModel& cfg,
    const std::vector<double>& transformed_features
) {
    if (!cfg.enabled) {
        return 0.0;
    }
    if (cfg.atr_pct_idx < 0 || cfg.bb_width_idx < 0 || cfg.vol_ratio_idx < 0 || cfg.notional_ratio_idx < 0) {
        return 0.0;
    }
    const auto idx_ok = [&transformed_features](int idx) {
        return idx >= 0 && static_cast<std::size_t>(idx) < transformed_features.size();
    };
    if (!idx_ok(cfg.atr_pct_idx) || !idx_ok(cfg.bb_width_idx) || !idx_ok(cfg.vol_ratio_idx) || !idx_ok(cfg.notional_ratio_idx)) {
        return 0.0;
    }

    // Runtime uses transformed features:
    // atr_pct/bb_width were multiplied by 100 in transform, ratios were log-transformed.
    const double atr_pct_raw = std::max(0.0, transformed_features[static_cast<std::size_t>(cfg.atr_pct_idx)] / 100.0);
    const double bb_width_raw = std::max(0.0, transformed_features[static_cast<std::size_t>(cfg.bb_width_idx)] / 100.0);
    const double vol_ratio = std::max(1e-9, std::exp(transformed_features[static_cast<std::size_t>(cfg.vol_ratio_idx)]));
    const double notional_ratio = std::max(1e-9, std::exp(transformed_features[static_cast<std::size_t>(cfg.notional_ratio_idx)]));

    const double atr_bps = atr_pct_raw * 10000.0;
    const double range_bps = bb_width_raw * 10000.0;
    const double vol_component = atr_bps / std::max(1e-9, cfg.volatility_norm_bps);
    const double range_component = range_bps / std::max(1e-9, cfg.range_norm_bps);
    const double liquidity_ratio = std::sqrt(std::max(1e-9, vol_ratio * notional_ratio));
    const double illiquidity = std::min(cfg.liquidity_penalty_cap, cfg.liquidity_ref_ratio / liquidity_ratio);

    const double cost_bps =
        cfg.fee_floor_bps +
        (cfg.volatility_weight * vol_component) +
        (cfg.range_weight * range_component) +
        (cfg.liquidity_weight * illiquidity);
    return std::clamp(cost_bps, cfg.fee_floor_bps, cfg.cost_cap_bps);
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
    std::string expected_pipeline;
    if (bundle_version == "probabilistic_runtime_bundle_v1") {
        expected_pipeline = "v1";
    } else if (bundle_version == "probabilistic_runtime_bundle_v2_draft") {
        expected_pipeline = "v2";
    } else {
        if (error_message != nullptr) {
            *error_message = "unsupported_bundle_version";
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

    if (expected_pipeline == "v2") {
        const std::string feature_contract_version =
            lowerCopy(root.value("feature_contract_version", std::string{}));
        const std::string runtime_contract_version =
            lowerCopy(root.value("runtime_bundle_contract_version", std::string{}));
        if (!feature_contract_version.empty() && feature_contract_version != "v2_draft") {
            if (error_message != nullptr) {
                *error_message = "unsupported_feature_contract_version";
            }
            return false;
        }
        if (!runtime_contract_version.empty() && runtime_contract_version != "v2_draft") {
            if (error_message != nullptr) {
                *error_message = "unsupported_runtime_bundle_contract_version";
            }
            return false;
        }
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
    const double fallback_expected_edge_bps =
        (h5_cal_primary * entry->h5.edge_win_mean_bps) +
        ((1.0 - h5_cal_primary) * entry->h5.edge_loss_mean_bps);
    double expected_edge_bps = fallback_expected_edge_bps;
    if (entry->h5.has_edge_regressor && entry->h5.edge_coef.size() == transformed_features.size()) {
        const double reg_edge = linearScore(entry->h5.edge_coef, entry->h5.edge_intercept, transformed_features);
        if (std::isfinite(reg_edge)) {
            expected_edge_bps = std::clamp(reg_edge, -entry->h5.edge_clip_abs_bps, entry->h5.edge_clip_abs_bps);
        }
    }
    out.expected_edge_before_cost_bps = expected_edge_bps;
    out.cost_bps_estimate = estimateRuntimeCostBps(cost_model_, transformed_features);
    out.expected_edge_bps = expected_edge_bps - out.cost_bps_estimate;
    out.expected_edge_pct = out.expected_edge_bps / 10000.0;
    out.select_h5 = (h5_cal_primary >= entry->h5.threshold);
    return true;
}

} // namespace analytics
} // namespace autolife
