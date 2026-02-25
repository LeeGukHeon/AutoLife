#pragma once

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace autolife {
namespace analytics {

struct ProbabilisticInference {
    double prob_h1_raw = 0.5;
    double prob_h1_calibrated = 0.5;
    double prob_h1_mean = 0.5;
    double prob_h1_std = 0.0;
    double prob_h5_raw = 0.5;
    double prob_h5_calibrated = 0.5;
    double prob_h5_mean = 0.5;
    double prob_h5_std = 0.0;
    int ensemble_member_count = 1;
    double selection_threshold_h5 = 0.6;
    double expected_edge_raw_bps = 0.0;
    double expected_edge_calibrated_bps = 0.0;
    double expected_edge_before_cost_bps = 0.0;
    double entry_cost_bps_estimate = 0.0;
    double exit_cost_bps_estimate = 0.0;
    double tail_cost_bps_estimate = 0.0;
    double cost_used_bps_estimate = 0.0;
    double cost_bps_estimate = 0.0;
    double expected_edge_bps = 0.0;
    double expected_edge_pct = 0.0;
    double ev_confidence = 1.0;
    bool edge_regressor_used = false;
    bool ev_calibration_applied = false;
    bool phase3_frontier_enabled = false;
    bool phase3_ev_calibration_enabled = false;
    bool phase3_cost_tail_enabled = false;
    bool phase3_adaptive_ev_blend_enabled = false;
    bool phase3_diagnostics_v2_enabled = false;
    std::string cost_mode = "mean_mode";
    bool select_h5 = false;
};

class ProbabilisticRuntimeModel {
public:
    struct Phase3FrontierPolicy {
        bool enabled = false;
        double k_margin = 0.0;
        double k_uncertainty = 0.0;
        double k_cost_tail = 0.0;
        double min_required_ev = -0.0002;
        double max_required_ev = 0.0050;
        double margin_floor = -1.0;
        double ev_confidence_floor = 0.0;
        double ev_confidence_penalty = 0.0;
        double cost_tail_penalty = 0.0;
        double cost_tail_reject_threshold_pct = 1.0;
    };

    struct Phase3EvCalibrationPolicy {
        bool enabled = false;
        bool use_quantile_map = false;
        int min_bucket_samples = 64;
        double default_confidence = 1.0;
        double min_confidence = 0.10;
        double ood_penalty = 0.10;
    };

    struct Phase3CostPolicy {
        bool enabled = false;
        std::string mode = "mean_mode";  // mean_mode | tail_mode | hybrid_mode
        double entry_multiplier = 0.50;
        double exit_multiplier = 0.50;
        double entry_add_bps = 0.0;
        double exit_add_bps = 0.0;
        double tail_markup_ratio = 0.35;
        double tail_add_bps = 0.0;
        double hybrid_lambda = 0.50;
    };

    struct Phase3AdaptiveEvBlendPolicy {
        bool enabled = false;
        double min = 0.05;
        double max = 0.40;
        double base = 0.20;
        double trend_bonus = 0.08;
        double ranging_penalty = 0.06;
        double hostile_penalty = 0.08;
        double high_confidence_bonus = 0.05;
        double low_confidence_penalty = 0.10;
        double cost_penalty = 0.06;
    };

    struct Phase3DiagnosticsPolicy {
        bool enabled = false;
    };

    struct Phase3Policy {
        bool phase3_frontier_enabled = false;
        bool phase3_ev_calibration_enabled = false;
        bool phase3_cost_tail_enabled = false;
        bool phase3_adaptive_ev_blend_enabled = false;
        bool phase3_diagnostics_v2_enabled = false;
        Phase3FrontierPolicy frontier;
        Phase3EvCalibrationPolicy ev_calibration;
        Phase3CostPolicy cost_model;
        Phase3AdaptiveEvBlendPolicy adaptive_ev_blend;
        Phase3DiagnosticsPolicy diagnostics_v2;
    };

    bool loadFromFile(const std::string& path, std::string* error_message = nullptr);
    bool isLoaded() const { return loaded_; }
    bool hasMarket(const std::string& market) const;
    bool supportsMarket(const std::string& market) const;
    bool hasDefaultModel() const { return has_default_entry_; }
    const std::vector<std::string>& featureColumns() const { return feature_columns_; }
    const Phase3Policy& phase3Policy() const { return phase3_policy_; }

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
        struct EnsembleMember {
            int member_index = 0;
            LinearHead h1;
            LinearHead h5;
        };
        int selected_fold_id = 0;
        LinearHead h1;
        LinearHead h5;
        std::vector<EnsembleMember> ensemble_members;
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

public:
    struct EvCalibrationBucket {
        std::string name = "default";
        std::string regime = "ANY";
        std::string volatility_bucket = "ANY";
        std::string liquidity_bucket = "ANY";
        double slope = 1.0;
        double intercept_bps = 0.0;
        std::vector<std::pair<double, double>> quantile_map;
        int sample_size = 0;
        double confidence = -1.0;
        double vol_bps_min = -1.0;
        double vol_bps_max = -1.0;
        double liquidity_ratio_min = -1.0;
        double liquidity_ratio_max = -1.0;
    };

private:
    std::unordered_map<std::string, MarketEntry> markets_;
    MarketEntry default_entry_;
    RuntimeCostModel cost_model_;
    Phase3Policy phase3_policy_;
    std::vector<EvCalibrationBucket> ev_calibration_buckets_;
    bool has_default_entry_ = false;
    bool prefer_default_entry_ = false;
    std::vector<std::string> feature_columns_;
    bool loaded_ = false;
};

} // namespace analytics
} // namespace autolife
