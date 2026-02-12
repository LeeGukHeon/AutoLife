#include "analytics/RegimeDetector.h"
#include "analytics/TechnicalIndicators.h"
#include <numeric>
#include <algorithm>
#include <cmath>

namespace autolife {
namespace analytics {

RegimeAnalysis RegimeDetector::analyzeRegime(const std::vector<Candle>& candles) {
    RegimeAnalysis result;
    result.regime = MarketRegime::UNKNOWN;
    
    if (candles.size() < 50) {
        result.description = "Insufficient Data";
        return result;
    }

    double current_price = candles.back().close;

    // 1. Calculate Indicators
    double adx = TechnicalIndicators::calculateADX(candles, 14);
    double atr = TechnicalIndicators::calculateATR(candles, 14);
    double atr_pct = (atr / current_price) * 100.0;
    
    auto prices = TechnicalIndicators::extractClosePrices(candles);
    double ema20 = TechnicalIndicators::calculateEMA(prices, 20);
    double ema50 = TechnicalIndicators::calculateEMA(prices, 50);

    result.adx = adx;
    result.atr_pct = atr_pct;

    // 2. Volatility Check
    // If ATR is extremly high (> 3% of price for 1m candle? or 1h?)
    // Assuming 5m/15m standard. 3% is huge. 
    if (atr_pct > 2.0) {
        result.regime = MarketRegime::HIGH_VOLATILITY;
        result.description = "High Volatility (ATR > 2%)";
        result.trend_score = 0.0;
        return result;
    }

    // 3. Trend Check via EMA Alignment
    bool ema_bullish = ema20 > ema50;
    
    // Trend Score Calculation
    // Positive for Bullish, Negative for Bearish
    // Weighted by ADX
    double direction = ema_bullish ? 1.0 : -1.0;
    result.trend_score = direction * (adx / 100.0);

    // 4. Regime Classification
    if (adx >= 25.0) {
        if (ema_bullish) {
            result.regime = MarketRegime::TRENDING_UP;
            result.description = "Strong Uptrend";
        } else {
            result.regime = MarketRegime::TRENDING_DOWN;
            result.description = "Strong Downtrend";
        }
    } else {
        result.regime = MarketRegime::RANGING; // Sideways
        result.description = "Ranging / Weak Trend";
    }

    return result;
}

} // namespace analytics
} // namespace autolife
