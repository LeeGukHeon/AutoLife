#pragma once

#include "gate_vnext/CandidateSnapshot.h"

#include <vector>

namespace autolife::gate_vnext {

class QualitySelector {
public:
    explicit QualitySelector(int topk = 5) : topk_(topk) {}

    std::vector<CandidateSnapshot> selectTopKByMargin(
        const std::vector<CandidateSnapshot>& snapshots
    ) const;

    int topk() const { return topk_; }

private:
    int topk_ = 5;
};

}  // namespace autolife::gate_vnext
