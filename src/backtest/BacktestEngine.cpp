#include "backtest/BacktestEngine.h"
#include "common/Logger.h"
#include <iostream>
#include <algorithm>

#include "strategy/ScalpingStrategy.h"
#include "strategy/MomentumStrategy.h"
#include "strategy/BreakoutStrategy.h"
#include "strategy/MeanReversionStrategy.h"
#include "strategy/GridTradingStrategy.h"

namespace autolife {
namespace backtest {

BacktestEngine::BacktestEngine() 
    : balance_krw_(0), balance_asset_(0), 
      max_balance_(0), max_drawdown_(0), 
      total_trades_(0), winning_trades_(0) {
      
      // Initialize with dummy/mock client for backtest
      // In a real scenario, we might want a MockUpbitHttpClient
      http_client_ = std::make_shared<network::UpbitHttpClient>("BACKTEST_KEY", "BACKTEST_SECRET"); 
      
      // Initialize Components
      strategy_manager_ = std::make_unique<strategy::StrategyManager>(http_client_);
      regime_detector_ = std::make_unique<analytics::RegimeDetector>();
      // RiskManager will be initialized in init() with config capital
}

void BacktestEngine::init(const Config& config) {

    balance_krw_ = config.getInitialCapital();
    balance_asset_ = 0.0;
    max_balance_ = balance_krw_;
    
    // Reset Risk Manager with initial capital
    risk_manager_ = std::make_unique<risk::RiskManager>(balance_krw_);
    // 백테스트에서는 실시간 쿨다운 비활성화 (실시간이 아닌 시뮬레이션 시간 사용)
    risk_manager_->setMinReentryInterval(0);
    risk_manager_->setMaxDailyTrades(1000);
    
    // Register Strategies (Similar to TradingEngine)
    // For backtest, we might want to enable all or specific ones
    auto scalping = std::make_shared<strategy::ScalpingStrategy>(http_client_);
    strategy_manager_->registerStrategy(scalping);
    
    auto momentum = std::make_shared<strategy::MomentumStrategy>(http_client_);
    strategy_manager_->registerStrategy(momentum);
    
    auto breakout = std::make_shared<strategy::BreakoutStrategy>(http_client_);
    strategy_manager_->registerStrategy(breakout);
    
    auto mean_rev = std::make_shared<strategy::MeanReversionStrategy>(http_client_);
    strategy_manager_->registerStrategy(mean_rev);
    
    auto grid = std::make_shared<strategy::GridTradingStrategy>(http_client_);
    strategy_manager_->registerStrategy(grid);
    
    LOG_INFO("BacktestEngine Initialized with all strategies.");
}

void BacktestEngine::loadData(const std::string& file_path) {
    if (file_path.find(".json") != std::string::npos) {
        history_data_ = DataHistory::loadJSON(file_path);
    } else {
        history_data_ = DataHistory::loadCSV(file_path);
    }
}

void BacktestEngine::run() {
    LOG_INFO("Starting Backtest with {} candles.", history_data_.size());
    
    for (const auto& candle : history_data_) {
        processCandle(candle);
    }
    
    LOG_INFO("Backtest Completed.");
    LOG_INFO("Final Balance: {}", balance_krw_ + (balance_asset_ * history_data_.back().close));
}

void BacktestEngine::processCandle(const Candle& candle) {
    // 1. Accumulate History
    current_candles_.push_back(candle);
    if (current_candles_.size() > 200) {
        current_candles_.erase(current_candles_.begin()); // Keep window size
    }
    
    // Need enough data
    if (current_candles_.size() < 100) return;

    double current_price = candle.close;
    
    // 2. Market/Regime Analysis
    auto regime = regime_detector_->analyzeRegime(current_candles_);
    analytics::CoinMetrics metrics; // Dummy metrics for backtest
    metrics.market = market_name_;
    metrics.candles = current_candles_;
    metrics.current_price = current_price;
    metrics.liquidity_score = 80.0; // Assume good liquidity
    metrics.volatility = regime.atr_pct;
    metrics.price_momentum = 50.0; // Placeholder
    
    // Calculate price_change_rate from previous candle
    if (current_candles_.size() >= 2) {
        double prev_close = current_candles_[current_candles_.size() - 2].close;
        if (prev_close > 0) {
            metrics.price_change_rate = ((current_price - prev_close) / prev_close) * 100.0;
        }
    }
    
    // Estimate volume surge from recent average
    if (current_candles_.size() >= 20) {
        double avg_vol = 0;
        for (size_t vi = current_candles_.size() - 20; vi < current_candles_.size() - 1; ++vi) {
            avg_vol += current_candles_[vi].volume;
        }
        avg_vol /= 19.0;
        metrics.volume_surge_ratio = (avg_vol > 0) ? (candle.volume / avg_vol) : 1.0;
    } else {
        metrics.volume_surge_ratio = 1.0;
    }
    
    // Synthetic orderbook for backtest (so strategies can compute microstructure score)
    metrics.order_book_imbalance = 0.1; // Slight buy pressure
    metrics.buy_pressure = 55.0;
    metrics.sell_pressure = 45.0;
    
    // Generate synthetic orderbook_units JSON (5 levels)
    {
        nlohmann::json units = nlohmann::json::array();
        double spread_pct = 0.01; // 0.01% spread (tight, like real BTC/KRW)
        double base_size = 0.5;   // Base order size in BTC
        
        for (int level = 0; level < 5; ++level) {
            double offset = spread_pct * (level + 1) / 100.0;
            double bid_price = current_price * (1.0 - offset);
            double ask_price = current_price * (1.0 + offset);
            
            // Slight buy pressure: bid sizes slightly larger
            double bid_size = base_size * (1.0 + 0.1 * (5 - level)) + rand() % 100 / 200.0;
            double ask_size = base_size * (1.0 + 0.05 * (5 - level)) + rand() % 100 / 200.0;
            
            nlohmann::json unit;
            unit["ask_price"] = ask_price;
            unit["bid_price"] = bid_price;
            unit["ask_size"] = ask_size;
            unit["bid_size"] = bid_size;
            units.push_back(unit);
        }
        metrics.orderbook_units = units;
    }

    // 3. Monitor & Exit Positions
    // In TradingEngine this is done by iterating active positions
    // Here we check if we have an open position in RiskManager
    auto position = risk_manager_->getPosition(market_name_);
    if (position) {
        auto strategy = strategy_manager_->getStrategy(position->strategy_name);
        if (strategy) {
            // Check Exit Condition
            bool should_exit = strategy->shouldExit(
                market_name_,
                position->entry_price,
                current_price,
                (candle.timestamp - position->entry_time) / 1000.0 // holding seconds
            );
            
            // Check Stop Loss / Take Profit (RiskManager handled?)
            // RiskManager::managePositions usually checks SL/TP. 
            // We should simulate that here.
            // Simplified:
            if (should_exit) {
                // Execute Sell
                Order order;
                order.market = market_name_;
                order.side = OrderSide::SELL;
                order.volume = position->quantity;
                order.price = current_price;
                order.strategy_name = position->strategy_name;
                executeOrder(order, current_price);
                risk_manager_->exitPosition(market_name_, current_price, "StrategyExit");
            } else {
                 // Check SL/TP via RiskManager logic simulation
                 if (current_price <= position->stop_loss) {
                     Order sl_order;
                     sl_order.market = market_name_;
                     sl_order.side = OrderSide::SELL;
                     sl_order.price = current_price;
                     sl_order.volume = position->quantity;
                     sl_order.strategy_name = position->strategy_name;
                     executeOrder(sl_order, current_price);
                     risk_manager_->exitPosition(market_name_, current_price, "StopLoss");
                 } else if (current_price >= position->take_profit_1) {
                     Order tp_order;
                     tp_order.market = market_name_;
                     tp_order.side = OrderSide::SELL;
                     tp_order.price = current_price;
                     tp_order.volume = position->quantity;
                     tp_order.strategy_name = position->strategy_name;
                     executeOrder(tp_order, current_price);
                     risk_manager_->exitPosition(market_name_, current_price, "TakeProfit");
                 }
            }
        }
    }
    
    // 4. Generate Entry Signals (only if no position)
    if (!position) {
        auto signals = strategy_manager_->collectSignals(
            market_name_,
            metrics,
            current_candles_,
            current_price,
            balance_krw_,
            regime
        );
        
        // Dynamic Filter Simulation (Self-learning stub)
        // If we had recent losses, filter might increase.
        double filter_threshold = dynamic_filter_value_;
        
        auto best_signal = strategy_manager_->selectBestSignal(
            strategy_manager_->filterSignals(signals, filter_threshold)
        );
        
        if (best_signal.type != strategy::SignalType::NONE) {
            // Validate Risk
            if (risk_manager_->canEnterPosition(
                market_name_,
                current_price,
                best_signal.position_size,
                best_signal.strategy_name
            )) {
                // Execute Buy
                double quantity = (balance_krw_ * best_signal.position_size) / current_price;
                // Basic Fee adjust
                double fee = quantity * current_price * 0.0005;
                if (balance_krw_ >= (quantity * current_price) + fee) {
                    Order order;
                    order.market = market_name_;
                    order.side = OrderSide::BUY;
                    order.price = current_price;
                    order.volume = quantity;
                    order.strategy_name = best_signal.strategy_name;
                    
                    executeOrder(order, current_price);
                    
                    // Register with Risk Manager
                    risk_manager_->enterPosition(
                        market_name_,
                        current_price,
                        quantity,
                        best_signal.stop_loss,
                        best_signal.take_profit_1,
                        best_signal.take_profit_2,
                        best_signal.strategy_name
                    );
                }
            }
        }
    }

    // 5. Update Portfolio Value for Drawdown calculation
    double current_equity = balance_krw_;
    auto pos = risk_manager_->getPosition(market_name_);
    if (pos) {
        current_equity += pos->quantity * current_price;
    }
    
    if (current_equity > max_balance_) {
        max_balance_ = current_equity;
    }
    
    double drawdown = (max_balance_ > 0) ? (max_balance_ - current_equity) / max_balance_ : 0.0;
    if (drawdown > max_drawdown_) {
        max_drawdown_ = drawdown;
    }
    
    // 6. Self-Learning Update
    updateDynamicFilter();
}

void BacktestEngine::checkOrders(const Candle& candle) {
    // This would iterate over a list of active orders.
    // For scaffolding, we leave this empty or implement a simple fill logic
}

void BacktestEngine::executeOrder(const Order& order, double price) {
    // Simulate Fee
    double fee_rate = Config::getInstance().getFeeRate();
    double trade_amount = order.price * order.volume;
    double fee = trade_amount * fee_rate;
    
    if (order.side == OrderSide::BUY) {
        if (balance_krw_ >= trade_amount + fee) {
            balance_krw_ -= (trade_amount + fee);
            // balance_asset_ is now tracked by RiskManager via enterPosition
            // but we keep it here for simple PnL calc if needed, 
            // checking RiskManager is better.
            total_trades_++;
            // LOG_DEBUG("Filled BUY at " + std::to_string(price)); 
        }
    } else if (order.side == OrderSide::SELL) {
        // Assume RiskManager manages quantity check, we just execute
        balance_krw_ += (trade_amount - fee);
        
        // Simple Win/Loss Check
        // Ideally we compare with entry price, but here we just track raw trades
        // Winning trades should be tracked via RiskManager history
        total_trades_++;
    }
}

BacktestEngine::Result BacktestEngine::getResult() const {
    double final_equity = balance_krw_;
    if (!history_data_.empty()) {
        final_equity += balance_asset_ * history_data_.back().close;
    }
    
    return {
        final_equity,
        final_equity - Config::getInstance().getInitialCapital(),
        max_drawdown_,
        total_trades_,
        winning_trades_
    };
}



void BacktestEngine::updateDynamicFilter() {
    // Simple Self-Learning Logic
    // Adjust filter based on recent 20 trades win rate
    auto history = risk_manager_->getTradeHistory();
    if (history.size() < 20) return;
    
    int recent_wins = 0;
    int count = 0;
    for (auto it = history.rbegin(); it != history.rend() && count < 20; ++it, ++count) {
        if (it->profit_loss > 0) recent_wins++;
    }
    
    double win_rate = static_cast<double>(recent_wins) / count;
    
    // Target Win Rate: 0.6 (60%)
    if (win_rate < 0.60) {
        // Performance dropped, tighten filter
        dynamic_filter_value_ = std::min(0.80, dynamic_filter_value_ + 0.005);
    } else if (win_rate > 0.70) {
        // Performance good, loosen filter to get more opportunities
        dynamic_filter_value_ = std::max(0.40, dynamic_filter_value_ - 0.005);
    }
}

} // namespace backtest
} // namespace autolife
