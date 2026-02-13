#pragma once

#include "strategy/IStrategy.h"
#include "network/UpbitHttpClient.h"
#include "analytics/MarketScanner.h"
#include <vector>
#include <map>
#include <memory>
#include <mutex>

namespace autolife {
namespace risk {
struct TradeHistory;
}
namespace strategy {

// Aggregates strategy execution and signal selection/filtering.
class StrategyManager {
public:
    StrategyManager(std::shared_ptr<network::UpbitHttpClient> client);

    void registerStrategy(std::shared_ptr<IStrategy> strategy);
    std::shared_ptr<IStrategy> getStrategy(const std::string& name);

    std::vector<Signal> collectSignals(
        const std::string& market,
        const analytics::CoinMetrics& metrics,
        const std::vector<Candle>& candles,
        double current_price,
        double available_capital,
        const analytics::RegimeAnalysis& regime
    );

    Signal selectBestSignal(const std::vector<Signal>& signals);
    Signal selectRobustSignal(
        const std::vector<Signal>& signals,
        analytics::MarketRegime regime
    );

    std::vector<Signal> filterSignals(
        const std::vector<Signal>& signals,
        double min_strength = 0.6,
        double min_expected_value = 0.0,
        analytics::MarketRegime regime = analytics::MarketRegime::UNKNOWN
    );

    Signal synthesizeSignals(const std::vector<Signal>& signals);

    std::map<std::string, IStrategy::Statistics> getAllStatistics() const;
    void enableStrategy(const std::string& name, bool enabled);
    std::vector<std::string> getActiveStrategies() const;
    double getOverallWinRate() const;
    std::vector<std::shared_ptr<IStrategy>> getStrategies() const;
    void refreshStrategyStatesFromHistory(
        const std::vector<risk::TradeHistory>& history,
        analytics::MarketRegime dominant_regime,
        bool avoid_high_volatility,
        bool avoid_trending_down,
        int min_trades_for_ev,
        double min_expectancy_krw,
        double min_profit_factor
    );

private:
    enum class StrategyRole {
        SCALPING,
        MOMENTUM,
        BREAKOUT,
        MEAN_REVERSION,
        GRID,
        OTHER
    };

    enum class RegimePolicy {
        ALLOW,
        HOLD,
        BLOCK
    };

    struct PerformanceGate {
        double min_win_rate = 0.0;
        double min_profit_factor = 0.0;
        int min_sample_trades = 0;
    };

    Signal processStrategySignal(
        std::shared_ptr<IStrategy> strategy,
        const std::string& market,
        const analytics::CoinMetrics& metrics,
        const std::vector<Candle>& candles,
        double current_price,
        double available_capital,
        const analytics::RegimeAnalysis& regime
    );

    std::vector<std::shared_ptr<IStrategy>> strategies_;
    std::shared_ptr<network::UpbitHttpClient> client_;
    mutable std::mutex mutex_;

    double calculateSignalScore(const Signal& signal) const;
    StrategyRole detectStrategyRole(const std::string& strategy_name) const;
    RegimePolicy getRegimePolicy(StrategyRole role, analytics::MarketRegime regime) const;
    PerformanceGate getPerformanceGate(StrategyRole role, analytics::MarketRegime regime) const;
};

} // namespace strategy
} // namespace autolife
