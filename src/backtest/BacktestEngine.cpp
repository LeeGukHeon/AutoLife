#include "backtest/BacktestEngine.h"
#include "common/Logger.h"
#include "common/PathUtils.h"
#include "core/execution/OrderLifecycleStateMachine.h"
#include "core/execution/ExecutionUpdateSchema.h"
#include <iostream>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>

#include "strategy/ScalpingStrategy.h"
#include "strategy/MomentumStrategy.h"
#include "strategy/BreakoutStrategy.h"
#include "strategy/MeanReversionStrategy.h"
#include "strategy/GridTradingStrategy.h"

namespace autolife {
namespace backtest {

namespace {
constexpr double ENTRY_SLIPPAGE_PCT = 0.0005;  // 0.05%
constexpr double EXIT_SLIPPAGE_PCT = 0.0007;   // 0.07%
constexpr double STOP_SLIPPAGE_PCT = 0.0015;   // 0.15% (worse fill on stop)

long long getCurrentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

std::filesystem::path executionUpdateArtifactPath() {
    return autolife::utils::PathUtils::resolveRelativePath("logs/execution_updates_backtest.jsonl");
}

void resetExecutionUpdateArtifact() {
    const auto path = executionUpdateArtifactPath();
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
}

void appendExecutionUpdateArtifact(const autolife::core::ExecutionUpdate& update) {
    const auto path = executionUpdateArtifactPath();
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out.is_open()) {
        LOG_WARN("Execution update artifact open failed: {}", path.string());
        return;
    }
    out << autolife::core::execution::toJson(update).dump() << "\n";
}

bool passesRegimeGate(analytics::MarketRegime regime, const engine::EngineConfig& cfg) {
    if (cfg.avoid_high_volatility && regime == analytics::MarketRegime::HIGH_VOLATILITY) {
        return false;
    }
    if (cfg.avoid_trending_down && regime == analytics::MarketRegime::TRENDING_DOWN) {
        return false;
    }
    return true;
}

double computeTargetRewardRisk(double strength, const engine::EngineConfig& cfg) {
    const double weak_rr = std::max(0.5, cfg.min_rr_weak_signal);
    const double strong_rr = std::max(0.5, std::min(cfg.min_rr_strong_signal, weak_rr));
    const double t = std::clamp((strength - 0.40) / 0.60, 0.0, 1.0);
    return weak_rr - (weak_rr - strong_rr) * t;
}

bool rebalanceSignalRiskReward(strategy::Signal& signal, const engine::EngineConfig& cfg) {
    if (signal.entry_price <= 0.0 || signal.stop_loss <= 0.0 || signal.stop_loss >= signal.entry_price) {
        return false;
    }

    const double risk_price = signal.entry_price - signal.stop_loss;
    if (risk_price <= 0.0) {
        return false;
    }

    if (signal.take_profit_1 <= signal.entry_price) {
        signal.take_profit_1 = signal.entry_price + risk_price * 1.05;
    }
    if (signal.take_profit_2 <= signal.entry_price) {
        signal.take_profit_2 = signal.take_profit_1;
    }

    const double target_rr = computeTargetRewardRisk(signal.strength, cfg);
    const double current_rr = (signal.take_profit_2 - signal.entry_price) / risk_price;
    if (current_rr + 1e-9 < target_rr) {
        signal.take_profit_2 = signal.entry_price + risk_price * target_rr;
    }

    const double min_tp1_rr = std::max(1.0, target_rr * 0.60);
    const double min_tp1 = signal.entry_price + risk_price * min_tp1_rr;
    if (signal.take_profit_1 < min_tp1) {
        signal.take_profit_1 = min_tp1;
    }
    if (signal.take_profit_2 < signal.take_profit_1) {
        signal.take_profit_2 = signal.take_profit_1;
    }
    return true;
}

struct StrategyEdgeStats {
    int trades = 0;
    int wins = 0;
    double gross_profit = 0.0;
    double gross_loss_abs = 0.0;
    double net_profit = 0.0;

    double expectancy() const {
        return (trades > 0) ? (net_profit / static_cast<double>(trades)) : 0.0;
    }
    double winRate() const {
        return (trades > 0) ? (static_cast<double>(wins) / static_cast<double>(trades)) : 0.0;
    }
    double profitFactor() const {
        return (gross_loss_abs > 1e-12) ? (gross_profit / gross_loss_abs) : 0.0;
    }
};

std::map<std::string, StrategyEdgeStats> buildStrategyEdgeStats(const std::vector<risk::TradeHistory>& history) {
    std::map<std::string, StrategyEdgeStats> out;
    for (const auto& trade : history) {
        if (trade.strategy_name.empty()) {
            continue;
        }
        auto& s = out[trade.strategy_name];
        s.trades++;
        s.net_profit += trade.profit_loss;
        if (trade.profit_loss > 0.0) {
            s.wins++;
            s.gross_profit += trade.profit_loss;
        } else if (trade.profit_loss < 0.0) {
            s.gross_loss_abs += std::abs(trade.profit_loss);
        }
    }
    return out;
}

void normalizeSignalStopLossByRegime(strategy::Signal& signal, analytics::MarketRegime regime) {
    if (signal.entry_price <= 0.0) {
        return;
    }

    double min_risk_pct = 0.0035;
    double max_risk_pct = 0.0100;
    if (regime == analytics::MarketRegime::HIGH_VOLATILITY) {
        min_risk_pct = 0.0060;
        max_risk_pct = 0.0150;
    } else if (regime == analytics::MarketRegime::TRENDING_DOWN) {
        min_risk_pct = 0.0050;
        max_risk_pct = 0.0120;
    }

    const double strength_t = std::clamp((signal.strength - 0.40) / 0.60, 0.0, 1.0);
    const double tighten = 1.0 - (0.15 * strength_t);
    min_risk_pct *= tighten;
    max_risk_pct *= tighten;
    if (max_risk_pct < min_risk_pct) {
        max_risk_pct = min_risk_pct;
    }

    double risk_pct = 0.0;
    if (signal.stop_loss > 0.0 && signal.stop_loss < signal.entry_price) {
        risk_pct = (signal.entry_price - signal.stop_loss) / signal.entry_price;
    }
    if (risk_pct <= 0.0) {
        risk_pct = (min_risk_pct + max_risk_pct) * 0.5;
    }

    risk_pct = std::clamp(risk_pct, min_risk_pct, max_risk_pct);
    signal.stop_loss = signal.entry_price * (1.0 - risk_pct);
}

bool passesEntryQualityGate(const strategy::Signal& signal, const engine::EngineConfig& cfg) {
    const double entry_price = signal.entry_price;
    const double take_profit_price = (signal.take_profit_2 > 0.0) ? signal.take_profit_2 : signal.take_profit_1;
    const double stop_loss_price = signal.stop_loss;

    if (entry_price <= 0.0 || take_profit_price <= 0.0 || stop_loss_price <= 0.0 ||
        take_profit_price <= entry_price || stop_loss_price >= entry_price) {
        return false;
    }

    const double gross_reward_pct = (take_profit_price - entry_price) / entry_price;
    const double gross_risk_pct = (entry_price - stop_loss_price) / entry_price;
    const double reward_risk_ratio = (gross_risk_pct > 1e-8) ? (gross_reward_pct / gross_risk_pct) : 0.0;

    const double fee_rate_per_side = Config::getInstance().getFeeRate();
    const double round_trip_cost_pct = (fee_rate_per_side * 2.0) + (cfg.max_slippage_pct * 2.0);
    const double expected_net_edge_pct = gross_reward_pct - round_trip_cost_pct;

    return reward_risk_ratio >= cfg.min_reward_risk &&
           expected_net_edge_pct >= cfg.min_expected_edge_pct;
}
}

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

    engine_config_ = config.getEngineConfig();
    resetExecutionUpdateArtifact();
    balance_krw_ = config.getInitialCapital();
    balance_asset_ = 0.0;
    max_balance_ = balance_krw_;
    
    // Reset Risk Manager with initial capital
    risk_manager_ = std::make_unique<risk::RiskManager>(balance_krw_);
    // 백테스트에서는 실시간 쿨다운 비활성화 (실시간이 아닌 시뮬레이션 시간 사용)
    risk_manager_->setMinReentryInterval(0);
    risk_manager_->setMaxDailyTrades(1000);
    
    std::set<std::string> enabled;
    for (const auto& s : engine_config_.enabled_strategies) {
        enabled.insert(s);
    }
    auto should_register = [&](const std::string& name) {
        if (enabled.empty()) {
            return true;
        }
        if (enabled.count(name) > 0) {
            return true;
        }
        if (name == "grid_trading" && enabled.count("grid") > 0) {
            return true;
        }
        return false;
    };

    if (should_register("scalping")) {
        auto scalping = std::make_shared<strategy::ScalpingStrategy>(http_client_);
        strategy_manager_->registerStrategy(scalping);
    }

    if (should_register("momentum")) {
        auto momentum = std::make_shared<strategy::MomentumStrategy>(http_client_);
        strategy_manager_->registerStrategy(momentum);
    }

    if (should_register("breakout")) {
        auto breakout = std::make_shared<strategy::BreakoutStrategy>(http_client_);
        strategy_manager_->registerStrategy(breakout);
    }

    if (should_register("mean_reversion")) {
        auto mean_rev = std::make_shared<strategy::MeanReversionStrategy>(http_client_);
        strategy_manager_->registerStrategy(mean_rev);
    }

    if (should_register("grid_trading")) {
        auto grid = std::make_shared<strategy::GridTradingStrategy>(http_client_);
        strategy_manager_->registerStrategy(grid);
    }
    
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
    if (risk_manager_) {
        auto metrics = risk_manager_->getRiskMetrics();
        LOG_INFO("Final Balance: {}", metrics.total_capital);
    } else {
        LOG_INFO("Final Balance: {}", balance_krw_ + (balance_asset_ * history_data_.back().close));
    }
}

void BacktestEngine::processCandle(const Candle& candle) {
    // 1. Accumulate History
    current_candles_.push_back(candle);
    if (current_candles_.size() > 200) {
        current_candles_.erase(current_candles_.begin()); // Keep window size
    }
    
    // Let each strategy enforce its own warm-up requirement.
    // A global 100-candle gate prevents short fixture datasets from producing any signal.
    if (current_candles_.size() < 30) return;

    double current_price = candle.close;
    
    // 2. Market/Regime Analysis
    auto regime = regime_detector_->analyzeRegime(current_candles_);
    analytics::CoinMetrics metrics;
    metrics.market = market_name_;
    metrics.candles = current_candles_;
    metrics.current_price = current_price;
    metrics.volatility = regime.atr_pct;
    
    // Calculate price_change_rate from previous candle
    if (current_candles_.size() >= 2) {
        double prev_close = current_candles_[current_candles_.size() - 2].close;
        if (prev_close > 0) {
            metrics.price_change_rate = ((current_price - prev_close) / prev_close) * 100.0;
        }
    }
    
    // Estimate volume surge and rolling average volume
    double avg_vol = 0.0;
    if (current_candles_.size() >= 20) {
        for (size_t vi = current_candles_.size() - 20; vi < current_candles_.size() - 1; ++vi) {
            avg_vol += current_candles_[vi].volume;
        }
        avg_vol /= 19.0;
        metrics.volume_surge_ratio = (avg_vol > 0) ? (candle.volume / avg_vol) : 1.0;
    } else {
        metrics.volume_surge_ratio = 1.0;
    }
    
    // Dynamic synthetic microstructure from recent candles
    const double candle_range_pct = (current_price > 0.0)
        ? ((candle.high - candle.low) / current_price) * 100.0
        : 0.0;

    double momentum_5 = 0.0;
    if (current_candles_.size() >= 6) {
        const double base = current_candles_[current_candles_.size() - 6].close;
        if (base > 0.0) {
            momentum_5 = ((current_price - base) / base) * 100.0;
        }
    }

    // Liquidity drops when range is wide and volume baseline is weak
    const double vol_baseline_t = std::clamp((avg_vol - 10.0) / 120.0, 0.0, 1.0);
    const double spread_stress_t = std::clamp((candle_range_pct - 0.15) / 1.5, 0.0, 1.0);
    const double surge_bonus = std::clamp((metrics.volume_surge_ratio - 1.0) * 8.0, -10.0, 15.0);
    metrics.liquidity_score = std::clamp(
        30.0 + (vol_baseline_t * 55.0) - (spread_stress_t * 25.0) + surge_bonus,
        10.0, 95.0
    );

    const double momentum_t = std::clamp(momentum_5 / 2.5, -1.0, 1.0);
    const double surge_t = std::clamp((metrics.volume_surge_ratio - 1.0) / 2.0, -1.0, 1.0);
    metrics.order_book_imbalance = std::clamp((momentum_t * 0.45) + (surge_t * 0.25), -0.7, 0.7);
    metrics.buy_pressure = std::clamp(50.0 + (metrics.order_book_imbalance * 35.0), 10.0, 90.0);
    metrics.sell_pressure = std::clamp(100.0 - metrics.buy_pressure, 10.0, 90.0);
    metrics.price_momentum = momentum_5;
    
    // Generate synthetic orderbook_units (5 levels), with spread tied to regime/liquidity
    {
        nlohmann::json units = nlohmann::json::array();
        const double spread_pct = std::clamp(
            0.01 + (metrics.volatility * 0.20) + ((100.0 - metrics.liquidity_score) * 0.0015),
            0.01, 0.40
        );
        const double base_size = std::clamp(avg_vol / 80.0, 0.2, 3.0);
        
        for (int level = 0; level < 5; ++level) {
            double offset = spread_pct * (level + 1) / 100.0;
            double bid_price = current_price * (1.0 - offset);
            double ask_price = current_price * (1.0 + offset);
            
            const double level_bias = (5.0 - static_cast<double>(level)) / 5.0;
            const double flow_bias = metrics.order_book_imbalance * 0.5;
            const double wave = std::sin((candle.timestamp + level * 17) * 0.001) * 0.08;
            const double bid_size = base_size * (1.0 + level_bias * 0.25 + flow_bias + wave);
            const double ask_size = base_size * (1.0 + level_bias * 0.20 - flow_bias - wave);
            
            nlohmann::json unit;
            unit["ask_price"] = ask_price;
            unit["bid_price"] = bid_price;
            unit["ask_size"] = std::max(0.05, ask_size);
            unit["bid_size"] = std::max(0.05, bid_size);
            units.push_back(unit);
        }
        metrics.orderbook_units = units;
    }

    // 3. Monitor & Exit Positions
    // In TradingEngine this is done by iterating active positions
    // Here we check if we have an open position in RiskManager
    auto position = risk_manager_->getPosition(market_name_);
    if (position) {
        risk_manager_->updatePosition(market_name_, current_price);
        position = risk_manager_->getPosition(market_name_);
    }
    if (risk_manager_->isDrawdownExceeded()) {
        if (position) {
            const double forced_exit = current_price * (1.0 - EXIT_SLIPPAGE_PCT);
            Order dd_order;
            dd_order.market = market_name_;
            dd_order.side = OrderSide::SELL;
            dd_order.volume = position->quantity;
            dd_order.price = forced_exit;
            dd_order.strategy_name = position->strategy_name;
            executeOrder(dd_order, forced_exit);
            risk_manager_->exitPosition(market_name_, forced_exit, "MaxDrawdown");
            position = nullptr;
        }
    }

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
            // Conservative intrabar execution model:
            // - If stop and take-profit touched in same candle, stop is prioritized.
            // - TP1 is partial exit, TP2 is full exit.
            const bool stop_touched = (candle.low <= position->stop_loss);
            const bool tp1_touched = (candle.high >= position->take_profit_1);
            const bool tp2_touched = (candle.high >= position->take_profit_2);

            if (stop_touched) {
                const double stop_fill = position->stop_loss * (1.0 - STOP_SLIPPAGE_PCT);
                Order sl_order;
                sl_order.market = market_name_;
                sl_order.side = OrderSide::SELL;
                sl_order.volume = position->quantity;
                sl_order.price = stop_fill;
                sl_order.strategy_name = position->strategy_name;
                executeOrder(sl_order, stop_fill);
                risk_manager_->exitPosition(market_name_, stop_fill, "StopLoss");
            } else {
                if (!position->half_closed && tp1_touched) {
                    const double tp1_fill = position->take_profit_1 * (1.0 - EXIT_SLIPPAGE_PCT);
                    const double partial_qty = position->quantity * 0.5;

                    Order tp1_order;
                    tp1_order.market = market_name_;
                    tp1_order.side = OrderSide::SELL;
                    tp1_order.volume = partial_qty;
                    tp1_order.price = tp1_fill;
                    tp1_order.strategy_name = position->strategy_name;
                    executeOrder(tp1_order, tp1_fill);
                    risk_manager_->partialExit(market_name_, tp1_fill);
                    position = risk_manager_->getPosition(market_name_);
                }

                if (position) {
                    const bool tp2_touched_after_partial = tp2_touched || (candle.high >= position->take_profit_2);
                    if (tp2_touched_after_partial) {
                        const double tp2_fill = position->take_profit_2 * (1.0 - EXIT_SLIPPAGE_PCT);
                        Order tp2_order;
                        tp2_order.market = market_name_;
                        tp2_order.side = OrderSide::SELL;
                        tp2_order.volume = position->quantity;
                        tp2_order.price = tp2_fill;
                        tp2_order.strategy_name = position->strategy_name;
                        executeOrder(tp2_order, tp2_fill);
                        risk_manager_->exitPosition(market_name_, tp2_fill, "TakeProfit2");
                    } else if (should_exit) {
                        // Execute Sell
                        Order order;
                        order.market = market_name_;
                        order.side = OrderSide::SELL;
                        order.volume = position->quantity;
                        order.price = current_price * (1.0 - EXIT_SLIPPAGE_PCT);
                        order.strategy_name = position->strategy_name;
                        executeOrder(order, order.price);
                        risk_manager_->exitPosition(market_name_, order.price, "StrategyExit");
                    }
                }
            }
        }
    }
    
    // 4. Generate Entry Signals (only if no position)
    if (!position) {
        bool entry_executed = false;
        auto signals = strategy_manager_->collectSignals(
            market_name_,
            metrics,
            current_candles_,
            current_price,
            risk_manager_->getRiskMetrics().available_capital,
            regime
        );
        
        // Dynamic Filter Simulation (Self-learning stub)
        // If we had recent losses, filter might increase.
        double filter_threshold = dynamic_filter_value_;
        const auto bt_metrics = risk_manager_->getRiskMetrics();
        const bool small_seed_mode =
            bt_metrics.total_capital > 0.0 &&
            bt_metrics.total_capital <= engine_config_.small_account_tier2_capital_krw;
        if (small_seed_mode) {
            filter_threshold = std::clamp(filter_threshold + 0.08, 0.35, 0.90);
        }
        
        double min_expected_value = 0.0;
        if (regime.regime == analytics::MarketRegime::HIGH_VOLATILITY) {
            min_expected_value = 0.0003;
            filter_threshold = std::max(filter_threshold, 0.48);
        } else if (regime.regime == analytics::MarketRegime::TRENDING_DOWN) {
            min_expected_value = 0.0005;
            filter_threshold = std::max(filter_threshold, 0.52);
        } else if (regime.regime == analytics::MarketRegime::RANGING) {
            min_expected_value = 0.0001;
            filter_threshold = std::max(filter_threshold, 0.43);
        }

        // Regime-aware minimum activation:
        // when there are no entries for a long time, ease thresholds slightly only in
        // non-bear regimes to avoid staying fully idle.
        if (no_entry_streak_candles_ >= 45 &&
            regime.regime == analytics::MarketRegime::TRENDING_UP &&
            metrics.liquidity_score >= 55.0 &&
            metrics.volume_surge_ratio >= 1.2 &&
            metrics.price_change_rate >= 0.10) {
            filter_threshold = std::max(0.34, filter_threshold - 0.07);
            min_expected_value = std::max(0.0, min_expected_value - 0.00005);
        } else if (no_entry_streak_candles_ >= 70 &&
                   regime.regime == analytics::MarketRegime::RANGING &&
                   metrics.liquidity_score >= 50.0) {
            filter_threshold = std::max(0.35, filter_threshold - 0.05);
            min_expected_value = std::max(0.0, min_expected_value - 0.00003);
        }

        auto best_signal = strategy_manager_->selectRobustSignal(
            strategy_manager_->filterSignals(signals, filter_threshold, min_expected_value, regime.regime),
            regime.regime
        );

        if (best_signal.type != strategy::SignalType::NONE) {
            best_signal.market_regime = regime.regime;
            const auto strategy_edge = buildStrategyEdgeStats(risk_manager_->getTradeHistory());
            bool strategy_ev_ok = true;
            double adaptive_rr_add = 0.0;
            double adaptive_edge_add = 0.0;
            auto stat_it = strategy_edge.find(best_signal.strategy_name);
            if (stat_it != strategy_edge.end()) {
                const auto& stat = stat_it->second;
                if (stat.trades >= engine_config_.min_strategy_trades_for_ev) {
                    strategy_ev_ok =
                        (stat.expectancy() >= engine_config_.min_strategy_expectancy_krw) &&
                        (stat.profitFactor() >= engine_config_.min_strategy_profit_factor);
                }
                if (stat.trades >= 8) {
                    const double wr = stat.winRate();
                    const double pf = stat.profitFactor();
                    const double exp_krw = stat.expectancy();
                    if (wr < 0.42) {
                        adaptive_rr_add += 0.35;
                        adaptive_edge_add += 0.0006;
                    } else if (wr < 0.48) {
                        adaptive_rr_add += 0.20;
                        adaptive_edge_add += 0.0003;
                    }
                    if (pf < 0.90) {
                        adaptive_rr_add += 0.25;
                        adaptive_edge_add += 0.0005;
                    } else if (pf < 1.00) {
                        adaptive_rr_add += 0.12;
                        adaptive_edge_add += 0.0002;
                    }
                    if (exp_krw < 0.0) {
                        adaptive_rr_add += std::clamp((-exp_krw) / 1000.0, 0.0, 0.25);
                        adaptive_edge_add += std::clamp((-exp_krw) / 80000.0, 0.0, 0.0004);
                    }
                }
            }

            engine::EngineConfig tuned_cfg = engine_config_;
            if (small_seed_mode) {
                tuned_cfg.min_reward_risk = engine_config_.min_reward_risk + 0.35;
                tuned_cfg.min_expected_edge_pct = engine_config_.min_expected_edge_pct * 1.8;
            }
            tuned_cfg.min_reward_risk = std::clamp(
                tuned_cfg.min_reward_risk + adaptive_rr_add,
                tuned_cfg.min_reward_risk,
                tuned_cfg.min_reward_risk + 0.9
            );
            tuned_cfg.min_expected_edge_pct = std::clamp(
                tuned_cfg.min_expected_edge_pct + adaptive_edge_add,
                tuned_cfg.min_expected_edge_pct,
                tuned_cfg.min_expected_edge_pct + 0.002
            );

            normalizeSignalStopLossByRegime(best_signal, best_signal.market_regime);
            if (strategy_ev_ok &&
                rebalanceSignalRiskReward(best_signal, tuned_cfg) &&
                passesRegimeGate(best_signal.market_regime, engine_config_) &&
                passesEntryQualityGate(best_signal, tuned_cfg)) {
                // Validate Risk
                if (risk_manager_->canEnterPosition(
                    market_name_,
                    current_price * (1.0 + ENTRY_SLIPPAGE_PCT),
                    best_signal.position_size,
                    best_signal.strategy_name
                )) {
                    // Execute Buy
                    double available_cash = risk_manager_->getRiskMetrics().available_capital;
                    double fill_price = current_price * (1.0 + ENTRY_SLIPPAGE_PCT);
                    const double fee_rate = Config::getInstance().getFeeRate();
                    const double min_order_krw = std::max(5000.0, engine_config_.min_order_krw);
                    const double fee_reserve = std::clamp(engine_config_.order_fee_reserve_pct, 0.0, 0.02);
                    const double spendable_capital = available_cash / (1.0 + fee_reserve);

                    if (spendable_capital >= min_order_krw) {
                        double desired_order_krw = available_cash * std::clamp(best_signal.position_size, 0.0, 1.0);
                        double max_order_krw = std::min(engine_config_.max_order_krw, spendable_capital);
                        if (available_cash <= engine_config_.small_account_tier1_capital_krw) {
                            const double tier_cap = std::clamp(engine_config_.small_account_tier1_max_order_pct, 0.01, 1.0);
                            max_order_krw = std::min(max_order_krw, std::max(min_order_krw, available_cash * tier_cap));
                        } else if (available_cash <= engine_config_.small_account_tier2_capital_krw) {
                            const double tier_cap = std::clamp(engine_config_.small_account_tier2_max_order_pct, 0.01, 1.0);
                            max_order_krw = std::min(max_order_krw, std::max(min_order_krw, available_cash * tier_cap));
                        }

                        const int max_lots = std::max(1, static_cast<int>(std::floor(max_order_krw / min_order_krw)));
                        int desired_lots = static_cast<int>(std::floor(desired_order_krw / min_order_krw));
                        desired_lots = std::clamp(desired_lots, 1, max_lots);
                        const double order_amount = static_cast<double>(desired_lots) * min_order_krw;
                        const double quantity = order_amount / (fill_price * (1.0 + fee_rate));
                        const double fee = quantity * fill_price * fee_rate;

                        if (quantity > 0.0 && available_cash >= (quantity * fill_price) + fee) {
                        Order order;
                        order.market = market_name_;
                        order.side = OrderSide::BUY;
                        order.price = fill_price;
                        order.volume = quantity;
                        order.strategy_name = best_signal.strategy_name;
                        
                        executeOrder(order, fill_price);
                        
                        // Register with Risk Manager
                        risk_manager_->enterPosition(
                            market_name_,
                            fill_price,
                            quantity,
                            best_signal.stop_loss,
                            best_signal.take_profit_1,
                            best_signal.take_profit_2,
                            best_signal.strategy_name
                        );
                        const double reward_pct = (best_signal.take_profit_2 - best_signal.entry_price) / std::max(1e-9, best_signal.entry_price);
                        const double risk_pct = (best_signal.entry_price - best_signal.stop_loss) / std::max(1e-9, best_signal.entry_price);
                        const double rr = (risk_pct > 1e-9) ? (reward_pct / risk_pct) : 0.0;
                        const double round_trip_cost = (Config::getInstance().getFeeRate() * 2.0) + (engine_config_.max_slippage_pct * 2.0);
                        const double net_edge = reward_pct - round_trip_cost;
                        risk_manager_->setPositionSignalInfo(
                            market_name_,
                            best_signal.signal_filter,
                            best_signal.strength,
                            best_signal.market_regime,
                            best_signal.liquidity_score,
                            best_signal.volatility,
                            (best_signal.expected_value != 0.0 ? best_signal.expected_value : net_edge),
                            rr
                        );
                        entry_executed = true;
                        }
                    }
                }
            }
        }

        if (entry_executed) {
            no_entry_streak_candles_ = 0;
        } else {
            no_entry_streak_candles_++;
        }
    }

    // Execute pending order lifecycle transitions for this candle.
    checkOrders(candle);

    // 5. Update Portfolio Value for Drawdown calculation
    double current_equity = risk_manager_->getRiskMetrics().total_capital;
    
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
    (void)candle;
    if (pending_orders_.empty()) {
        return;
    }

    const double fee_rate = Config::getInstance().getFeeRate();
    for (const auto& pending : pending_orders_) {
        const auto transitioned = core::execution::OrderLifecycleStateMachine::transition(
            "filled",
            0.0,
            pending.order.volume,
            pending.order.volume,
            0.0
        );
        const double trade_amount = pending.requested_price * transitioned.filled_volume;
        const double fee = trade_amount * fee_rate;
        (void)fee;

        LOG_INFO(
            "Execution lifecycle: source={}, event={}, order_id={}, market={}, side={}, status={}, filled={:.8f}, volume={:.8f}, terminal={}",
            "backtest",
            "filled",
            pending.order.order_id,
            pending.order.market,
            core::execution::orderSideToString(pending.order.side),
            core::execution::orderStatusToString(transitioned.status),
            transitioned.filled_volume,
            pending.order.volume,
            transitioned.terminal ? "true" : "false"
        );
        appendExecutionUpdateArtifact(
            core::execution::makeExecutionUpdate(
                "backtest",
                "filled",
                pending.order.order_id,
                pending.order.market,
                pending.order.side,
                transitioned.status,
                transitioned.filled_volume,
                pending.order.volume,
                pending.requested_price,
                pending.order.strategy_name,
                transitioned.terminal,
                getCurrentTimeMs()
            )
        );

        if (transitioned.terminal &&
            (pending.order.side == OrderSide::BUY || pending.order.side == OrderSide::SELL)) {
            total_trades_++;
        }
    }

    pending_orders_.clear();
}

void BacktestEngine::executeOrder(const Order& order, double price) {
    Order queued = order;
    if (queued.order_id.empty()) {
        queued.order_id = "bt-" + std::to_string(++backtest_order_seq_);
    }

    const auto submitted = core::execution::OrderLifecycleStateMachine::transition(
        "submitted",
        0.0,
        queued.volume,
        0.0,
        queued.volume
    );
    LOG_INFO(
        "Execution lifecycle: source={}, event={}, order_id={}, market={}, side={}, status={}, filled={:.8f}, volume={:.8f}, terminal={}",
        "backtest",
        "submitted",
        queued.order_id,
        queued.market,
        core::execution::orderSideToString(queued.side),
        core::execution::orderStatusToString(submitted.status),
        submitted.filled_volume,
        queued.volume,
        submitted.terminal ? "true" : "false"
    );
    appendExecutionUpdateArtifact(
        core::execution::makeExecutionUpdate(
            "backtest",
            "submitted",
            queued.order_id,
            queued.market,
            queued.side,
            submitted.status,
            submitted.filled_volume,
            queued.volume,
            price,
            queued.strategy_name,
            submitted.terminal,
            getCurrentTimeMs()
        )
    );

    PendingBacktestOrder pending;
    pending.order = queued;
    pending.requested_price = price;
    pending.enqueued_at_ms = getCurrentTimeMs();
    pending_orders_.push_back(std::move(pending));
}

BacktestEngine::Result BacktestEngine::getResult() const {
    Result result{};
    double final_equity = balance_krw_;
    if (risk_manager_) {
        final_equity = risk_manager_->getRiskMetrics().total_capital;
    } else if (!history_data_.empty()) {
        final_equity += balance_asset_ * history_data_.back().close;
    }

    int closed_trades = 0;
    int wins = 0;
    int losses = 0;
    double gross_profit = 0.0;
    double gross_loss_abs = 0.0;
    std::map<std::string, Result::StrategySummary> strategy_map;
    if (risk_manager_) {
        const auto history = risk_manager_->getTradeHistory();
        closed_trades = static_cast<int>(history.size());
        for (const auto& trade : history) {
            const std::string strategy_name = trade.strategy_name.empty() ? "unknown" : trade.strategy_name;
            auto& ss = strategy_map[strategy_name];
            ss.strategy_name = strategy_name;
            ss.total_trades++;
            ss.total_profit += trade.profit_loss;

            if (trade.profit_loss > 0.0) {
                ++wins;
                gross_profit += trade.profit_loss;
                ss.winning_trades++;
                ss.avg_win_krw += trade.profit_loss; // temp as gross win accumulator
            } else if (trade.profit_loss < 0.0) {
                ++losses;
                gross_loss_abs += std::abs(trade.profit_loss);
                ss.losing_trades++;
                ss.avg_loss_krw += std::abs(trade.profit_loss); // temp as gross loss accumulator
            }
        }

        for (auto& [_, ss] : strategy_map) {
            ss.win_rate = (ss.total_trades > 0)
                ? (static_cast<double>(ss.winning_trades) / static_cast<double>(ss.total_trades))
                : 0.0;
            const double gross_win = ss.avg_win_krw;
            const double gross_loss = ss.avg_loss_krw;
            ss.avg_win_krw = (ss.winning_trades > 0) ? (gross_win / static_cast<double>(ss.winning_trades)) : 0.0;
            ss.avg_loss_krw = (ss.losing_trades > 0) ? (gross_loss / static_cast<double>(ss.losing_trades)) : 0.0;
            ss.profit_factor = (gross_loss > 1e-12) ? (gross_win / gross_loss) : 0.0;
            result.strategy_summaries.push_back(ss);
        }
        std::sort(result.strategy_summaries.begin(), result.strategy_summaries.end(),
            [](const Result::StrategySummary& a, const Result::StrategySummary& b) {
                return a.total_profit > b.total_profit;
            });
    } else {
        closed_trades = total_trades_;
        wins = winning_trades_;
        losses = std::max(0, closed_trades - wins);
    }

    const double win_rate = (closed_trades > 0)
        ? static_cast<double>(wins) / static_cast<double>(closed_trades)
        : 0.0;
    const double avg_win_krw = (wins > 0) ? (gross_profit / static_cast<double>(wins)) : 0.0;
    const double avg_loss_krw = (losses > 0) ? (gross_loss_abs / static_cast<double>(losses)) : 0.0;
    const double profit_factor = (gross_loss_abs > 1e-12) ? (gross_profit / gross_loss_abs) : 0.0;
    const double expectancy_krw = (closed_trades > 0)
        ? ((gross_profit - gross_loss_abs) / static_cast<double>(closed_trades))
        : 0.0;

    result.final_balance = final_equity;
    result.total_profit = final_equity - Config::getInstance().getInitialCapital();
    result.max_drawdown = max_drawdown_;
    result.total_trades = closed_trades;
    result.winning_trades = wins;
    result.losing_trades = losses;
    result.win_rate = win_rate;
    result.avg_win_krw = risk_manager_ ? avg_win_krw : 0.0;
    result.avg_loss_krw = risk_manager_ ? avg_loss_krw : 0.0;
    result.profit_factor = risk_manager_ ? profit_factor : 0.0;
    result.expectancy_krw = risk_manager_ ? expectancy_krw : 0.0;
    return result;
}



void BacktestEngine::updateDynamicFilter() {
    auto history = risk_manager_->getTradeHistory();
    if (history.size() < 20) {
        return;
    }

    const size_t sample_n = std::min<size_t>(60, history.size());
    double gross_profit = 0.0;
    double gross_loss_abs = 0.0;
    double sum_pnl = 0.0;
    int wins = 0;

    size_t seen = 0;
    for (auto it = history.rbegin(); it != history.rend() && seen < sample_n; ++it, ++seen) {
        const double pnl = it->profit_loss;
        sum_pnl += pnl;
        if (pnl > 0.0) {
            gross_profit += pnl;
            ++wins;
        } else if (pnl < 0.0) {
            gross_loss_abs += std::abs(pnl);
        }
    }

    const double expectancy = sum_pnl / static_cast<double>(sample_n);
    const double profit_factor = (gross_loss_abs > 1e-12) ? (gross_profit / gross_loss_abs) : 0.0;
    const double win_rate = static_cast<double>(wins) / static_cast<double>(sample_n);

    if (expectancy < 0.0 || profit_factor < 1.0) {
        dynamic_filter_value_ = std::min(0.70, dynamic_filter_value_ + 0.01);
    } else if (expectancy > 0.0 && profit_factor > 1.15 && win_rate >= 0.50) {
        dynamic_filter_value_ = std::max(0.35, dynamic_filter_value_ - 0.01);
    } else {
        if (dynamic_filter_value_ > 0.50) {
            dynamic_filter_value_ = std::max(0.35, dynamic_filter_value_ - 0.003);
        } else if (dynamic_filter_value_ < 0.50) {
            dynamic_filter_value_ = std::min(0.70, dynamic_filter_value_ + 0.003);
        }
    }
}

} // namespace backtest
} // namespace autolife
