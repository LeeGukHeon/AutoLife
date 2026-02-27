#pragma once

#include "strategy/IStrategy.h"
#include "network/UpbitHttpClient.h"
#include "analytics/MarketScanner.h"
#include <vector>
#include <map>
#include <memory>
#include <mutex>

namespace autolife {
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
        std::map<std::string, int> no_signal_reason_counts;
        std::map<std::string, int> exception_by_strategy;
    };

    struct FilterDiagnostics {
        struct FrontierDecisionSample {
            std::string market;
            std::string strategy_name;
            std::string regime;
            bool frontier_enabled = false;
            bool frontier_pass = false;
            bool expected_value_pass = false;
            bool margin_pass = true;
            bool ev_confidence_pass = true;
            bool cost_tail_pass = true;
            bool manager_pass = false;
            double expected_value_observed = 0.0;
            double required_expected_value = 0.0;
            double expected_value_slack = 0.0;
            double margin_observed = 0.0;
            double margin_floor = 0.0;
            double margin_slack = 0.0;
            double ev_confidence_observed = 0.0;
            double ev_confidence_floor = 0.0;
            double ev_confidence_slack = 0.0;
            double cost_tail_observed = 0.0;
            double cost_tail_limit = 0.0;
            double cost_tail_slack = 0.0;
        };

        int input_count = 0;
        int output_count = 0;
        std::map<std::string, int> rejection_reason_counts;
        std::vector<FrontierDecisionSample> frontier_decision_samples;
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

    std::vector<Signal> filterSignalsWithDiagnostics(
        const std::vector<Signal>& signals,
        double min_strength,
        double min_expected_value,
        analytics::MarketRegime regime,
        FilterDiagnostics* diagnostics
    );

    std::map<std::string, IStrategy::Statistics> getAllStatistics() const;
    std::vector<std::shared_ptr<IStrategy>> getStrategies() const;
private:
    enum class StrategyRole {
        FOUNDATION,
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

    StrategyRole detectStrategyRole(const std::string& strategy_name) const;
    RegimePolicy getRegimePolicy(StrategyRole role, analytics::MarketRegime regime) const;
};

} // namespace strategy
} // namespace autolife

