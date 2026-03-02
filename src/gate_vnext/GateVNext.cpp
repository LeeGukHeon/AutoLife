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
        if (!std::isfinite(s.p_calibrated) || !std::isfinite(s.tp_pct) ||
            !std::isfinite(s.sl_pct) || s.tp_pct <= 0.0 || s.sl_pct <= 0.0) {
            s.expected_value_from_prob_bps = std::numeric_limits<double>::quiet_NaN();
            s.ev_in_bps = std::numeric_limits<double>::quiet_NaN();
            s.ev_for_size_bps = std::numeric_limits<double>::quiet_NaN();
            s.expected_value_vnext_bps = std::numeric_limits<double>::quiet_NaN();
            s.size_fraction = 0.0;
            continue;
        }
        // Gate vNext v2:
        // ev_in_bps is sourced from p-calibrated EV, not EDGE_REGRESSOR.
        const double implied_win = std::clamp(s.p_calibrated, 0.0, 1.0);
        const double label_cost_bps = std::max(0.0, s.label_cost_bps);
        const double expected_value_from_prob_bps =
            (10000.0 * ((implied_win * s.tp_pct) - ((1.0 - implied_win) * s.sl_pct))) -
            label_cost_bps;
        if (!std::isfinite(expected_value_from_prob_bps)) {
            s.expected_value_from_prob_bps = std::numeric_limits<double>::quiet_NaN();
            s.ev_in_bps = std::numeric_limits<double>::quiet_NaN();
            s.ev_for_size_bps = std::numeric_limits<double>::quiet_NaN();
            s.expected_value_vnext_bps = std::numeric_limits<double>::quiet_NaN();
            s.size_fraction = 0.0;
            continue;
        }
        s.expected_value_from_prob_bps = expected_value_from_prob_bps;
        s.ev_in_bps = expected_value_from_prob_bps;
        s.expected_value_vnext_bps = expected_value_from_prob_bps;
        s.ev_for_size_bps = std::max(expected_value_from_prob_bps, 0.0);

        const double scale = std::max(ev_scale_bps_, 1e-9);
        const double raw = s.ev_for_size_bps / scale;
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
            if (!std::isfinite(s.expected_value_vnext_bps)) {
                s.execution_reject_reason = "missing_ev_source";
            } else if (s.expected_value_vnext_bps < 0.0) {
                s.execution_reject_reason = "ev_negative_size_zero";
            } else {
                s.execution_reject_reason = "size_zero";
            }
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
        std::vector<double> ev_prob_values;
        std::vector<double> p_cal_values;
        std::vector<double> tp_values;
        std::vector<double> sl_values;
        std::vector<double> ev_in_values;
        std::vector<double> ev_for_size_values;
        std::vector<double> size_values;
        ev_prob_values.reserve(gated.size());
        p_cal_values.reserve(gated.size());
        tp_values.reserve(gated.size());
        sl_values.reserve(gated.size());
        ev_in_values.reserve(gated.size());
        ev_for_size_values.reserve(gated.size());
        size_values.reserve(gated.size());
        for (const auto& s : gated) {
            if (std::isfinite(s.expected_value_from_prob_bps)) {
                ev_prob_values.push_back(s.expected_value_from_prob_bps);
            }
            if (std::isfinite(s.p_calibrated)) {
                p_cal_values.push_back(s.p_calibrated);
            }
            if (std::isfinite(s.tp_pct)) {
                tp_values.push_back(s.tp_pct);
            }
            if (std::isfinite(s.sl_pct)) {
                sl_values.push_back(s.sl_pct);
            }
            if (std::isfinite(s.ev_in_bps)) {
                ev_in_values.push_back(s.ev_in_bps);
            }
            if (std::isfinite(s.ev_for_size_bps)) {
                ev_for_size_values.push_back(s.ev_for_size_bps);
            }
            if (std::isfinite(s.size_fraction)) {
                size_values.push_back(s.size_fraction);
            }
            if (std::isfinite(s.ev_in_bps) && s.ev_in_bps < 0.0) {
                telemetry->sizing.ev_negative_size_zero_count++;
            }
            if (s.size_fraction > 0.0) {
                telemetry->funnel.s2_sized_count++;
                telemetry->sizing.ev_positive_size_gt_zero_count++;
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
        telemetry->sizing.expected_value_from_prob_min_bps = computeQuantile(ev_prob_values, 0.00);
        telemetry->sizing.expected_value_from_prob_median_bps = computeQuantile(ev_prob_values, 0.50);
        telemetry->sizing.expected_value_from_prob_max_bps = computeQuantile(ev_prob_values, 1.00);
        telemetry->sizing.p_cal_min = computeQuantile(p_cal_values, 0.00);
        telemetry->sizing.p_cal_median = computeQuantile(p_cal_values, 0.50);
        telemetry->sizing.p_cal_max = computeQuantile(p_cal_values, 1.00);
        telemetry->sizing.tp_pct_min = computeQuantile(tp_values, 0.00);
        telemetry->sizing.tp_pct_median = computeQuantile(tp_values, 0.50);
        telemetry->sizing.tp_pct_max = computeQuantile(tp_values, 1.00);
        telemetry->sizing.sl_pct_min = computeQuantile(sl_values, 0.00);
        telemetry->sizing.sl_pct_median = computeQuantile(sl_values, 0.50);
        telemetry->sizing.sl_pct_max = computeQuantile(sl_values, 1.00);
        telemetry->sizing.ev_in_min_bps = computeQuantile(ev_in_values, 0.00);
        telemetry->sizing.ev_in_median_bps = computeQuantile(ev_in_values, 0.50);
        telemetry->sizing.ev_in_max_bps = computeQuantile(ev_in_values, 1.00);
        telemetry->sizing.ev_for_size_min_bps = computeQuantile(ev_for_size_values, 0.00);
        telemetry->sizing.ev_for_size_median_bps = computeQuantile(ev_for_size_values, 0.50);
        telemetry->sizing.ev_for_size_max_bps = computeQuantile(ev_for_size_values, 1.00);
        telemetry->sizing.size_fraction_min = computeQuantile(size_values, 0.00);
        telemetry->sizing.size_fraction_median = computeQuantile(size_values, 0.50);
        telemetry->sizing.size_fraction_max = computeQuantile(size_values, 1.00);
        telemetry->provenance.backend_request = params_.backend_request;
        telemetry->provenance.backend_effective = params_.backend_effective;
        telemetry->provenance.model_sha256 = params_.model_sha256;
    }

    return gated;
}

}  // namespace autolife::gate_vnext
