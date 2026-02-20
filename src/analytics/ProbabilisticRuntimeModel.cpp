#include "analytics/ProbabilisticRuntimeModel.h"

#include <algorithm>
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
    }
    return !out.coef.empty();
}

} // namespace

bool ProbabilisticRuntimeModel::loadFromFile(const std::string& path, std::string* error_message) {
    loaded_ = false;
    markets_.clear();
    feature_columns_.clear();

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

    const auto markets = root.value("markets", nlohmann::json::array());
    if (!markets.is_array() || markets.empty()) {
        if (error_message != nullptr) {
            *error_message = "markets_empty";
        }
        return false;
    }

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
        markets_.insert_or_assign(market, std::move(entry));
    }

    loaded_ = !markets_.empty();
    if (!loaded_ && error_message != nullptr) {
        *error_message = "no_valid_market_entries";
    }
    return loaded_;
}

bool ProbabilisticRuntimeModel::hasMarket(const std::string& market) const {
    return markets_.find(market) != markets_.end();
}

bool ProbabilisticRuntimeModel::infer(
    const std::string& market,
    const std::vector<double>& transformed_features,
    ProbabilisticInference& out
) const {
    const auto it = markets_.find(market);
    if (it == markets_.end()) {
        return false;
    }
    const auto& entry = it->second;
    if (transformed_features.size() != feature_columns_.size()) {
        return false;
    }

    const double h1_raw = clipProb(sigmoid(linearScore(entry.h1.coef, entry.h1.intercept, transformed_features)));
    const double h5_raw = clipProb(sigmoid(linearScore(entry.h5.coef, entry.h5.intercept, transformed_features)));
    const double h1_cal = calibrateProb(h1_raw, entry.h1.calib_a, entry.h1.calib_b);
    const double h5_cal = calibrateProb(h5_raw, entry.h5.calib_a, entry.h5.calib_b);

    out.prob_h1_raw = h1_raw;
    out.prob_h1_calibrated = h1_cal;
    out.prob_h5_raw = h5_raw;
    out.prob_h5_calibrated = h5_cal;
    out.selection_threshold_h5 = entry.h5.threshold;
    out.select_h5 = (h5_cal >= entry.h5.threshold);
    return true;
}

} // namespace analytics
} // namespace autolife

