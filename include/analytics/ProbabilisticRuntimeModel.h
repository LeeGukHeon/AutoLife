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

    struct Phase3PrimaryMinimumPolicy {
        bool enabled = false;
        double min_h5_calibrated = 0.48;
        double min_h5_margin = -0.03;
        double min_liquidity_score = 42.0;
        double min_signal_strength = 0.34;
    };

    struct Phase3PrimaryPriorityPolicy {
        bool enabled = false;
        double conf_prob_shift = 0.50;
        double conf_prob_scale = 0.25;
        double conf_margin_shift = 0.02;
        double conf_margin_scale = 0.12;
        double conf_prob_weight = 0.65;
        double conf_margin_weight = 0.35;
        double margin_score_shift = 0.10;
        double margin_score_scale = 0.20;
        double edge_score_shift = 0.0005;
        double edge_score_scale = 0.0025;
        double prob_weight = 0.50;
        double margin_weight = 0.22;
        double liquidity_weight = 0.10;
        double strength_weight = 0.10;
        double edge_weight = 0.08;
        double hostile_prob_weight = 0.54;
        double hostile_margin_weight = 0.22;
        double hostile_liquidity_weight = 0.11;
        double hostile_strength_weight = 0.09;
        double hostile_edge_weight = 0.04;
        double strong_buy_bonus = 0.02;
        double margin_bonus_scale = 0.08;
        double margin_bonus_cap = 0.03;
        double rescue_penalty = 0.16;
        double rescue_bonus = 0.02;
        double rescue_confidence_floor = 0.72;
        double rescue_strength_floor = 0.46;
        double rescue_margin_floor = 0.002;
        double range_penalty = 0.11;
        double range_bonus = 0.03;
        double range_penalty_strength_floor = 0.50;
        double range_penalty_margin_floor = 0.008;
        double range_penalty_prob_floor = 0.54;
        double range_bonus_margin_floor = 0.012;
        double range_bonus_prob_floor = 0.57;
        double uptrend_bonus = 0.03;
        double uptrend_bonus_margin_floor = 0.0;
        double uptrend_bonus_prob_floor = 0.52;
    };

    struct Phase3ManagerFilterPolicy {
        bool enabled = false;
        double base_min_strength_default = 0.40;
        double base_min_strength_ranging = 0.43;
        double base_min_strength_high_volatility = 0.48;
        double base_min_strength_trending_down = 0.52;
        double base_min_expected_value = 0.0;

        double hostile_strength_add_scale = 0.18;
        double hostile_strength_add_cap = 0.08;
        double hostile_ev_add_scale = 0.0008;
        double hostile_ev_add_cap = 0.00035;
        double hostile_pause_min_strength = 0.96;
        double hostile_pause_min_expected_value = 0.0040;

        double min_strength_floor = 0.35;
        double min_strength_cap = 0.98;
        double min_expected_value_floor = -0.0002;
        double min_expected_value_cap = 0.0050;

        double no_snapshot_min_strength_hostile = 0.36;
        double no_snapshot_min_strength_calm = 0.28;
        double no_snapshot_min_expected_value_hostile = 0.00002;
        double no_snapshot_min_expected_value_calm = -0.00030;

        double confidence_prob_shift = 0.50;
        double confidence_prob_scale = 0.20;
        double confidence_margin_shift = 0.01;
        double confidence_margin_scale = 0.08;
        double confidence_prob_weight = 0.65;
        double confidence_margin_weight = 0.35;

        double target_strength_hostile_base = 0.34;
        double target_strength_hostile_confidence_scale = 0.08;
        double target_strength_calm_base = 0.24;
        double target_strength_calm_confidence_scale = 0.14;
        double target_expected_value_hostile_base = 0.00002;
        double target_expected_value_hostile_confidence_scale = 0.00008;
        double target_expected_value_calm_base = -0.00035;
        double target_expected_value_calm_confidence_scale = 0.00035;

        double negative_margin_strength_add_hostile = 0.03;
        double negative_margin_strength_add_calm = 0.02;
        double negative_margin_expected_value_add_hostile = 0.00005;
        double negative_margin_expected_value_add_calm = 0.00010;

        double target_strength_hostile_min = 0.26;
        double target_strength_hostile_max = 0.38;
        double target_strength_calm_min = 0.12;
        double target_strength_calm_max = 0.24;
        double target_expected_value_hostile_min = -0.00010;
        double target_expected_value_hostile_max = 0.00008;
        double target_expected_value_calm_min = -0.00080;
        double target_expected_value_calm_max = -0.00020;

        double required_strength_cap = 0.95;
        double core_signal_ownership_strength_relief = 0.02;
        double core_signal_ownership_expected_value_floor = -0.00005;
        double policy_hold_strength_add = 0.05;
        double policy_hold_expected_value_add_core = 0.00010;
        double policy_hold_expected_value_add_other = 0.00018;
        double off_trend_strength_add = 0.06;
        double off_trend_expected_value_add_core = 0.00009;
        double off_trend_expected_value_add_other = 0.00015;
        double hostile_regime_strength_add = 0.03;
        double hostile_regime_expected_value_add_core = 0.00005;
        double hostile_regime_expected_value_add_other = 0.00008;

        double probabilistic_confidence_strength_relief_scale = 0.03;
        double probabilistic_confidence_expected_value_relief_scale = 0.00010;

        int history_min_sample_hostile = 18;
        int history_min_sample_calm = 36;
        double history_guard_scale_base = 0.45;
        double history_guard_scale_confidence_scale = 0.35;
        double history_guard_scale_min_hostile = 0.18;
        double history_guard_scale_min_calm = 0.10;
        double history_guard_scale_max_hostile = 0.60;
        double history_guard_scale_max_calm = 0.45;
        double history_strength_bump_prob = 0.012;
        double history_strength_bump_non_prob = 0.05;
        double history_edge_bump_core_prob = 0.00002;
        double history_edge_bump_core_non_prob = 0.00005;
        double history_edge_bump_other_prob = 0.00003;
        double history_edge_bump_other_non_prob = 0.00010;

        double rr_guard_floor_hostile = 1.12;
        double rr_guard_floor_calm = 1.08;
        double rr_guard_skip_min_rr = 0.95;
        double rr_guard_scale_base = 0.90;
        double rr_guard_scale_confidence_scale = 0.60;
        double rr_guard_scale_min = 0.20;
        double rr_guard_scale_max = 0.90;
        double rr_guard_strength_add = 0.03;
        double rr_guard_expected_value_add_core = 0.00003;
        double rr_guard_expected_value_add_other = 0.00006;
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
        Phase3PrimaryMinimumPolicy primary_minimums;
        Phase3PrimaryPriorityPolicy primary_priority;
        Phase3ManagerFilterPolicy manager_filter;
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
