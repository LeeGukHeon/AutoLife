#pragma once

#include <cstdint>
#include <limits>
#include <string>

namespace autolife::gate_vnext {

struct CandidateSnapshot {
    int source_index = -1;
    std::string market;
    long long ts_ms = 0;
    std::string regime = "UNKNOWN";

    double atr_pct_14 = 0.0;
    double adx = 0.0;

    double spread_pct = 0.0;
    double notional = 0.0;
    double volume_surge = 0.0;
    double imbalance = 0.0;
    double drop_vs_recent = 0.0;
    double drop_vs_signal = 0.0;

    double p_calibrated = 0.5;
    double selection_threshold = 0.5;
    double margin = 0.0;
    double tp_pct = 0.0;
    double sl_pct = 0.0;
    double label_cost_bps = 12.0;
    double expected_edge_calibrated_bps = 0.0;
    double expected_edge_used_for_gate_bps = 0.0;
    double edge_bps = 0.0;
    double expected_value_from_prob_bps = std::numeric_limits<double>::quiet_NaN();
    double ev_in_bps = std::numeric_limits<double>::quiet_NaN();
    double ev_for_size_bps = std::numeric_limits<double>::quiet_NaN();

    double expected_value_vnext_bps = 0.0;
    double size_fraction = 0.0;

    bool snapshot_valid = false;
    bool execution_gate_pass = false;
    std::string fail_reason;
    std::string execution_reject_reason;
};

}  // namespace autolife::gate_vnext
