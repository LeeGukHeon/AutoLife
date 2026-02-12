#pragma once

#include "common/Types.h"
#include <vector>
#include <string>

namespace autolife {
namespace analytics {

enum class MarketRegime {
    UNKNOWN,
    TRENDING_UP,        // Strong Uptrend (ADX > 25, MA align)
    TRENDING_DOWN,      // Strong Downtrend
    RANGING,            // Sideways / Chop (Low ADX)
    HIGH_VOLATILITY     // Dangerous Volatility (High ATR/Price)
};

struct RegimeAnalysis {
    MarketRegime regime;
    double adx;
    double atr_pct;     // ATR / Price
    double trend_score; // -1.0 (Down) to 1.0 (Up)
    std::string description;
};

class RegimeDetector {
public:
    RegimeDetector() = default;

    // Detect Current Regime based on recent candles
    RegimeAnalysis analyzeRegime(const std::vector<Candle>& candles);

private:
    // Helper helpers
    bool isTrendingUp(const std::vector<double>& prices, double adx);
    bool isTrendingDown(const std::vector<double>& prices, double adx);
};

} // namespace analytics
} // namespace autolife
