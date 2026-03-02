#pragma once

#include "gate_vnext/CandidateSnapshot.h"

#include <vector>

namespace autolife::gate_vnext {

class RiskSizer {
public:
    RiskSizer(double base_size = 0.1, double ev_scale_bps = 10.0)
        : base_size_(base_size), ev_scale_bps_(ev_scale_bps) {}

    std::vector<CandidateSnapshot> applySizing(
        const std::vector<CandidateSnapshot>& selected
    ) const;

private:
    double base_size_ = 0.1;
    double ev_scale_bps_ = 10.0;
};

}  // namespace autolife::gate_vnext
