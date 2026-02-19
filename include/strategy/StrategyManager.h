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
    struct CollectDiagnostics {
        int strategy_total = 0;
        int strategy_enabled = 0;
        int generated_signal_count = 0;
        int skipped_disabled_count = 0;
        int no_signal_count = 0;
        int exception_count = 0;
        std::map<std::string, int> generated_by_strategy;
        std::map<std::string, int> skipped_disabled_by_strategy;
        std::map<std::string, int> no_signal_by_strategy;
        std::map<std::string, int> exception_by_strategy;
    };

    struct FilterDiagnostics {
        int input_count = 0;
        int output_count = 0;
        std::map<std::string, int> rejection_reason_counts;
    };

    struct SelectionDiagnostics {
        int input_count = 0;
        int directional_candidate_count = 0;
        int scored_candidate_count = 0;
        std::map<std::string, int> rejection_reason_counts;
        int live_hint_adjusted_candidate_count = 0;
        std::map<std::string, int> live_hint_adjustment_counts;
    };

    struct LiveSignalBottleneckHint {
        bool enabled = false;
        std::string top_group;
        bool no_trade_bias_active = false;
        double signal_generation_share = 0.0;
        double manager_prefilter_share = 0.0;
        double position_state_share = 0.0;
    };

    StrategyManager(std::shared_ptr<network::UpbitHttpClient> client);

    void registerStrategy(std::shared_ptr<IStrategy> strategy);
    std::shared_ptr<IStrategy> getStrategy(const std::string& name);

    std::vector<Signal> collectSignals(
        const std::string& market,
        const analytics::CoinMetrics& metrics,
        const std::vector<Candle>& candles,
        double current_price,
        double available_capital,
        const analytics::RegimeAnalysis& regime,
        CollectDiagnostics* diagnostics = nullptr
    );

    Signal selectBestSignal(const std::vector<Signal>& signals);
    Signal selectRobustSignalWithDiagnostics(
        const std::vector<Signal>& signals,
        analytics::MarketRegime regime,
        SelectionDiagnostics* diagnostics
    );

    std::vector<Signal> filterSignalsWithDiagnostics(
        const std::vector<Signal>& signals,
        double min_strength,
        double min_expected_value,
        analytics::MarketRegime regime,
        FilterDiagnostics* diagnostics
    );

    std::map<std::string, IStrategy::Statistics> getAllStatistics() const;
    std::vector<std::shared_ptr<IStrategy>> getStrategies() const;
    void setLiveSignalBottleneckHint(const LiveSignalBottleneckHint& hint);
    LiveSignalBottleneckHint getLiveSignalBottleneckHint() const;
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
        FOUNDATION,
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
    LiveSignalBottleneckHint live_bottleneck_hint_;

    double calculateSignalScore(const Signal& signal) const;
    StrategyRole detectStrategyRole(const std::string& strategy_name) const;
    RegimePolicy getRegimePolicy(StrategyRole role, analytics::MarketRegime regime) const;
    PerformanceGate getPerformanceGate(StrategyRole role, analytics::MarketRegime regime) const;
};

} // namespace strategy
} // namespace autolife

