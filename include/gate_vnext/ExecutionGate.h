#pragma once

#include "gate_vnext/CandidateSnapshot.h"

#include <vector>

namespace autolife::gate_vnext {

class ExecutionGate {
public:
    std::vector<CandidateSnapshot> filterExecutable(
        const std::vector<CandidateSnapshot>& sized
    ) const;
};

}  // namespace autolife::gate_vnext
