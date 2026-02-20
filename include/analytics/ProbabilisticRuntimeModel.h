#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace autolife {
namespace analytics {

struct ProbabilisticInference {
    double prob_h1_raw = 0.5;
    double prob_h1_calibrated = 0.5;
    double prob_h5_raw = 0.5;
    double prob_h5_calibrated = 0.5;
    double selection_threshold_h5 = 0.6;
    bool select_h5 = false;
};

class ProbabilisticRuntimeModel {
public:
    bool loadFromFile(const std::string& path, std::string* error_message = nullptr);
    bool isLoaded() const { return loaded_; }
    bool hasMarket(const std::string& market) const;
    const std::vector<std::string>& featureColumns() const { return feature_columns_; }

    // `transformed_features` must already match the runtime bundle transform contract.
    bool infer(
        const std::string& market,
        const std::vector<double>& transformed_features,
        ProbabilisticInference& out
    ) const;
    // Internal bundle-parsed heads; kept public for translation-unit helper parsing.
    struct LinearHead {
        std::vector<double> coef;
        double intercept = 0.0;
        double calib_a = 1.0;
        double calib_b = 0.0;
        double threshold = 0.6;
    };

    struct MarketEntry {
        int selected_fold_id = 0;
        LinearHead h1;
        LinearHead h5;
    };

private:
    std::unordered_map<std::string, MarketEntry> markets_;
    std::vector<std::string> feature_columns_;
    bool loaded_ = false;
};

} // namespace analytics
} // namespace autolife
