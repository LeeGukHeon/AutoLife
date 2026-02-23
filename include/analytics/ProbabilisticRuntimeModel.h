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
    double expected_edge_before_cost_bps = 0.0;
    double cost_bps_estimate = 0.0;
    double expected_edge_bps = 0.0;
    double expected_edge_pct = 0.0;
    bool select_h5 = false;
};

class ProbabilisticRuntimeModel {
public:
    bool loadFromFile(const std::string& path, std::string* error_message = nullptr);
    bool isLoaded() const { return loaded_; }
    bool hasMarket(const std::string& market) const;
    bool supportsMarket(const std::string& market) const;
    bool hasDefaultModel() const { return has_default_entry_; }
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
        bool has_edge_regressor = false;
        std::vector<double> edge_coef;
        double edge_intercept = 0.0;
        double edge_clip_abs_bps = 250.0;
        double edge_win_mean_bps = 0.0;
        double edge_loss_mean_bps = 0.0;
        double edge_neutral_mean_bps = 0.0;
    };

    struct MarketEntry {
        int selected_fold_id = 0;
        LinearHead h1;
        LinearHead h5;
    };

public:
    struct RuntimeCostModel {
        bool enabled = false;
        double fee_floor_bps = 6.0;
        double volatility_weight = 3.0;
        double range_weight = 1.5;
        double liquidity_weight = 2.5;
        double volatility_norm_bps = 50.0;
        double range_norm_bps = 80.0;
        double liquidity_ref_ratio = 1.0;
        double liquidity_penalty_cap = 8.0;
        double cost_cap_bps = 200.0;
        int atr_pct_idx = -1;
        int bb_width_idx = -1;
        int vol_ratio_idx = -1;
        int notional_ratio_idx = -1;
    };

private:
    std::unordered_map<std::string, MarketEntry> markets_;
    MarketEntry default_entry_;
    RuntimeCostModel cost_model_;
    bool has_default_entry_ = false;
    bool prefer_default_entry_ = false;
    std::vector<std::string> feature_columns_;
    bool loaded_ = false;
};

} // namespace analytics
} // namespace autolife
