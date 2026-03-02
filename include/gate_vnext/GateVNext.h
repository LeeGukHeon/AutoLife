#pragma once

#include "gate_vnext/CandidateSnapshot.h"
#include "gate_vnext/ExecutionGate.h"
#include "gate_vnext/QualitySelector.h"
#include "gate_vnext/RiskSizer.h"
#include "gate_vnext/Telemetry.h"

#include <vector>

namespace autolife::gate_vnext {

class GateVNext {
public:
    struct Params {
        int quality_topk = 5;
        double base_size = 0.1;
        double ev_scale_bps = 10.0;
    };

    explicit GateVNext(Params params = {});

    std::vector<CandidateSnapshot> run(
        const std::vector<CandidateSnapshot>& snapshots,
        GateVNextTelemetry* telemetry = nullptr
    ) const;

private:
    Params params_{};
    QualitySelector selector_{};
    RiskSizer sizer_{};
    ExecutionGate execution_gate_{};
};

}  // namespace autolife::gate_vnext
