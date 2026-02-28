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
    std::string edge_semantics = "net";
    bool root_cost_model_enabled_configured = false;
    bool phase3_cost_model_enabled_configured = false;
    bool root_cost_model_enabled_effective = false;
    bool phase3_cost_model_enabled_effective = false;
    bool edge_semantics_guard_violation = false;
    bool edge_semantics_guard_forced_off = false;
    std::string edge_semantics_guard_action = "none";
    double ev_confidence = 1.0;
    bool edge_regressor_used = false;
    bool ev_calibration_applied = false;
    bool phase3_frontier_enabled = false;
    bool phase3_ev_calibration_enabled = false;
    bool phase3_cost_tail_enabled = false;
    bool phase3_adaptive_ev_blend_enabled = false;
    bool phase3_diagnostics_v2_enabled = false;
    bool phase4_portfolio_allocator_enabled = false;
    bool phase4_correlation_control_enabled = false;
    bool phase4_risk_budget_enabled = false;
    bool phase4_drawdown_governor_enabled = false;
    bool phase4_execution_aware_sizing_enabled = false;
    bool phase4_portfolio_diagnostics_enabled = false;
    std::string cost_mode = "mean_mode";
    bool select_h5 = false;
};

class ProbabilisticRuntimeModel {
public:
    struct RuntimeSemanticsState {
        std::string edge_semantics = "net";
        bool root_cost_model_enabled_configured = false;
        bool phase3_cost_model_enabled_configured = false;
        bool root_cost_model_enabled_effective = false;
        bool phase3_cost_model_enabled_effective = false;
        bool guard_violation = false;
        bool guard_forced_off = false;
        std::string guard_action = "none";
    };

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
        double high_confidence_threshold = 0.72;
        double high_confidence_bonus = 0.05;
        double low_confidence_penalty = 0.10;
        double cost_penalty = 0.06;
    };

    struct Phase3OperationsControlPolicy {
        bool enabled = false;
        std::string mode = "manual";
        double required_ev_offset = 0.0;
        double required_ev_offset_trending_add = 0.0;
        double required_ev_offset_min = -0.0030;
        double required_ev_offset_max = 0.0030;
        double k_margin_scale = 1.0;
        double k_margin_scale_min = 0.50;
        double k_margin_scale_max = 2.00;
        double ev_blend_scale = 1.0;
        double ev_blend_scale_min = 0.50;
        double ev_blend_scale_max = 1.50;
        double max_step_per_update = 0.05;
        int min_update_interval_sec = 3600;
    };

    struct Phase3PrimaryMinimumPolicy {
        bool enabled = false;
        double min_h5_calibrated = 0.48;
        double min_h5_margin = -0.03;
        double min_liquidity_score = 42.0;
        double min_signal_strength = 0.34;
        double hostile_add_h5_calibrated = 0.06;
        double hostile_add_h5_margin = 0.03;
        double hostile_add_liquidity_score = 10.0;
        double hostile_add_signal_strength = 0.08;
        double ranging_add_h5_calibrated = 0.01;
        double ranging_add_h5_margin = 0.005;
        double ranging_add_liquidity_score = 2.0;
        double ranging_add_signal_strength = 0.02;
        double trending_up_add_h5_calibrated = -0.02;
        double trending_up_add_h5_margin = -0.01;
        double trending_up_add_liquidity_score = -5.0;
        double trending_up_add_signal_strength = -0.03;
        double regime_volatile_add_signal_strength = 0.03;
        double regime_hostile_add_liquidity_score = 8.0;
        double regime_hostile_add_signal_strength = 0.08;
        double clamp_h5_margin_min = -0.50;
        double clamp_h5_margin_max = 0.20;
        double clamp_liquidity_min = 0.0;
        double clamp_liquidity_max = 100.0;
        double clamp_strength_min = 0.0;
        double clamp_strength_max = 1.0;
    };

    struct Phase3PrimaryPriorityPolicy {
        bool enabled = false;
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

    struct Phase3PrimaryDecisionProfilePolicy {
        bool enabled = false;
        double confidence_prob_shift = 0.50;
        double confidence_prob_scale = 0.20;
        double confidence_margin_shift = 0.01;
        double confidence_margin_scale = 0.08;
        double confidence_prob_weight = 0.65;
        double confidence_margin_weight = 0.35;
        double target_strength_base = 0.22;
        double target_strength_prob_weight = 0.60;
        double target_strength_margin_weight = 1.80;
        double target_strength_min_hostile = 0.26;
        double target_strength_min_calm = 0.18;
        double target_strength_max = 0.98;
        double strength_blend_old_weight = 0.25;
        double strength_blend_target_weight = 0.75;
        double target_filter_base = 0.42;
        double target_filter_prob_weight = 0.40;
        double target_filter_margin_weight = 0.70;
        double target_filter_min = 0.20;
        double target_filter_max = 0.95;
        double filter_blend_old_weight = 0.30;
        double filter_blend_target_weight = 0.70;
        double implied_win_margin_weight = 0.45;
        double implied_win_threshold_gap_weight = 0.35;
        double implied_win_min_hostile = 0.44;
        double implied_win_min_calm = 0.40;
        double implied_win_max = 0.86;
        double probabilistic_edge_floor_margin_weight = 0.0012;
        double probabilistic_edge_floor_prob_weight = 0.0010;
        double probabilistic_edge_floor_prob_center = 0.50;
        double probabilistic_edge_floor_penalty_hostile = 0.00045;
        double probabilistic_edge_floor_penalty_calm = 0.00030;
        double current_risk_min = 0.0010;
        double current_risk_max = 0.0500;
        double target_risk_base_hostile = 0.0026;
        double target_risk_base_calm = 0.0028;
        double target_risk_confidence_scale_hostile = 0.0036;
        double target_risk_confidence_scale_calm = 0.0048;
        double fragility_target_risk_multiplier = 0.78;
        double fragility_negative_margin_threshold = -0.006;
        double fragility_negative_margin_target_risk_multiplier = 0.90;
        double target_risk_min_hostile = 0.0022;
        double target_risk_min_calm = 0.0024;
        double target_risk_max_hostile = 0.0075;
        double target_risk_max_calm = 0.0105;
        double blended_risk_old_weight = 0.35;
        double blended_risk_target_weight = 0.65;
        double blended_risk_min_hostile = 0.0022;
        double blended_risk_min_calm = 0.0024;
        double blended_risk_max_hostile = 0.0080;
        double blended_risk_max_calm = 0.0120;
        double rr_base_hostile = 1.10;
        double rr_base_calm = 1.20;
        double rr_confidence_weight_hostile = 1.00;
        double rr_confidence_weight_calm = 1.30;
        double rr_margin_positive_weight_hostile = 2.0;
        double rr_margin_positive_weight_calm = 2.8;
        double fragility_rr_bonus = 0.18;
        double rr_min_hostile = 1.05;
        double rr_min_calm = 1.10;
        double rr_max_hostile = 2.40;
        double rr_max_calm = 3.20;
        double tp1_rr_min = 1.0;
        double tp1_rr_multiplier = 0.55;
        double breakeven_mult_fragility = 0.48;
        double breakeven_mult_default = 0.70;
        double trailing_mult_fragility = 0.82;
        double trailing_mult_default = 1.10;
        double size_base = 0.42;
        double size_confidence_weight = 0.80;
        double size_margin_positive_weight = 1.20;
        double size_negative_margin_multiplier = 0.86;
        double size_hostile_multiplier = 0.88;
        double size_min = 0.30;
        double size_max = 1.35;
        double score_margin_boost_cap = 0.12;
        double filter_margin_boost_scale = 0.10;
        double edge_clip_abs_pct = 0.05;
        double primary_nudge_base = 0.20;
        double primary_nudge_blend_scale = 0.25;
        double primary_filter_nudge_base = 0.10;
        double primary_filter_nudge_blend_scale = 0.10;
    };

    struct Phase3ManagerFilterPolicy {
        bool enabled = false;
        double base_min_strength_default = 0.40;
        double base_min_strength_ranging = 0.43;
        double base_min_strength_high_volatility = 0.48;
        double base_min_strength_trending_down = 0.52;
        double base_min_expected_value = 0.0;
        double margin_min_ranging = 0.0;
        std::string margin_min_ranging_mode = "enforce";
        double scan_prefilter_margin_add_hostile = 0.015;
        double scan_prefilter_margin_add_trending_up = -0.005;
        double scan_prefilter_margin_clamp_min = -0.30;
        double scan_prefilter_margin_clamp_max = 0.15;
        double scan_prefilter_margin_with_regime_clamp_min = -0.30;
        double scan_prefilter_margin_with_regime_clamp_max = 0.30;
        double history_gate_min_win_rate_base = 0.50;
        double history_gate_min_profit_factor_base = 1.10;
        int history_gate_min_sample_trades_base = 16;
        double history_gate_win_rate_add_trending_down = 0.03;
        double history_gate_profit_factor_add_trending_down = 0.05;
        double history_gate_win_rate_add_high_volatility = 0.02;
        double history_gate_profit_factor_add_high_volatility = 0.04;

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
        double probabilistic_confidence_prob_shift = 0.50;
        double probabilistic_confidence_prob_scale = 0.25;
        double probabilistic_confidence_margin_shift = 0.02;
        double probabilistic_confidence_margin_scale = 0.12;
        double probabilistic_confidence_prob_weight = 0.40;
        double probabilistic_confidence_margin_weight = 0.60;
        double probabilistic_high_confidence_threshold = 0.65;

        int history_min_sample_hostile = 18;
        int history_min_sample_calm = 36;
        double history_severe_win_rate_shortfall = 0.08;
        double history_severe_profit_factor_shortfall = 0.30;
        int history_relief_max_trade_count = 52;
        double history_relief_min_h5_calibrated = 0.48;
        double history_relief_min_h5_margin = -0.012;
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
        double frontier_uncertainty_prob_weight = 0.60;
        double frontier_uncertainty_ev_weight = 0.40;
    };

    struct Phase3DiagnosticsPolicy {
        bool enabled = false;
    };

    struct Phase3LiquidityVolumeGatePolicy {
        bool enabled = false;
        std::string mode = "legacy_fixed";
        int window_minutes = 60;
        double quantile_q = 0.20;
        int min_samples_required = 30;
        std::string low_conf_action = "hold";
    };
    struct Phase3FoundationStructureGatePolicy {
        bool enabled = false;
        std::string mode = "legacy_fixed";
        double relax_delta = 0.0;
    };
    struct Phase3BearReboundGuardPolicy {
        bool enabled = false;
        std::string mode = "legacy_fixed";
        int window_minutes = 60;
        double quantile_q = 0.20;
        int min_samples_required = 30;
        std::string low_conf_action = "hold";
        double static_threshold = 1.0;
    };
    struct Phase3ExitPolicy {
        std::string strategy_exit_mode = "enforce";
    };

    struct Phase3Policy {
        bool phase3_frontier_enabled = false;
        bool phase3_ev_calibration_enabled = false;
        bool phase3_cost_tail_enabled = false;
        bool phase3_adaptive_ev_blend_enabled = false;
        bool phase3_diagnostics_v2_enabled = false;
        bool regime_entry_disable_enabled = false;
        bool disable_ranging_entry = false;
        std::unordered_map<std::string, bool> regime_entry_disable;
        Phase3FrontierPolicy frontier;
        Phase3EvCalibrationPolicy ev_calibration;
        Phase3CostPolicy cost_model;
        Phase3AdaptiveEvBlendPolicy adaptive_ev_blend;
        Phase3OperationsControlPolicy operations_control;
        Phase3PrimaryMinimumPolicy primary_minimums;
        Phase3PrimaryPriorityPolicy primary_priority;
        Phase3PrimaryDecisionProfilePolicy primary_decision_profile;
        Phase3ManagerFilterPolicy manager_filter;
        Phase3DiagnosticsPolicy diagnostics_v2;
        Phase3LiquidityVolumeGatePolicy liq_vol_gate;
        Phase3FoundationStructureGatePolicy foundation_structure_gate;
        Phase3BearReboundGuardPolicy bear_rebound_guard;
        Phase3ExitPolicy exit;
    };

    struct Phase4Policy {
        struct PortfolioAllocatorPolicy {
            bool enabled = false;
            int top_k = 1;
            double min_score = -1.0e6;
            double lambda_tail = 1.0;
            double lambda_cost = 1.0;
            double lambda_uncertainty = 1.0;
            double lambda_margin = 1.0;
            double uncertainty_prob_weight = 0.50;
            double uncertainty_ev_weight = 0.50;
        };
        struct RiskBudgetPolicy {
            bool enabled = false;
            double per_market_cap = 1.0;
            double gross_cap = 1.0;
            double risk_budget_cap = 1.0;
            double risk_proxy_stop_pct = 0.03;
            std::unordered_map<std::string, double> regime_budget_multipliers;
        };
        struct DrawdownGovernorPolicy {
            bool enabled = false;
            double dd_threshold_soft = 0.05;
            double dd_threshold_hard = 0.10;
            double budget_multiplier_soft = 0.70;
            double budget_multiplier_hard = 0.40;
        };
        struct CorrelationControlPolicy {
            bool enabled = false;
            double default_cluster_cap = 1.0;
            std::unordered_map<std::string, std::string> market_cluster_map;
            std::unordered_map<std::string, double> cluster_caps;
            // Optional soft-constraint penalty applied before cluster-cap hard filter.
            double penalty_weight = 0.0;
            double penalty_utilization_trigger = 0.85;
            double penalty_reject_threshold = 1.0e9;
            // If true, allocator skips cap-violating ranked candidates and refills from next ranked items.
            bool cluster_cap_reallocate_in_allocator = false;
        };
        struct ExecutionAwareSizingPolicy {
            bool enabled = false;
            double liquidity_low_threshold = 40.0;
            double liquidity_mid_threshold = 65.0;
            double liquidity_low_size_multiplier = 0.50;
            double liquidity_mid_size_multiplier = 0.75;
            double liquidity_high_size_multiplier = 1.00;
            double tail_cost_soft_pct = 0.0015;
            double tail_cost_hard_pct = 0.0030;
            double tail_soft_multiplier = 0.80;
            double tail_hard_multiplier = 0.60;
            double min_position_size = 0.01;
        };

        bool phase4_portfolio_allocator_enabled = false;
        bool phase4_correlation_control_enabled = false;
        bool phase4_risk_budget_enabled = false;
        bool phase4_drawdown_governor_enabled = false;
        bool phase4_execution_aware_sizing_enabled = false;
        bool phase4_portfolio_diagnostics_enabled = false;
        PortfolioAllocatorPolicy portfolio_allocator;
        RiskBudgetPolicy risk_budget;
        DrawdownGovernorPolicy drawdown_governor;
        CorrelationControlPolicy correlation_control;
        ExecutionAwareSizingPolicy execution_aware_sizing;
    };

    bool loadFromFile(const std::string& path, std::string* error_message = nullptr);
    bool isLoaded() const { return loaded_; }
    bool hasMarket(const std::string& market) const;
    bool supportsMarket(const std::string& market) const;
    bool hasDefaultModel() const { return has_default_entry_; }
    const std::vector<std::string>& featureColumns() const { return feature_columns_; }
    const Phase3Policy& phase3Policy() const { return phase3_policy_; }
    const Phase4Policy& phase4Policy() const { return phase4_policy_; }
    const RuntimeSemanticsState& runtimeSemanticsState() const { return runtime_semantics_state_; }

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
    Phase4Policy phase4_policy_;
    RuntimeSemanticsState runtime_semantics_state_;
    std::vector<EvCalibrationBucket> ev_calibration_buckets_;
    bool has_default_entry_ = false;
    bool prefer_default_entry_ = false;
    std::vector<std::string> feature_columns_;
    bool loaded_ = false;
};

} // namespace analytics
} // namespace autolife
