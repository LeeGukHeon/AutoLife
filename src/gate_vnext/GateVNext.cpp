#include "gate_vnext/GateVNext.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace autolife::gate_vnext {

namespace {

double computeMean(const std::vector<double>& values) {
    if (values.empty()) {
        return 0.0;
    }
    double sum = 0.0;
    for (double v : values) {
        sum += v;
    }
    return sum / static_cast<double>(values.size());
}

double computeStd(const std::vector<double>& values, double mean) {
    if (values.size() <= 1) {
        return 0.0;
    }
    double sq = 0.0;
    for (double v : values) {
        const double d = v - mean;
        sq += d * d;
    }
    return std::sqrt(sq / static_cast<double>(values.size()));
}

double computeQuantile(std::vector<double> values, double q) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const double clamped = std::clamp(q, 0.0, 1.0);
    const std::size_t idx = static_cast<std::size_t>(
        std::llround(clamped * static_cast<double>(values.size() - 1))
    );
    return values[std::min(idx, values.size() - 1)];
}

}  // namespace

std::vector<CandidateSnapshot> QualitySelector::selectTopKByMargin(
    const std::vector<CandidateSnapshot>& snapshots
) const {
    std::vector<CandidateSnapshot> valid;
    valid.reserve(snapshots.size());
    for (const auto& s : snapshots) {
        if (!s.snapshot_valid) {
            continue;
        }
        valid.push_back(s);
    }

    std::stable_sort(
        valid.begin(),
        valid.end(),
        [](const CandidateSnapshot& a, const CandidateSnapshot& b) {
            return a.margin > b.margin;
        }
    );

    const std::size_t keep = static_cast<std::size_t>(std::max(topk_, 0));
    if (valid.size() > keep) {
        valid.resize(keep);
    }
    return valid;
}

std::vector<CandidateSnapshot> RiskSizer::applySizing(
    const std::vector<CandidateSnapshot>& selected
) const {
    std::vector<CandidateSnapshot> out = selected;
    for (auto& s : out) {
        s.expected_value_vnext_bps = s.edge_bps;
        if (s.expected_value_vnext_bps < 0.0) {
            s.size_fraction = 0.0;
            continue;
        }
        const double scale = std::max(ev_scale_bps_, 1e-9);
        const double raw = s.expected_value_vnext_bps / scale;
        const double clamped = std::clamp(raw, 0.0, 1.0);
        s.size_fraction = base_size_ * clamped;
    }
    return out;
}

std::vector<CandidateSnapshot> ExecutionGate::filterExecutable(
    const std::vector<CandidateSnapshot>& sized
) const {
    std::vector<CandidateSnapshot> out;
    out.reserve(sized.size());
    for (auto s : sized) {
        if (s.size_fraction <= 0.0) {
            s.execution_gate_pass = false;
            s.execution_reject_reason = "size_zero";
            out.push_back(std::move(s));
            continue;
        }
        s.execution_gate_pass = true;
        s.execution_reject_reason.clear();
        out.push_back(std::move(s));
    }
    return out;
}

GateVNext::GateVNext(Params params)
    : params_(params),
      selector_(params.quality_topk),
      sizer_(params.base_size, params.ev_scale_bps) {}

std::vector<CandidateSnapshot> GateVNext::run(
    const std::vector<CandidateSnapshot>& snapshots,
    GateVNextTelemetry* telemetry
) const {
    const auto selected = selector_.selectTopKByMargin(snapshots);
    const auto sized = sizer_.applySizing(selected);
    auto gated = execution_gate_.filterExecutable(sized);

    if (telemetry != nullptr) {
        telemetry->funnel.s0_snapshots_valid = 0;
        for (const auto& s : snapshots) {
            if (s.snapshot_valid) {
                telemetry->funnel.s0_snapshots_valid++;
            }
        }
        telemetry->funnel.s1_selected_topk = static_cast<long long>(selected.size());
        telemetry->funnel.s2_sized_count = 0;
        telemetry->funnel.s3_exec_gate_pass = 0;
        for (const auto& s : gated) {
            if (s.size_fraction > 0.0) {
                telemetry->funnel.s2_sized_count++;
            }
            if (s.execution_gate_pass) {
                telemetry->funnel.s3_exec_gate_pass++;
            } else if (!s.execution_reject_reason.empty()) {
                telemetry->veto_reason_counts[s.execution_reject_reason]++;
            }
        }
        telemetry->quality.topk_effective = selector_.topk();
        std::vector<double> margins;
        margins.reserve(selected.size());
        for (const auto& s : selected) {
            if (std::isfinite(s.margin)) {
                margins.push_back(s.margin);
            }
        }
        telemetry->quality.margin_mean = computeMean(margins);
        telemetry->quality.margin_std = computeStd(margins, telemetry->quality.margin_mean);
        telemetry->quality.margin_p10 = computeQuantile(margins, 0.10);
        telemetry->quality.margin_p50 = computeQuantile(margins, 0.50);
        telemetry->quality.margin_p90 = computeQuantile(margins, 0.90);
    }

    return gated;
}

}  // namespace autolife::gate_vnext
