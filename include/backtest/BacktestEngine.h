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
#include "engine/AdaptivePolicyController.h"
#include "engine/PerformanceStore.h"
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
        struct StrategySummary {
            std::string strategy_name;
            int total_trades = 0;
            int winning_trades = 0;
            int losing_trades = 0;
            double win_rate = 0.0;
            double total_profit = 0.0;
            double avg_win_krw = 0.0;
            double avg_loss_krw = 0.0;
            double profit_factor = 0.0;
        };
        struct PatternSummary {
            std::string strategy_name;
            std::string entry_archetype;
            std::string regime;
            std::string strength_bucket;
            std::string expected_value_bucket;
            std::string reward_risk_bucket;
            int total_trades = 0;
            int winning_trades = 0;
            int losing_trades = 0;
            double win_rate = 0.0;
            double total_profit = 0.0;
            double avg_profit_krw = 0.0;
            double profit_factor = 0.0;
        };
        struct EntryFunnelSummary {
            int entry_rounds = 0;
            int skipped_due_to_open_position = 0;
            int no_signal_generated = 0;
            int filtered_out_by_manager = 0;
            int filtered_out_by_policy = 0;
            int no_best_signal = 0;
            int blocked_pattern_gate = 0;
            int blocked_rr_rebalance = 0;
            int blocked_risk_gate = 0;
            int blocked_risk_manager = 0;
            int blocked_min_order_or_capital = 0;
            int blocked_order_sizing = 0;
            int entries_executed = 0;
        };
        struct StrategySignalFunnel {
            std::string strategy_name;
            int generated_signals = 0;
            int selected_best = 0;
            int blocked_by_risk_manager = 0;
            int entries_executed = 0;
        };

        double final_balance;
        double total_profit;
        double max_drawdown;
        int total_trades;
        int winning_trades;
        int losing_trades;
        double win_rate;
        double avg_win_krw;
        double avg_loss_krw;
        double profit_factor;
        double expectancy_krw;
        double avg_holding_minutes = 0.0;
        double avg_fee_krw = 0.0;
        int intrabar_stop_tp_collision_count = 0;
        std::map<std::string, int> exit_reason_counts;
        std::map<std::string, int> intrabar_collision_by_strategy;
        std::vector<StrategySummary> strategy_summaries;
        std::vector<PatternSummary> pattern_summaries;
        std::vector<StrategySignalFunnel> strategy_signal_funnel;
        EntryFunnelSummary entry_funnel;
    };
    Result getResult() const;

private:

    std::vector<Candle> history_data_;
    engine::EngineConfig engine_config_;
    
    // Account State
    double balance_krw_;
    double balance_asset_; // Simplified for single asset for now
    std::string market_name_ = "KRW-BTC"; // Default for backtest
    
    // Execution State
    std::vector<Candle> current_candles_;
    std::map<std::string, std::vector<Candle>> loaded_tf_candles_;
    std::map<std::string, size_t> loaded_tf_cursors_;
    double dynamic_filter_value_ = 0.46; // Self-learning filter (backtest bootstrap)
    int no_entry_streak_candles_ = 0;   // Regime-aware minimum activation helper
    double market_hostility_ewma_ = 0.0;
    int hostile_entry_pause_candles_ = 0;
    struct PendingBacktestOrder {
        Order order;
        double requested_price = 0.0;
        long long enqueued_at_ms = 0;
    };
    std::vector<PendingBacktestOrder> pending_orders_;
    long long backtest_order_seq_ = 0;
    
    // Components
    std::shared_ptr<network::UpbitHttpClient> http_client_; // Mockable
    std::unique_ptr<strategy::StrategyManager> strategy_manager_;
    std::unique_ptr<analytics::RegimeDetector> regime_detector_;
    std::unique_ptr<engine::AdaptivePolicyController> policy_controller_;
    std::unique_ptr<engine::PerformanceStore> performance_store_;
    std::unique_ptr<risk::RiskManager> risk_manager_;

    // Performance Metrics
    double max_balance_;
    double max_drawdown_;
    int total_trades_;
    int winning_trades_;
    Result::EntryFunnelSummary entry_funnel_;
    std::map<std::string, int> strategy_generated_counts_;
    std::map<std::string, int> strategy_selected_best_counts_;
    std::map<std::string, int> strategy_blocked_by_risk_manager_counts_;
    std::map<std::string, int> strategy_entries_executed_counts_;
    int intrabar_stop_tp_collision_count_ = 0;
    std::map<std::string, int> intrabar_collision_by_strategy_;

    // Simulation Methods
    void processCandle(const Candle& candle);
    void checkOrders(const Candle& candle);
    void executeOrder(const Order& order, double price);

    void loadCompanionTimeframes(const std::string& file_path);
    std::vector<Candle> getTimeframeCandles(
        const std::string& timeframe,
        long long current_timestamp,
        int fallback_minutes,
        size_t max_bars
    );
    static void normalizeTimestampsToMs(std::vector<Candle>& candles);
    static std::vector<Candle> aggregateCandles(
        const std::vector<Candle>& candles_1m,
        int timeframe_minutes,
        size_t max_bars
    );
    static long long toMsTimestamp(long long ts);
    
    // Self-learning
    void updateDynamicFilter();
};

} // namespace backtest
} // namespace autolife
