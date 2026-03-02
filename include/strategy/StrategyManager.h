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
        struct LiqVolGateSample {
            std::string market;
            double observed = 0.0;
            double threshold_dynamic = 0.0;
            int history_count = 0;
            bool pass = false;
            bool low_conf = false;
        };
        struct StructureGateSample {
            std::string market;
            std::string regime;
            double observed_score = 0.0;
            double threshold_before = 0.0;
            double threshold_after = 0.0;
            bool pass = false;
            bool relax_applied = false;
        };
        struct BearReboundGuardSample {
            std::string market;
            std::string regime;
            double observed = 0.0;
            double threshold_dynamic = 0.0;
            int history_count = 0;
            bool pass = false;
            bool low_conf = false;
        };

        int strategy_total = 0;
        int strategy_enabled = 0;
        int generated_signal_count = 0;
        int skipped_disabled_count = 0;
        int no_signal_count = 0;
        int exception_count = 0;
        int liq_vol_gate_observation_count = 0;
        int liq_vol_gate_pass_count = 0;
        int liq_vol_gate_fail_count = 0;
        int liq_vol_gate_low_conf_triggered_count = 0;
        double liq_vol_gate_observed_sum = 0.0;
        double liq_vol_gate_threshold_sum = 0.0;
        std::string liq_vol_gate_mode = "legacy_fixed";
        double liq_vol_gate_quantile_q = 0.0;
        int liq_vol_gate_window_minutes = 0;
        int liq_vol_gate_min_samples_required = 0;
        std::string liq_vol_gate_low_conf_action = "hold";
        std::vector<LiqVolGateSample> liq_vol_gate_samples;
        int structure_gate_observation_count = 0;
        int structure_gate_fail_count_total = 0;
        std::map<std::string, int> structure_gate_fail_count_by_regime;
        std::map<std::string, int> structure_gate_pass_count_by_regime;
        std::string structure_gate_mode = "legacy_fixed";
        double structure_gate_relax_delta = 0.0;
        double structure_gate_observed_score_sum = 0.0;
        double structure_gate_threshold_before_sum = 0.0;
        double structure_gate_threshold_after_sum = 0.0;
        std::vector<StructureGateSample> structure_gate_samples;
        int bear_rebound_observation_count = 0;
        int bear_rebound_pass_count = 0;
        int bear_rebound_fail_count = 0;
        int bear_rebound_low_conf_triggered_count = 0;
        std::map<std::string, int> bear_rebound_fail_count_by_regime;
        std::map<std::string, int> bear_rebound_pass_count_by_regime;
        std::string bear_rebound_mode = "legacy_fixed";
        double bear_rebound_quantile_q = 0.0;
        int bear_rebound_window_minutes = 0;
        int bear_rebound_min_samples_required = 0;
        std::string bear_rebound_low_conf_action = "hold";
        double bear_rebound_observed_sum = 0.0;
        double bear_rebound_threshold_sum = 0.0;
        std::vector<BearReboundGuardSample> bear_rebound_samples;
        std::map<std::string, int> generated_by_strategy;
        std::map<std::string, int> skipped_disabled_by_strategy;
        std::map<std::string, int> no_signal_by_strategy;
        std::map<std::string, int> no_signal_reason_counts;
        std::map<std::string, int> exception_by_strategy;
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

    std::map<std::string, IStrategy::Statistics> getAllStatistics() const;
    std::vector<std::shared_ptr<IStrategy>> getStrategies() const;
private:
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
};

} // namespace strategy
} // namespace autolife

