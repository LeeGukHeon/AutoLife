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

struct GateVNextTelemetry {
    StageFunnel funnel{};
    Provenance provenance{};
    QualityStats quality{};
    std::map<std::string, long long> veto_reason_counts;
};

}  // namespace autolife::gate_vnext
