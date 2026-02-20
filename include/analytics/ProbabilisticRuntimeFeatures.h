#pragma once

#include "analytics/MarketScanner.h"
#include "strategy/IStrategy.h"

#include <string>
#include <vector>

namespace autolife {
namespace analytics {

// Build transformed feature vector aligned with
// scripts/train_probabilistic_pattern_model.py::build_feature_vector.
bool buildProbabilisticTransformedFeatures(
    const strategy::Signal& signal,
    const CoinMetrics& metrics,
    std::vector<double>& out_transformed_features,
    std::string* error_message = nullptr
);

} // namespace analytics
} // namespace autolife

