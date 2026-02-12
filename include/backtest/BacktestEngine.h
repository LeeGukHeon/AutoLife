#pragma once

#include <vector>
#include <string>
#include <map>
#include "common/Types.h"
#include "common/Config.h"
#include "backtest/DataHistory.h"
#include "strategy/StrategyManager.h"
#include "analytics/RegimeDetector.h"
#include "risk/RiskManager.h"
#include "network/UpbitHttpClient.h"
#include <memory>

namespace autolife {
namespace backtest {

class BacktestEngine {
public:
    BacktestEngine();

    // Initialize engine with configuration
    void init(const Config& config);

    // Load historical data
    void loadData(const std::string& file_path);

    // Run the backtest simulation
    void run();

    // Get results
    struct Result {
        double final_balance;
        double total_profit;
        double max_drawdown;
        int total_trades;
        int winning_trades;
    };
    Result getResult() const;

private:

    std::vector<Candle> history_data_;
    
    // Account State
    double balance_krw_;
    double balance_asset_; // Simplified for single asset for now
    std::string market_name_ = "KRW-BTC"; // Default for backtest
    
    // Execution State
    std::vector<Candle> current_candles_;
    double dynamic_filter_value_ = 0.5; // Self-learning filter
    
    // Components
    std::shared_ptr<network::UpbitHttpClient> http_client_; // Mockable
    std::unique_ptr<strategy::StrategyManager> strategy_manager_;
    std::unique_ptr<analytics::RegimeDetector> regime_detector_;
    std::unique_ptr<risk::RiskManager> risk_manager_;

    // Performance Metrics
    double max_balance_;
    double max_drawdown_;
    int total_trades_;
    int winning_trades_;

    // Simulation Methods
    void processCandle(const Candle& candle);
    void checkOrders(const Candle& candle);
    void executeOrder(const Order& order, double price);
    
    // Self-learning
    void updateDynamicFilter();
};

} // namespace backtest
} // namespace autolife
