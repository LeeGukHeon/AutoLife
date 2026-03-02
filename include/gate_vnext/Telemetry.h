#pragma once

#include <map>
#include <string>

namespace autolife::gate_vnext {

struct StageFunnel {
    long long s0_snapshots_valid = 0;
    long long s1_selected_topk = 0;
    long long s2_sized_count = 0;
    long long s3_exec_gate_pass = 0;
    long long s4_submitted = 0;
    long long s5_filled = 0;
};

struct Provenance {
    std::string config_path;
    std::string run_dir;
    std::string backend_request;
    std::string backend_effective;
    std::string model_sha256;
};

struct QualityStats {
    int topk_effective = 5;
    double margin_mean = 0.0;
    double margin_std = 0.0;
    double margin_p10 = 0.0;
    double margin_p50 = 0.0;
    double margin_p90 = 0.0;
};

struct SizingStats {
    long long ev_negative_size_zero_count = 0;
    long long ev_positive_size_gt_zero_count = 0;
    double expected_value_from_prob_min_bps = 0.0;
    double expected_value_from_prob_median_bps = 0.0;
    double expected_value_from_prob_max_bps = 0.0;
    double p_cal_min = 0.0;
    double p_cal_median = 0.0;
    double p_cal_max = 0.0;
    double tp_pct_min = 0.0;
    double tp_pct_median = 0.0;
    double tp_pct_max = 0.0;
    double sl_pct_min = 0.0;
    double sl_pct_median = 0.0;
    double sl_pct_max = 0.0;
    double ev_in_min_bps = 0.0;
    double ev_in_median_bps = 0.0;
    double ev_in_max_bps = 0.0;
    double ev_for_size_min_bps = 0.0;
    double ev_for_size_median_bps = 0.0;
    double ev_for_size_max_bps = 0.0;
    double size_fraction_min = 0.0;
    double size_fraction_median = 0.0;
    double size_fraction_max = 0.0;
};

struct GateVNextTelemetry {
    StageFunnel funnel{};
    Provenance provenance{};
    QualityStats quality{};
    SizingStats sizing{};
    std::map<std::string, long long> veto_reason_counts;
};

}  // namespace autolife::gate_vnext
