#pragma once

#include "strategy/IStrategy.h"
#include "network/UpbitHttpClient.h"

#include <memory>
#include <mutex>

namespace autolife {
namespace strategy {

class FoundationAdaptiveStrategy : public IStrategy {
public:
    explicit FoundationAdaptiveStrategy(std::shared_ptr<network::UpbitHttpClient> client);

    StrategyInfo getInfo() const override;

    Signal generateSignal(
        const std::string& market,
        const analytics::CoinMetrics& metrics,
        const std::vector<Candle>& candles,
        double current_price,
        double available_capital,
        const analytics::RegimeAnalysis& regime
    ) override;

    bool shouldEnter(
        const std::string& market,
        const analytics::CoinMetrics& metrics,
        const std::vector<Candle>& candles,
        double current_price,
        const analytics::RegimeAnalysis& regime
    ) override;

    bool shouldExit(
        const std::string& market,
        double entry_price,
        double current_price,
        double holding_time_seconds
    ) override;

    double calculateStopLoss(
        double entry_price,
        const std::vector<Candle>& candles
    ) override;

    double calculateTakeProfit(
        double entry_price,
        const std::vector<Candle>& candles
    ) override;

    double calculatePositionSize(
        double capital,
        double entry_price,
        double stop_loss,
        const analytics::CoinMetrics& metrics
    ) override;

    void setEnabled(bool enabled) override;
    bool isEnabled() const override;

    Statistics getStatistics() const override;
    void updateStatistics(const std::string& market, bool is_win, double profit_loss) override;
    void setStatistics(const Statistics& stats) override;

private:
    static long long nowMs();
    static double clampRiskPctByRegime(analytics::MarketRegime regime, double atr_pct);
    static double targetRewardRiskByRegime(analytics::MarketRegime regime);
    static std::string archetypeByRegime(analytics::MarketRegime regime);

    std::shared_ptr<network::UpbitHttpClient> client_;
    bool enabled_ = true;
    mutable std::mutex mutex_;
    Statistics stats_;
};

} // namespace strategy
} // namespace autolife

