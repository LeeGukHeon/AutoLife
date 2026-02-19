#pragma once

#include "analytics/RegimeDetector.h"

#include <string>
#include <string_view>

namespace autolife::common::runtime_diag {

const char* marketRegimeLabel(analytics::MarketRegime regime);

std::string strengthBucket(double strength);
std::string expectedValueBucket(double expected_value);
std::string rewardRiskBucket(double rr);
std::string volatilityBucket(double volatility);
std::string liquidityBucket(double liquidity_score);

const char* classifySignalRejectionGroup(std::string_view reason);

} // namespace autolife::common::runtime_diag
