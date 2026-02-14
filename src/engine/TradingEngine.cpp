#include "engine/TradingEngine.h"
#include "common/Logger.h"
#include "common/Config.h"
#include "strategy/ScalpingStrategy.h"
#include "strategy/MomentumStrategy.h" 
#include "strategy/BreakoutStrategy.h"
#include "strategy/MeanReversionStrategy.h"
#include "strategy/GridTradingStrategy.h"
#include "analytics/TechnicalIndicators.h"
#include "analytics/OrderbookAnalyzer.h"
#include "core/adapters/LegacyExecutionPlaneAdapter.h"
#include "core/adapters/LegacyPolicyLearningPlaneAdapter.h"
#include "core/adapters/UpbitComplianceAdapter.h"
#include "core/state/EventJournalJsonl.h"
#include "core/state/LearningStateStoreJson.h"
#include "risk/RiskManager.h"
#include "common/PathUtils.h"
#include "common/TickSizeHelper.h"  // [Phase 3] ???????????? ????????⑤벡苑????遺얘턁????????????
#include <chrono>
#include <iostream>
#include <thread>
#include <algorithm>
#include <cctype>

#undef max
#undef min
#include <sstream>
#include <iomanip>
#include <map>
#include <cmath>
#include <set>
#include <functional>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace {
long long getCurrentTimestampMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

const char* regimeToString(autolife::analytics::MarketRegime regime) {
    switch (regime) {
        case autolife::analytics::MarketRegime::TRENDING_UP: return "TRENDING_UP";
        case autolife::analytics::MarketRegime::TRENDING_DOWN: return "TRENDING_DOWN";
        case autolife::analytics::MarketRegime::RANGING: return "RANGING";
        case autolife::analytics::MarketRegime::HIGH_VOLATILITY: return "HIGH_VOLATILITY";
        default: return "UNKNOWN";
    }
}

double computeTargetRewardRisk(double strength, const autolife::engine::EngineConfig& cfg) {
    const double weak_rr = std::max(0.5, cfg.min_rr_weak_signal);
    const double strong_rr = std::max(0.5, std::min(cfg.min_rr_strong_signal, weak_rr));
    const double t = std::clamp((strength - 0.40) / 0.60, 0.0, 1.0);
    return weak_rr - (weak_rr - strong_rr) * t;
}

bool rebalanceSignalRiskReward(autolife::strategy::Signal& signal, const autolife::engine::EngineConfig& cfg) {
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
        if (gross_loss_abs > 1e-12) {
            return gross_profit / gross_loss_abs;
        }
        return (gross_profit > 1e-12) ? 99.9 : 0.0;
    }
};

std::string makeStrategyRegimeKey(const std::string& strategy_name, autolife::analytics::MarketRegime regime) {
    return strategy_name + "|" + std::to_string(static_cast<int>(regime));
}

std::string makeMarketStrategyRegimeKey(
    const std::string& market,
    const std::string& strategy_name,
    autolife::analytics::MarketRegime regime
) {
    return market + "|" + strategy_name + "|" + std::to_string(static_cast<int>(regime));
}

std::map<std::string, StrategyEdgeStats> buildStrategyRegimeEdgeStats(
    const std::vector<autolife::risk::TradeHistory>& history
) {
    std::map<std::string, StrategyEdgeStats> out;
    for (const auto& trade : history) {
        if (trade.strategy_name.empty()) {
            continue;
        }
        const std::string key = makeStrategyRegimeKey(trade.strategy_name, trade.market_regime);
        auto& s = out[key];
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

std::map<std::string, StrategyEdgeStats> buildMarketStrategyRegimeEdgeStats(
    const std::vector<autolife::risk::TradeHistory>& history
) {
    std::map<std::string, StrategyEdgeStats> out;
    for (const auto& trade : history) {
        if (trade.strategy_name.empty() || trade.market.empty()) {
            continue;
        }
        const std::string key = makeMarketStrategyRegimeKey(
            trade.market, trade.strategy_name, trade.market_regime
        );
        auto& s = out[key];
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

bool isLossFocusMarket(const std::string& market) {
    std::string lower = market;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower == "krw-ada" ||
           lower == "krw-avax" ||
           lower == "krw-xrp" ||
           lower == "krw-sui" ||
           lower == "krw-dot";
}

void normalizeSignalStopLossByRegime(autolife::strategy::Signal& signal, autolife::analytics::MarketRegime regime) {
    if (signal.entry_price <= 0.0) {
        return;
    }

    double min_risk_pct = 0.0035;
    double max_risk_pct = 0.0100;
    if (regime == autolife::analytics::MarketRegime::HIGH_VOLATILITY) {
        min_risk_pct = 0.0060;
        max_risk_pct = 0.0150;
    } else if (regime == autolife::analytics::MarketRegime::TRENDING_DOWN) {
        min_risk_pct = 0.0050;
        max_risk_pct = 0.0120;
    }

    const double strength_t = std::clamp((signal.strength - 0.40) / 0.60, 0.0, 1.0);
    const double tighten = 1.0 - (0.15 * strength_t); // stronger signal -> slightly tighter stop
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

std::string toLowerCopy(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

void appendPolicyDecisionAudit(
    const std::vector<autolife::engine::PolicyDecisionRecord>& decisions,
    autolife::analytics::MarketRegime dominant_regime,
    bool small_seed_mode,
    int max_new_orders_per_scan
) {
    if (decisions.empty()) {
        return;
    }

    nlohmann::json line;
    line["ts"] = getCurrentTimestampMs();
    line["dominant_regime"] = regimeToString(dominant_regime);
    line["small_seed_mode"] = small_seed_mode;
    line["max_new_orders_per_scan"] = max_new_orders_per_scan;
    line["decisions"] = nlohmann::json::array();

    for (const auto& d : decisions) {
        nlohmann::json item;
        item["market"] = d.market;
        item["strategy"] = d.strategy_name;
        item["selected"] = d.selected;
        item["reason"] = d.reason;
        item["base_score"] = d.base_score;
        item["policy_score"] = d.policy_score;
        item["strength"] = d.strength;
        item["expected_value"] = d.expected_value;
        item["liquidity"] = d.liquidity_score;
        item["volatility"] = d.volatility;
        item["trades"] = d.strategy_trades;
        item["win_rate"] = d.strategy_win_rate;
        item["profit_factor"] = d.strategy_profit_factor;
        item["used_preloaded_tf_5m"] = d.used_preloaded_tf_5m;
        item["used_preloaded_tf_1h"] = d.used_preloaded_tf_1h;
        item["used_resampled_tf_fallback"] = d.used_resampled_tf_fallback;
        line["decisions"].push_back(std::move(item));
    }

    auto path = autolife::utils::PathUtils::resolveRelativePath("logs/policy_decisions.jsonl");
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path.string(), std::ios::app);
    out << line.dump() << "\n";
}

}
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "Ws2_32.lib")

// Using declarations for risk namespace types
using autolife::risk::TradeHistory;
using autolife::risk::Position;

namespace autolife {
namespace engine {

TradingEngine::TradingEngine(
    const EngineConfig& config,
    std::shared_ptr<network::UpbitHttpClient> http_client
)
    : config_(config)
    , http_client_(http_client)
    , running_(false)
    , start_time_(0)
    , total_scans_(0)
    , total_signals_(0)
{
    LOG_INFO("TradingEngine initialized");
    LOG_INFO("Trading mode: {}", config.mode == TradingMode::LIVE ? "LIVE" : "PAPER");
    LOG_INFO("Initial capital: {:.0f} KRW", config.initial_capital);

    if (config.mode == TradingMode::LIVE) {
        LOG_INFO("Live risk and order constraints enabled");
        LOG_INFO("  Max daily loss (KRW): {:.0f}", config.max_daily_loss_krw);
        LOG_INFO("  Max order amount (KRW): {:.0f}", config.max_order_krw);
        LOG_INFO("  Min order amount (KRW): {:.0f}", config.min_order_krw);
        LOG_INFO("  Dry Run: {}", config.dry_run ? "ON" : "OFF");
    }

    scanner_ = std::make_unique<analytics::MarketScanner>(http_client);
    strategy_manager_ = std::make_unique<strategy::StrategyManager>(http_client);
    policy_controller_ = std::make_unique<AdaptivePolicyController>();
    performance_store_ = std::make_unique<PerformanceStore>();
    risk_manager_ = std::make_unique<risk::RiskManager>(config.initial_capital);
    order_manager_ = std::make_unique<execution::OrderManager>(
        http_client,
        config.mode == TradingMode::LIVE
    );
    regime_detector_ = std::make_unique<analytics::RegimeDetector>();
    learning_state_store_ = std::make_unique<core::LearningStateStoreJson>(
        utils::PathUtils::resolveRelativePath("state/learning_state.json")
    );
    event_journal_ = std::make_unique<core::EventJournalJsonl>(
        utils::PathUtils::resolveRelativePath("state/event_journal.jsonl")
    );

    if (config_.enable_core_plane_bridge) {
        if (config_.enable_core_policy_plane && policy_controller_) {
            core_policy_plane_ = std::make_shared<core::LegacyPolicyLearningPlaneAdapter>(
                *policy_controller_,
                performance_store_.get()
            );
        }
        if (config_.enable_core_risk_plane && risk_manager_) {
            core_risk_plane_ = std::make_shared<core::UpbitComplianceAdapter>(
                http_client_,
                *risk_manager_,
                config_
            );
        }
        if (config_.enable_core_execution_plane && order_manager_) {
            core_execution_plane_ = std::make_shared<core::LegacyExecutionPlaneAdapter>(
                *order_manager_
            );
        }
        core_cycle_ = std::make_unique<core::TradingCycleCoordinator>(
            core_policy_plane_,
            core_risk_plane_,
            core_execution_plane_
        );

        LOG_INFO(
            "Core plane bridge enabled (policy={}, risk={}, execution={})",
            config_.enable_core_policy_plane ? "on" : "off",
            config_.enable_core_risk_plane ? "on" : "off",
            config_.enable_core_execution_plane ? "on" : "off"
        );
    }

    risk_manager_->setMaxPositions(config.max_positions);
    risk_manager_->setMaxDailyTrades(config.max_daily_trades);
    risk_manager_->setMaxDrawdown(config.max_drawdown);
    risk_manager_->setMaxExposurePct(config.max_exposure_pct);
    risk_manager_->setDailyLossLimitPct(config.max_daily_loss_pct);
    risk_manager_->setDailyLossLimitKrw(config.max_daily_loss_krw);
    risk_manager_->setMinOrderKrw(config.min_order_krw);

    std::set<std::string> enabled;
    for (const auto& s : config_.enabled_strategies) {
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
        auto scalping = std::make_shared<strategy::ScalpingStrategy>(http_client);
        strategy_manager_->registerStrategy(scalping);
        LOG_INFO("Registered strategy: scalping");
    }

    if (should_register("momentum")) {
        auto momentum = std::make_shared<strategy::MomentumStrategy>(http_client);
        strategy_manager_->registerStrategy(momentum);
        LOG_INFO("Registered strategy: momentum");
    }

    if (should_register("breakout")) {
        auto breakout = std::make_shared<strategy::BreakoutStrategy>(http_client);
        strategy_manager_->registerStrategy(breakout);
        LOG_INFO("Registered strategy: breakout");
    }

    if (should_register("mean_reversion")) {
        auto mean_rev = std::make_shared<strategy::MeanReversionStrategy>(http_client);
        strategy_manager_->registerStrategy(mean_rev);
        LOG_INFO("Registered strategy: mean_reversion");
    }

    if (should_register("grid_trading")) {
        auto grid = std::make_shared<strategy::GridTradingStrategy>(http_client);
        strategy_manager_->registerStrategy(grid);
        LOG_INFO("Registered strategy: grid_trading");
    }
}

TradingEngine::~TradingEngine() {
    stop();
}

bool TradingEngine::start() {
    if (running_) {
        LOG_WARN("Trading engine is already running");
        return false;
    }

    LOG_INFO("========================================");
    LOG_INFO("AutoLife trading engine start requested");
    LOG_INFO("========================================");

    loadState();
    loadLearningState();
    if (config_.mode == TradingMode::LIVE) {
        syncAccountState();
    }

    running_ = true;
    start_time_ = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    worker_thread_ = std::make_unique<std::thread>(&TradingEngine::run, this);

    prometheus_http_thread_ = std::make_unique<std::thread>(&TradingEngine::runPrometheusHttpServer, this, 8080);
    LOG_INFO("Prometheus HTTP server started (port 8080)");

    state_persist_running_ = true;
    state_persist_thread_ = std::make_unique<std::thread>(&TradingEngine::runStatePersistence, this);
    LOG_INFO("State persistence thread started (every 30 seconds)");

    return true;
}

void TradingEngine::stop() {
    if (!running_) {
        return;
    }

    LOG_INFO("========================================");
    LOG_INFO("Trading engine stop requested");
    LOG_INFO("========================================");

    running_ = false;

    prometheus_server_running_ = false;
    if (prometheus_http_thread_ && prometheus_http_thread_->joinable()) {
        prometheus_http_thread_->join();
    }

    state_persist_running_ = false;
    if (state_persist_thread_ && state_persist_thread_->joinable()) {
        state_persist_thread_->join();
    }

    if (worker_thread_ && worker_thread_->joinable()) {
        worker_thread_->join();
    }

    logPerformance();
    saveState();
}

void TradingEngine::run() {
    LOG_INFO("Trading engine main loop started");

    auto scan_interval = std::chrono::seconds(config_.scan_interval_seconds);
    auto last_scan_time = std::chrono::steady_clock::now() - scan_interval;
    auto monitor_interval = std::chrono::milliseconds(500);
    auto last_account_sync_time = std::chrono::steady_clock::now();
    auto account_sync_interval = std::chrono::seconds(300);

    while (running_) {
        auto tick_start = std::chrono::steady_clock::now();

        try {
            monitorPositions();

            if (order_manager_) {
                order_manager_->monitorOrders();

                auto filled_orders = order_manager_->getFilledOrders();
                for (const auto& order : filled_orders) {
                    LOG_INFO("Order fill event: {} {} {:.8f} @ {:.0f}",
                             order.market, (order.side == OrderSide::BUY ? "BUY" : "SELL"),
                             order.filled_volume, order.price);

                    if (order.side == OrderSide::BUY) {
                        double filled_amount = order.price * order.filled_volume;
                        risk_manager_->releasePendingCapital(filled_amount);

                        risk_manager_->enterPosition(
                            order.market,
                            order.price,
                            order.filled_volume,
                            order.stop_loss,
                            order.take_profit_1,
                            order.take_profit_2 > 0.0 ? order.take_profit_2 : order.take_profit_1,
                            order.strategy_name,
                            order.breakeven_trigger,
                            order.trailing_start
                        );

                        nlohmann::json fill_payload;
                        fill_payload["side"] = "BUY";
                        fill_payload["filled_volume"] = order.filled_volume;
                        fill_payload["avg_price"] = order.price;
                        fill_payload["strategy_name"] = order.strategy_name;
                        fill_payload["stop_loss"] = order.stop_loss;
                        fill_payload["take_profit_1"] = order.take_profit_1;
                        fill_payload["take_profit_2"] = order.take_profit_2;
                        appendJournalEvent(
                            core::JournalEventType::FILL_APPLIED,
                            order.market,
                            order.order_id,
                            fill_payload
                        );

                        nlohmann::json position_payload;
                        position_payload["entry_price"] = order.price;
                        position_payload["quantity"] = order.filled_volume;
                        position_payload["strategy_name"] = order.strategy_name;
                        appendJournalEvent(
                            core::JournalEventType::POSITION_OPENED,
                            order.market,
                            order.order_id,
                            position_payload
                        );
                    }
                }
            }

            auto now = std::chrono::steady_clock::now();
            if (config_.mode == TradingMode::LIVE) {
                if (now - last_account_sync_time >= account_sync_interval) {
                    LOG_INFO("Periodic account sync started");
                    syncAccountState();
                    last_account_sync_time = now;
                }
            }

            auto elapsed_since_scan = std::chrono::duration_cast<std::chrono::seconds>(now - last_scan_time);
            if (elapsed_since_scan >= scan_interval) {
                LOG_INFO("Starting market scan (elapsed {}s)", elapsed_since_scan.count());

                scanMarkets();
                generateSignals();
                learnOptimalFilterValue();
                executeSignals();

                last_scan_time = std::chrono::steady_clock::now();
                updateMetrics();
            }

            auto tick_end = std::chrono::steady_clock::now();
            auto tick_duration = tick_end - tick_start;
            auto sleep_duration = monitor_interval - tick_duration;

            if (sleep_duration.count() > 0) {
                std::this_thread::sleep_for(sleep_duration);
            }
        } catch (const std::exception& e) {
            LOG_ERROR("Trading engine loop error: {}", e.what());
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    LOG_INFO("Trading engine loop ended");
}

void TradingEngine::scanMarkets() {
    LOG_INFO("===== Market Scan Start =====");

    total_scans_++;

    scanned_markets_ = scanner_->scanAllMarkets();
    if (scanned_markets_.empty()) {
        LOG_WARN("No markets scanned");
        return;
    }

    std::sort(scanned_markets_.begin(), scanned_markets_.end(),
        [](const analytics::CoinMetrics& a, const analytics::CoinMetrics& b) {
            return a.composite_score > b.composite_score;
        });

    std::vector<analytics::CoinMetrics> filtered;
    const double MAX_SPREAD_PCT = 0.35;
    for (const auto& coin : scanned_markets_) {
        if (coin.volume_24h < config_.min_volume_krw) {
            continue;
        }
        if (!coin.orderbook_snapshot.valid) {
            continue;
        }
        if (coin.orderbook_snapshot.spread_pct > MAX_SPREAD_PCT) {
            continue;
        }
        if (coin.orderbook_snapshot.best_bid <= 0.0 || coin.orderbook_snapshot.best_ask <= 0.0) {
            continue;
        }
        if (coin.orderbook_snapshot.ask_notional < config_.min_order_krw * 5.0) {
            continue;
        }

        filtered.push_back(coin);
    }

    if (filtered.size() > 20) {
        filtered.resize(20);
    }

    scanned_markets_ = filtered;

    LOG_INFO("Scanned markets: {}", scanned_markets_.size());

    int count = 0;
    for (const auto& coin : scanned_markets_) {
        if (count++ >= 5) {
            break;
        }

        LOG_INFO("  #{} {} - score {:.1f}, 24h vol {:.0f}?듭썝, volatility {:.2f}%",
                 count, coin.market, coin.composite_score,
                 coin.volume_24h / 100000000.0,
                 coin.volatility);
    }
}

void TradingEngine::generateSignals() {
    if (scanned_markets_.empty()) {
        return;
    }

    LOG_INFO("===== Generate Signals =====");

    pending_signals_.clear();

    for (const auto& coin : scanned_markets_) {
        if (risk_manager_->getPosition(coin.market) != nullptr) {
            continue;
        }

        if (order_manager_->hasActiveOrder(coin.market)) {
            continue;
        }

        try {
            const auto& candles = coin.candles;
            if (candles.empty()) {
                LOG_INFO("{} - no candle data, skipping", coin.market);
                continue;
            }

            double current_price = candles.back().close;
            auto regime = regime_detector_->analyzeRegime(candles);

            auto signals = strategy_manager_->collectSignals(
                coin.market,
                coin,
                candles,
                current_price,
                risk_manager_->getRiskMetrics().available_capital,
                regime
            );

            if (signals.empty()) {
                continue;
            }

            double min_strength = 0.40;
            double min_expected_value = 0.0;
            if (regime.regime == analytics::MarketRegime::HIGH_VOLATILITY) {
                min_strength = 0.48;
                min_expected_value = 0.0003;
            } else if (regime.regime == analytics::MarketRegime::TRENDING_DOWN) {
                min_strength = 0.52;
                min_expected_value = 0.0005;
            } else if (regime.regime == analytics::MarketRegime::RANGING) {
                min_strength = 0.43;
                min_expected_value = 0.0001;
            }

            auto filtered = strategy_manager_->filterSignals(
                signals, min_strength, min_expected_value, regime.regime
            );
            if (filtered.empty() &&
                scans_without_new_entry_ >= 6 &&
                regime.regime != analytics::MarketRegime::TRENDING_DOWN &&
                regime.regime != analytics::MarketRegime::HIGH_VOLATILITY) {
                // Entry-frequency floor: relax a little only in non-stress regimes.
                const double relaxed_strength = std::max(0.35, min_strength - 0.04);
                const double relaxed_ev = std::max(0.0, min_expected_value - 0.0001);
                filtered = strategy_manager_->filterSignals(
                    signals, relaxed_strength, relaxed_ev, regime.regime
                );
            }
            if (filtered.empty()) {
                continue;
            }

            auto best_signal = strategy_manager_->selectRobustSignal(filtered, regime.regime);
            if (best_signal.type != strategy::SignalType::NONE) {
                best_signal.market_regime = regime.regime;
                pending_signals_.push_back(best_signal);
                total_signals_++;

                LOG_INFO("Signal selected: {} - {} (strength: {:.2f}, tf5m_preloaded={}, tf1h_preloaded={}, fallback={})",
                         coin.market,
                         best_signal.type == strategy::SignalType::STRONG_BUY ? "STRONG_BUY" : "BUY",
                         best_signal.strength,
                         best_signal.used_preloaded_tf_5m ? "Y" : "N",
                         best_signal.used_preloaded_tf_1h ? "Y" : "N",
                         best_signal.used_resampled_tf_fallback ? "Y" : "N");
            }
        } catch (const std::exception& e) {
            LOG_ERROR("Signal generation failed: {} - {}", coin.market, e.what());
        }
    }

    LOG_INFO("Signal generation complete: {} candidates", pending_signals_.size());
}
void TradingEngine::executeSignals() {
    if (pending_signals_.empty()) {
        return;
    }

    LOG_INFO("===== Execute Signals =====");

    if (risk_manager_->isDailyLossLimitExceeded()) {
        const double loss_pct = risk_manager_->getDailyLossPct();
        LOG_ERROR("Daily loss limit exceeded: {:.2f}% >= {:.2f}% (skip new entries)",
                  loss_pct * 100.0, config_.max_daily_loss_pct * 100.0);
        pending_signals_.clear();
        return;
    }
    if (risk_manager_->isDrawdownExceeded()) {
        LOG_ERROR("Max drawdown exceeded (limit {:.2f}%), skip new entries",
                  config_.max_drawdown * 100.0);
        pending_signals_.clear();
        return;
    }

    const double current_filter = calculateDynamicFilterValue();
    LOG_INFO("Current dynamic signal filter: {:.3f}", current_filter);
    const double starvation_relax = std::min(0.06, scans_without_new_entry_ * 0.005);
    const double effective_filter = std::max(0.35, current_filter - starvation_relax);
    if (starvation_relax > 0.0) {
        LOG_INFO("Adaptive filter relaxation active: streak={}, relax={:.3f}, effective={:.3f}",
                 scans_without_new_entry_, starvation_relax, effective_filter);
    }

    const double current_scale = calculatePositionScaleMultiplier();
    LOG_INFO("Position scale multiplier: {:.2f}", current_scale);

    const auto metrics_snapshot = risk_manager_->getRiskMetrics();
    const bool small_seed_mode =
        metrics_snapshot.total_capital > 0.0 &&
        metrics_snapshot.total_capital <= config_.small_account_tier2_capital_krw;
    const double adaptive_filter_floor =
        std::clamp(effective_filter + (small_seed_mode ? 0.08 : 0.0), 0.35, 0.90);
    const int per_scan_buy_limit = small_seed_mode
        ? 1
        : std::max(1, config_.max_new_orders_per_scan);
    const double min_reward_risk_gate = config_.min_reward_risk + (small_seed_mode ? 0.35 : 0.0);
    const double min_expected_edge_gate =
        config_.min_expected_edge_pct * (small_seed_mode ? 1.8 : 1.0);
    if (small_seed_mode) {
        LOG_INFO("Small-seed mode active: capital {:.0f}, filter {:.3f}, rr>= {:.2f}, edge>= {:.3f}%",
                 metrics_snapshot.total_capital,
                 adaptive_filter_floor,
                 min_reward_risk_gate,
                 min_expected_edge_gate * 100.0);
    }

    std::map<std::string, StrategyEdgeStats> strategy_edge;
    std::map<std::string, StrategyEdgeStats> strategy_regime_edge;
    std::map<std::string, StrategyEdgeStats> market_strategy_regime_edge;
    {
        const auto history = risk_manager_->getTradeHistory();
        if (performance_store_) {
            performance_store_->rebuild(history);
        }
        strategy_regime_edge = buildStrategyRegimeEdgeStats(history);
        market_strategy_regime_edge = buildMarketStrategyRegimeEdgeStats(history);
        for (const auto& trade : history) {
            if (trade.strategy_name.empty()) {
                continue;
            }
            auto& s = strategy_edge[trade.strategy_name];
            s.trades++;
            s.net_profit += trade.profit_loss;
            if (trade.profit_loss > 0.0) {
                s.wins++;
                s.gross_profit += trade.profit_loss;
            } else if (trade.profit_loss < 0.0) {
                s.gross_loss_abs += std::abs(trade.profit_loss);
            }
        }
    }

    analytics::MarketRegime dominant_regime = analytics::MarketRegime::UNKNOWN;
    {
        std::map<analytics::MarketRegime, int> regime_counts;
        for (const auto& coin : scanned_markets_) {
            if (coin.candles.empty()) {
                continue;
            }
            auto r = regime_detector_->analyzeRegime(coin.candles);
            regime_counts[r.regime]++;
        }
        int best_count = -1;
        for (const auto& [r, count] : regime_counts) {
            if (count > best_count) {
                best_count = count;
                dominant_regime = r;
            }
        }
    }

    if (strategy_manager_) {
        strategy_manager_->refreshStrategyStatesFromHistory(
            risk_manager_->getTradeHistory(),
            dominant_regime,
            config_.avoid_high_volatility,
            config_.avoid_trending_down,
            config_.min_strategy_trades_for_ev,
            config_.min_strategy_expectancy_krw,
            config_.min_strategy_profit_factor
        );
    }

    std::vector<strategy::Signal> execution_candidates = pending_signals_;
    if (config_.enable_core_plane_bridge &&
        config_.enable_core_policy_plane &&
        core_cycle_) {
        core::PolicyContext context;
        context.small_seed_mode = small_seed_mode;
        context.max_new_orders_per_scan = per_scan_buy_limit;
        context.dominant_regime = dominant_regime;

        auto decision_batch = core_cycle_->selectPolicyCandidates(pending_signals_, context);
        execution_candidates = std::move(decision_batch.selected_candidates);
        appendPolicyDecisionAudit(
            decision_batch.decisions,
            dominant_regime,
            small_seed_mode,
            per_scan_buy_limit
        );
        if (!decision_batch.decisions.empty()) {
            nlohmann::json payload;
            payload["selected"] = decision_batch.selected_candidates.size();
            payload["decisions"] = decision_batch.decisions.size();
            payload["small_seed_mode"] = small_seed_mode;
            payload["max_new_orders_per_scan"] = per_scan_buy_limit;
            appendJournalEvent(
                core::JournalEventType::POLICY_CHANGED,
                "MULTI",
                "policy_cycle",
                payload
            );
        }
        if (decision_batch.dropped_by_policy > 0) {
            LOG_INFO("Policy plane dropped {} candidate(s)",
                     decision_batch.dropped_by_policy);
        }
    } else if (policy_controller_) {
        PolicyInput policy_input;
        policy_input.candidates = pending_signals_;
        policy_input.small_seed_mode = small_seed_mode;
        policy_input.max_new_orders_per_scan = per_scan_buy_limit;
        policy_input.dominant_regime = dominant_regime;
        if (performance_store_) {
            policy_input.strategy_stats = &performance_store_->byStrategy();
            policy_input.bucket_stats = &performance_store_->byBucket();
        }
        auto policy_output = policy_controller_->selectCandidates(policy_input);
        execution_candidates = std::move(policy_output.selected_candidates);
        appendPolicyDecisionAudit(
            policy_output.decisions,
            dominant_regime,
            small_seed_mode,
            per_scan_buy_limit
        );
        if (!policy_output.decisions.empty()) {
            nlohmann::json payload;
            payload["selected"] = policy_output.selected_candidates.size();
            payload["decisions"] = policy_output.decisions.size();
            payload["small_seed_mode"] = small_seed_mode;
            payload["max_new_orders_per_scan"] = per_scan_buy_limit;
            appendJournalEvent(
                core::JournalEventType::POLICY_CHANGED,
                "MULTI",
                "policy_cycle",
                payload
            );
        }
        if (policy_output.dropped_by_policy > 0) {
            LOG_INFO("AdaptivePolicyController dropped {} candidate(s)",
                     policy_output.dropped_by_policy);
        }
    }

    int executed = 0;
    int filtered_out = 0;
    int executed_buys_this_scan = 0;

    auto getTotalPotentialPositions = [&]() -> size_t {
        const size_t current_positions = static_cast<size_t>(risk_manager_->getRiskMetrics().active_positions);
        const size_t pending_buys = order_manager_ ? order_manager_->getActiveBuyOrderCount() : 0;
        return current_positions + pending_buys;
    };

    size_t total_potential = getTotalPotentialPositions();
    if (total_potential >= config_.max_positions) {
        LOG_WARN("Position limit reached: {} / max {}", total_potential, config_.max_positions);
    }

    for (auto& signal : execution_candidates) {
        if (signal.type != strategy::SignalType::BUY &&
            signal.type != strategy::SignalType::STRONG_BUY) {
            continue;
        }

        if (executed_buys_this_scan >= per_scan_buy_limit) {
            LOG_WARN("Per-scan buy limit reached: {} / {}",
                     executed_buys_this_scan, per_scan_buy_limit);
            filtered_out++;
            continue;
        }

        total_potential = getTotalPotentialPositions();
        if (total_potential >= config_.max_positions) {
            filtered_out++;
            continue;
        }

        double required_signal_strength = adaptive_filter_floor;
        double regime_rr_add = 0.0;
        double regime_edge_add = 0.0;
        bool regime_pattern_block = false;
        auto regime_edge_it = strategy_regime_edge.find(
            makeStrategyRegimeKey(signal.strategy_name, signal.market_regime)
        );
        if (regime_edge_it != strategy_regime_edge.end()) {
            const auto& stat = regime_edge_it->second;
            if (stat.trades >= 6) {
                const double wr = stat.winRate();
                const double pf = stat.profitFactor();
                const double exp_krw = stat.expectancy();

                if (exp_krw < -20.0 || (wr < 0.30 && pf < 0.78)) {
                    regime_pattern_block = true;
                }
                if (stat.trades >= 12 &&
                    exp_krw < -15.0 &&
                    (wr < 0.40 || pf < 0.90)) {
                    required_signal_strength = std::max(required_signal_strength, 0.62);
                    regime_rr_add += 0.20;
                    regime_edge_add += 0.0003;
                }
                if (stat.trades >= 18 &&
                    exp_krw < -22.0 &&
                    wr < 0.20) {
                    regime_pattern_block = true;
                }
                if (exp_krw < 0.0) {
                    required_signal_strength += std::clamp((-exp_krw) / 2500.0, 0.0, 0.08);
                    regime_rr_add += std::clamp((-exp_krw) / 1800.0, 0.0, 0.20);
                    regime_edge_add += std::clamp((-exp_krw) / 200000.0, 0.0, 0.0004);
                } else if (exp_krw > 8.0 && wr >= 0.58 && pf >= 1.15) {
                    required_signal_strength -= 0.02;
                    regime_rr_add -= 0.08;
                    regime_edge_add -= 0.00015;
                }
                if (wr < 0.42) {
                    required_signal_strength += 0.03;
                    regime_rr_add += 0.10;
                    regime_edge_add += 0.0002;
                }
                if (pf < 0.95) {
                    required_signal_strength += 0.02;
                    regime_rr_add += 0.08;
                    regime_edge_add += 0.0002;
                }
            }
        }
        auto market_regime_it = market_strategy_regime_edge.find(
            makeMarketStrategyRegimeKey(
                signal.market,
                signal.strategy_name,
                signal.market_regime
            )
        );
        if (market_regime_it != market_strategy_regime_edge.end()) {
            const auto& stat = market_regime_it->second;
            if (stat.trades >= 4) {
                const double wr = stat.winRate();
                const double pf = stat.profitFactor();
                const double exp_krw = stat.expectancy();
                const bool focus_market = isLossFocusMarket(signal.market);
                const bool momentum_trending_up =
                    signal.strategy_name == "Advanced Momentum" &&
                    signal.market_regime == analytics::MarketRegime::TRENDING_UP;

                if (exp_krw < -40.0 && wr < 0.25 && pf < 0.75) {
                    regime_pattern_block = true;
                }
                if (exp_krw < -20.0 && wr < 0.35 && pf < 0.85) {
                    required_signal_strength += 0.05;
                    regime_rr_add += 0.15;
                    regime_edge_add += 0.0003;
                } else if (exp_krw < -10.0) {
                    required_signal_strength += 0.02;
                    regime_rr_add += 0.08;
                    regime_edge_add += 0.00015;
                } else if (exp_krw > 10.0 && wr >= 0.60 && pf >= 1.20) {
                    required_signal_strength -= 0.02;
                    regime_rr_add -= 0.06;
                    regime_edge_add -= 0.0001;
                }

                if (focus_market && exp_krw < -12.0) {
                    required_signal_strength += 0.03;
                    regime_rr_add += 0.10;
                    regime_edge_add += 0.0002;
                }
                if (focus_market && momentum_trending_up) {
                    if (exp_krw < -8.0 && stat.trades >= 3) {
                        required_signal_strength = std::max(required_signal_strength, 0.68);
                        regime_rr_add += 0.18;
                        regime_edge_add += 0.00035;
                    }
                    if (exp_krw < -15.0 && wr < 0.30 && stat.trades >= 5) {
                        regime_pattern_block = true;
                    }
                }
            }
        }
        required_signal_strength = std::clamp(required_signal_strength, 0.35, 0.92);
        signal.signal_filter = required_signal_strength;
        if (regime_pattern_block) {
            LOG_INFO("{} skipped by regime-pattern gate [{}]: severe negative pattern history",
                     signal.market, signal.strategy_name);
            filtered_out++;
            continue;
        }
        if (signal.strength < required_signal_strength) {
            LOG_INFO("{} filtered out by dynamic filter (strength {:.3f} < filter {:.3f})",
                     signal.market, signal.strength, required_signal_strength);
            filtered_out++;
            continue;
        }

        auto stat_it = strategy_edge.find(signal.strategy_name);
        if (stat_it != strategy_edge.end()) {
            const auto& stat = stat_it->second;
            if (stat.trades >= config_.min_strategy_trades_for_ev) {
                const double exp_krw = stat.expectancy();
                const double pf = stat.profitFactor();
                if (exp_krw < config_.min_strategy_expectancy_krw ||
                    pf < config_.min_strategy_profit_factor) {
                    LOG_INFO("{} skipped by strategy EV gate [{}]: exp {:.2f} < {:.2f} or PF {:.2f} < {:.2f}",
                             signal.market, signal.strategy_name,
                             exp_krw, config_.min_strategy_expectancy_krw,
                             pf, config_.min_strategy_profit_factor);
                    filtered_out++;
                    continue;
                }
            }
        }
        if (small_seed_mode &&
            signal.strategy_trade_count >= 10 &&
            signal.strategy_win_rate < 0.55) {
            LOG_INFO("{} skipped by small-seed quality gate: win_rate {:.2f} < 0.55 ({} trades)",
                     signal.market, signal.strategy_win_rate, signal.strategy_trade_count);
            filtered_out++;
            continue;
        }

        normalizeSignalStopLossByRegime(signal, signal.market_regime);

        if (!rebalanceSignalRiskReward(signal, config_)) {
            LOG_WARN("{} skipped: invalid price levels for RR normalization", signal.market);
            filtered_out++;
            continue;
        }

        if (config_.avoid_high_volatility &&
            signal.market_regime == analytics::MarketRegime::HIGH_VOLATILITY) {
            LOG_INFO("{} skipped by regime gate: {}", signal.market, regimeToString(signal.market_regime));
            filtered_out++;
            continue;
        }
        if (config_.avoid_trending_down &&
            signal.market_regime == analytics::MarketRegime::TRENDING_DOWN) {
            LOG_INFO("{} skipped by regime gate: {}", signal.market, regimeToString(signal.market_regime));
            filtered_out++;
            continue;
        }

        const double entry_price = signal.entry_price;
        const double take_profit_price = (signal.take_profit_2 > 0.0) ? signal.take_profit_2 : signal.take_profit_1;
        const double stop_loss_price = signal.stop_loss;

        if (entry_price <= 0.0 || take_profit_price <= 0.0 || stop_loss_price <= 0.0 ||
            take_profit_price <= entry_price || stop_loss_price >= entry_price) {
            LOG_WARN("{} skipped: invalid TP/SL for entry quality gate (entry {:.0f}, tp {:.0f}, sl {:.0f})",
                     signal.market, entry_price, take_profit_price, stop_loss_price);
            filtered_out++;
            continue;
        }

        const double gross_reward_pct = (take_profit_price - entry_price) / entry_price;
        const double gross_risk_pct = (entry_price - stop_loss_price) / entry_price;
        const double reward_risk_ratio = (gross_risk_pct > 1e-8) ? (gross_reward_pct / gross_risk_pct) : 0.0;

        const double fee_rate_per_side = Config::getInstance().getFeeRate();
        const double round_trip_cost_pct = (fee_rate_per_side * 2.0) + (config_.max_slippage_pct * 2.0);
        const double expected_net_edge_pct = gross_reward_pct - round_trip_cost_pct;

        double adaptive_rr_gate = min_reward_risk_gate;
        double adaptive_edge_gate = min_expected_edge_gate;
        if (auto quality_it = strategy_edge.find(signal.strategy_name); quality_it != strategy_edge.end()) {
            const auto& stat = quality_it->second;
            if (stat.trades >= 8) {
                const double wr = stat.winRate();
                const double pf = stat.profitFactor();
                const double exp_krw = stat.expectancy();

                if (wr < 0.42) {
                    adaptive_rr_gate += 0.35;
                    adaptive_edge_gate += 0.0006;
                } else if (wr < 0.48) {
                    adaptive_rr_gate += 0.20;
                    adaptive_edge_gate += 0.0003;
                }

                if (pf < 0.90) {
                    adaptive_rr_gate += 0.25;
                    adaptive_edge_gate += 0.0005;
                } else if (pf < 1.00) {
                    adaptive_rr_gate += 0.12;
                    adaptive_edge_gate += 0.0002;
                }

                if (exp_krw < 0.0) {
                    adaptive_rr_gate += std::clamp((-exp_krw) / 1000.0, 0.0, 0.25);
                    adaptive_edge_gate += std::clamp((-exp_krw) / 80000.0, 0.0, 0.0004);
                }
            }
        }
        adaptive_rr_gate += regime_rr_add;
        adaptive_edge_gate += regime_edge_add;
        adaptive_rr_gate = std::clamp(adaptive_rr_gate, min_reward_risk_gate, min_reward_risk_gate + 0.95);
        adaptive_edge_gate = std::clamp(adaptive_edge_gate, min_expected_edge_gate, min_expected_edge_gate + 0.0022);

        if (reward_risk_ratio < adaptive_rr_gate) {
            LOG_INFO("{} skipped by RR gate: {:.2f} < {:.2f}",
                     signal.market, reward_risk_ratio, adaptive_rr_gate);
            filtered_out++;
            continue;
        }

        if (expected_net_edge_pct < adaptive_edge_gate) {
            LOG_INFO("{} skipped by edge gate: {:.3f}% < {:.3f}% (gross {:.3f}%, cost {:.3f}%)",
                     signal.market,
                     expected_net_edge_pct * 100.0,
                     adaptive_edge_gate * 100.0,
                     gross_reward_pct * 100.0,
                     round_trip_cost_pct * 100.0);
            filtered_out++;
            continue;
        }

        signal.position_size *= current_scale;
        const double strength_multiplier = std::clamp(0.5 + signal.strength, 0.75, 1.5);
        signal.position_size *= strength_multiplier;

        LOG_INFO("Signal candidate - {} [{}] (strength {:.3f}, scale {:.2f}x, strength_mul {:.2f}x => pos {:.4f})",
                 signal.market, signal.strategy_name, signal.strength, current_scale,
                 strength_multiplier, signal.position_size);

        auto risk_metrics = risk_manager_->getRiskMetrics();
        const double risk_budget_krw = risk_metrics.total_capital * config_.risk_per_trade_pct;
        const double min_required_krw = config_.min_order_krw;

        if (risk_metrics.available_capital < min_required_krw) {
            LOG_WARN("{} skipped - insufficient available capital (have {:.0f}, need {:.0f})",
                     signal.market, risk_metrics.available_capital, min_required_krw);
            continue;
        }

        double min_position_size = min_required_krw / risk_metrics.available_capital;
        if (signal.position_size < min_position_size) {
            LOG_INFO("{} raised position_size {:.4f} -> {:.4f} (min order {:.0f})",
                     signal.market, signal.position_size, min_position_size, min_required_krw);
            signal.position_size = min_position_size;
        }

        if (signal.position_size > 1.0) {
            LOG_WARN("{} position_size clamped: {:.4f} -> 1.0", signal.market, signal.position_size);
            signal.position_size = 1.0;
        }

        double risk_pct = 0.0;
        if (signal.entry_price > 0.0 && signal.stop_loss > 0.0) {
            risk_pct = (signal.entry_price - signal.stop_loss) / signal.entry_price;
        }

        if (risk_pct > 0.0 && risk_metrics.available_capital > 0.0 && risk_budget_krw > 0.0) {
            const double max_invest_amount = risk_budget_krw / risk_pct;
            const double max_position_size = max_invest_amount / risk_metrics.available_capital;

            if (signal.position_size > max_position_size) {
                LOG_INFO("{} risk-based size clamp: {:.4f} -> {:.4f} (budget {:.0f})",
                         signal.market, signal.position_size, max_position_size, risk_budget_krw);
                signal.position_size = max_position_size;
            }
        }

        auto strategy_ptr = strategy_manager_ ? strategy_manager_->getStrategy(signal.strategy_name) : nullptr;
        const bool is_grid_strategy = (signal.strategy_name == "Grid Trading Strategy");

        if (is_grid_strategy && strategy_ptr) {
            auto grid_metrics = risk_manager_->getRiskMetrics();
            const double allocated_capital = grid_metrics.available_capital * signal.position_size;

            if (!risk_manager_->reserveGridCapital(signal.market, allocated_capital, signal.strategy_name)) {
                LOG_WARN("{} grid order skipped (capital reservation failed)", signal.market);
                continue;
            }

            if (!strategy_ptr->onSignalAccepted(signal, allocated_capital)) {
                risk_manager_->releaseGridCapital(signal.market);
                LOG_WARN("{} grid order skipped (strategy acceptance failed)", signal.market);
                continue;
            }

            LOG_INFO("{} grid order accepted (allocated capital {:.0f})", signal.market, allocated_capital);
            executed++;
            continue;
        }

        if (!risk_manager_->canEnterPosition(
            signal.market,
            signal.entry_price,
            signal.position_size,
            signal.strategy_name
        )) {
            LOG_WARN("{} buy skipped (risk check rejected)", signal.market);
            continue;
        }

        signal.entry_amount = risk_metrics.available_capital * signal.position_size;
        if (signal.entry_amount < min_required_krw) {
            signal.entry_amount = min_required_krw;
        }

        if (executeBuyOrder(signal.market, signal)) {
            risk_manager_->setPositionSignalInfo(
                signal.market,
                signal.signal_filter,
                signal.strength,
                signal.market_regime,
                signal.liquidity_score,
                signal.volatility,
                (signal.expected_value != 0.0 ? signal.expected_value : expected_net_edge_pct),
                reward_risk_ratio
            );
            if (!is_grid_strategy && strategy_ptr) {
                const double allocated_capital =
                    signal.entry_amount > 0.0
                        ? signal.entry_amount
                        : (risk_metrics.available_capital * signal.position_size);
                if (!strategy_ptr->onSignalAccepted(signal, allocated_capital)) {
                    LOG_WARN("{} strategy accepted order but state registration was skipped: {}",
                             signal.market, signal.strategy_name);
                }
            }
            executed++;
            executed_buys_this_scan++;

            const size_t previous_total = total_potential;
            total_potential = getTotalPotentialPositions();
            LOG_INFO("Position counter updated: {} -> {} (max {})",
                     previous_total, total_potential, config_.max_positions);
        }
    }

    LOG_INFO("Signal execution done: executed {}, filtered {}", executed, filtered_out);
    if (executed_buys_this_scan > 0) {
        scans_without_new_entry_ = 0;
    } else {
        scans_without_new_entry_ = std::min(scans_without_new_entry_ + 1, 100);
    }
    pending_signals_.clear();
}
bool TradingEngine::executeBuyOrder(
    const std::string& market,
    const strategy::Signal& signal
) {
    LOG_INFO("?耀붾굝??????????轅붽틓????? {} (???????욧펾???????ル뒌??嶺??影袁る젺?? {:.2f})", market, signal.strength);
    
    try {
        // 1. [????? ?????Orderbook) ???????????????耀붾굝????????????븐뼐???????????????ル???????癲됱빖???嶺????
        //    ???????????袁⑸즴筌?씛彛?????????レ몖?????????쎛(Ticker)???????ル??? ??????袁⑸즴筌?씛彛???? ???????롮쾸?椰?嚥싲갭怡?????????????????'????븐뼐??????????遺븍き????1???'?????????댁삩?????
        auto orderbook = http_client_->getOrderBook(market);
        if (orderbook.empty()) {
            LOG_ERROR("??? ????⑥ル????????????ㅼ뒩?? {}", market);
            return false;
        }
        
        // [??????ш끽踰椰?????袁ㅻ쇀??? API ??????醫딇떍?????????????????????녳븢?????????롮쾸?椰???????븐뼐???????????癲ル슢??룸퀬苑??
        nlohmann::json units;
        if (orderbook.is_array() && !orderbook.empty()) {
            units = orderbook[0]["orderbook_units"];
        } else if (orderbook.contains("orderbook_units")) {
            units = orderbook["orderbook_units"];
        } else {
            LOG_ERROR("{} - ??? ?????????orderbook_units)???耀붾굝????????????????源낆┰?????????곸죩: {}", market, orderbook.dump());
            return false;
        }

        double best_ask_price = calculateOptimalBuyPrice(market, signal.entry_price, orderbook); // ????븐뼐??????????遺븍き????1???
        
        // 2. ??????????댁뢿援????????????????????????????
        auto metrics = risk_manager_->getRiskMetrics();
        
        // Keep execution minimum aligned with config and strategy sizing.
        const double MIN_ORDER_BUFFER = config_.min_order_krw;

        double invest_amount = 0.0;
        double safe_position_size = 0.0;
        
        // [NEW] executeSignals?????????????????????遺얘턁??????????????댁뢿援????????????
        if (signal.entry_amount > 0.0) {
            invest_amount = signal.entry_amount;
            // ???? position_size ??????熬곣몿????(??????醫딇떍?????繹먮굞議???????븐뼐????????? ??癲됱빖???嶺??????븐뼐????傭?끆?????Β?ｊ콞???癲??
            if (metrics.available_capital > 0) {
                safe_position_size = invest_amount / metrics.available_capital;
            }
        } else {
            // Fallback (???????????????雅?퍔瑗?땟???
            safe_position_size = signal.position_size;
            if (safe_position_size > 1.0) safe_position_size = 1.0;
            invest_amount = metrics.available_capital * safe_position_size;
        }

        // [????븐뼐????????????곌떽?깃뎀??????븐뼐???????????1] ???????ル???????????????????븐뼐?????????????椰???????遺얘턁??????????(???? ????????????????????耀붾굝???????
        // ??????獄쏅챷???? signal.entry_amount??????????????????????ル?????????????????????????????????롮쾸?椰?嚥▲굧???븍툖????????곗뵰?????
        // ??????袁⑸즴筌?씛彛?????????? ???????ル탛????????????????????????븐뼐?????? ???遺얘턁????????????????袁⑸즴筌?씛彛??
        if (metrics.available_capital < invest_amount) {
             // ?????????????깆궔?????? ??? ??????袁⑸즴筌?씛彛???돗?????⑸걦?????? ??????袁⑸즴筌?씛彛???????룸챷援?????????獄쏅챶留???
             // ????븐뼐????????????????獄쏅챷???饔낅떽??????怨몃뮡????????곕츥?????????????耀붾굝????癲ル슢???苡?????????????獄쏅챶留????????濚밸Ŧ援욃퐲????
             if (metrics.available_capital < MIN_ORDER_BUFFER) {
                 LOG_WARN("{} - ?????ル뒌??????????????堉온???{:.0f} < {:.0f} (?耀붾굝????????????Β?ル윲??)", 
                          market, metrics.available_capital, MIN_ORDER_BUFFER);
                 return false;
             }
             // ??? ???????롮쾸?椰???⑤챷寃?┼?????븐뼐????????????????곕츥????????????????⑤슢堉??거??? ??? ?????關?쒎첎?嫄???????????????븐뼐????????(Partial Entry)
             LOG_INFO("{} - ?????ル뒌??????????????堉온?????⑥ル???????????????筌??????⑥ル????? {:.0f} -> {:.0f}", 
                      market, invest_amount, metrics.available_capital);
             invest_amount = metrics.available_capital;
        }
        
        LOG_INFO("{} - [??傭?끆??????? ????????μ떜媛?걫?????雅?굛肄???????? {:.0f} KRW (?????ル뒌????{:.0f})", 
                     market, invest_amount, metrics.available_capital);
        
        if (invest_amount < MIN_ORDER_BUFFER) {
            LOG_WARN("{} - ?耀붾굝??????????????곕춴???????堉온??????雅?굛肄???????? {:.0f}, ?????獄쏅챶留?? {:.0f} KRW) [??????????????]", 
                     market, invest_amount, MIN_ORDER_BUFFER);
            return false;
        }
        
        // [?????????ш끽踰椰?????袁ㅻ쇀??? ??????⑤벡瑜?????position_size??RiskManager???????
        if (!risk_manager_->canEnterPosition(
            market,
            signal.entry_price,
            safe_position_size,
            signal.strategy_name
        )) {
            LOG_WARN("{} - ?????얠뺏?????깅젿癲????汝뷴젆?琉???????????ㅼ뒩??(?????얠뺏?????깅젿癲??????????", market);
            return false;
        }

        if (invest_amount > config_.max_order_krw) invest_amount = config_.max_order_krw;
        
        // ????븐뼐????????? ??????獄쏅챷???饔낅떽??????怨몃뮡????????????????????(??????8????????꿔꺂???????)
        double quantity = invest_amount / best_ask_price;

        // [NEW] ??????獄쏅챷???饔낅떽??????怨몃뮡???????????????????????? ???????
        double slippage_pct = estimateOrderbookSlippagePct(
            orderbook,
            quantity,
            true,
            best_ask_price
        );

        if (slippage_pct > config_.max_slippage_pct) {
            LOG_WARN("{} ?????? ????? {:.3f}% > {:.3f}% (?耀붾굝????????耀붾굝????곌램????節뗪텤????",
                     market, slippage_pct * 100.0, config_.max_slippage_pct * 100.0);
            return false;
        }

        if (config_.enable_core_plane_bridge &&
            config_.enable_core_risk_plane &&
            core_cycle_) {
            core::ExecutionRequest request;
            request.market = market;
            request.side = OrderSide::BUY;
            request.price = best_ask_price;
            request.volume = quantity;
            request.strategy_name = signal.strategy_name;
            request.stop_loss = signal.stop_loss;
            request.take_profit_1 = signal.take_profit_1;
            request.take_profit_2 = signal.take_profit_2;
            request.breakeven_trigger = signal.breakeven_trigger;
            request.trailing_start = signal.trailing_start;

            auto compliance = core_cycle_->validateEntry(request, signal);
            if (!compliance.allowed) {
                LOG_WARN("{} buy skipped by compliance gate: {}", market, compliance.reason);
                return false;
            }
        }
        
        // ???????????????⑤벡瑜????(??????????遺얘턁???????????????????븐뼐???????????癲ル슢??룸퀬苑?????????곗뿨????????곷♧??????????????????獄쏅챷????
        // [?????? ?????????????????(sprintf ?????stringstream ????
        char buffer[64];
        // ??????? ??????8????????꿔꺂???????, ??????????ъ몥?????????0 ???????곕??????????????雅?퍔瑗?땟?????????袁⑸즴筌?씛彛?????????????熬곣몿???
        std::snprintf(buffer, sizeof(buffer), "%.8f", quantity); 
        std::string vol_str(buffer);
        
        LOG_INFO("  ??????밸쫫??꿔꺂????댁슦???????袁ⓦ걤?嶺뚯쉶?????렢??? ??? {:.0f}, ??????{}, ???雅?굛肄????????{:.0f}", 
                 best_ask_price, vol_str, invest_amount);

            // 3. [???????롮쾸?椰?嚥싲갭怡??? ???????롮쾸?椰?嚥싲갭怡???????븐뼐??????????????獄쏅챷???饔낅떽??????怨몃뮡??(OrderManager ??????袁⑸즴筌?씛彛??鶯ㅺ동????泥?
        if (config_.mode == TradingMode::LIVE && !config_.dry_run) {
            
            // [NEW] OrderManager??????????????뉙뀭???????獄쏅챷???饔낅떽??????怨몃뮡?????耀붾굝???????
            // Strategy Metadata ??????袁⑸즴筌?씛彛???
            bool submitted = order_manager_->submitOrder(
                market,
                OrderSide::BUY,
                best_ask_price,
                quantity,
                signal.strategy_name,
                signal.stop_loss,
                signal.take_profit_1,
                signal.take_profit_2,
                signal.breakeven_trigger,
                signal.trailing_start
            );

            if (submitted) {
                LOG_INFO("OrderManager ??????밸쫫??꿔꺂????댁슦?????轅붽틓??????????獄쏅챶留?? {}", market);
                risk_manager_->reservePendingCapital(invest_amount);  // ???耀붾굝???????????????????????
                nlohmann::json order_payload;
                order_payload["side"] = "BUY";
                order_payload["price"] = best_ask_price;
                order_payload["volume"] = quantity;
                order_payload["strategy_name"] = signal.strategy_name;
                order_payload["entry_amount"] = invest_amount;
                appendJournalEvent(
                    core::JournalEventType::ORDER_SUBMITTED,
                    market,
                    "live_buy",
                    order_payload
                );
                return true; // Async Success
            } else {
                LOG_ERROR("OrderManager submit failed");
                return false;
            }
        } 
        else {
            // Paper Trading (????븐뼐???????????븐뼔?????? - ???????????????雅?퍔瑗?땟?????? (Simulation)
            // ... (Paper Mode Logic stays roughly same or we use OrderManager for Paper too?)
            // OrderManager currently hits API. So for Paper Mode we should NOT use OrderManager 
            // unless we Mock API.
            // Current Paper Mode simulates "Fill" immediately.
            
            // [?????關?쒎첎?嫄?嶺뚮슢梨뜹ㅇ??????????????????
            double dynamic_stop_loss = best_ask_price * 0.975; // ?????????? -2.5%
            try {
                auto candles_json = http_client_->getCandles(market, "60", 200);
                if (!candles_json.empty() && candles_json.is_array()) {
                    auto candles = analytics::TechnicalIndicators::jsonToCandles(candles_json);
                    if (!candles.empty()) {
                        dynamic_stop_loss = risk_manager_->calculateDynamicStopLoss(best_ask_price, candles);
                        LOG_INFO("[PAPER] ????嚥싲갭큔?琉몃쨨???????????????쎛 ??傭?끆??????? {:.0f} ({:.2f}%)", 
                                 dynamic_stop_loss, (dynamic_stop_loss - best_ask_price) / best_ask_price * 100.0);
                    }
                }
            } catch (const std::exception& e) {
                LOG_WARN("[PAPER] ????嚥싲갭큔?琉몃쨨???????????????쎛 ??傭?끆??????????????ㅼ뒩?? ??????????-2.5%) ???? {}", e.what());
            }

            double applied_stop_loss = dynamic_stop_loss;
            if (signal.stop_loss > 0.0 && signal.strategy_name == "Advanced Scalping") {
                if (signal.stop_loss < best_ask_price) {
                    applied_stop_loss = signal.stop_loss;
                }
            }
            
             double tp1 = signal.take_profit_1 > 0 ? signal.take_profit_1 : best_ask_price * 1.020;
             double tp2 = signal.take_profit_2 > 0 ? signal.take_profit_2 : best_ask_price * 1.030;

             risk_manager_->enterPosition(
                market,
                best_ask_price,
                quantity,
                applied_stop_loss,
                tp1,
                tp2,
                signal.strategy_name,
                signal.breakeven_trigger,
                signal.trailing_start
            );

            nlohmann::json paper_fill_payload;
            paper_fill_payload["side"] = "BUY";
            paper_fill_payload["filled_volume"] = quantity;
            paper_fill_payload["avg_price"] = best_ask_price;
            paper_fill_payload["strategy_name"] = signal.strategy_name;
            appendJournalEvent(
                core::JournalEventType::FILL_APPLIED,
                market,
                "paper_buy",
                paper_fill_payload
            );

            nlohmann::json paper_pos_payload;
            paper_pos_payload["entry_price"] = best_ask_price;
            paper_pos_payload["quantity"] = quantity;
            paper_pos_payload["strategy_name"] = signal.strategy_name;
            appendJournalEvent(
                core::JournalEventType::POSITION_OPENED,
                market,
                "paper_buy",
                paper_pos_payload
            );
            
            return true;
        }

        // Unreachable
        return false;

    } catch (const std::exception& e) {
        LOG_ERROR("executeBuyOrder ????μ떜媛?걫??? {}", e.what());
        return false;
    }
}


void TradingEngine::monitorPositions() {

    static int log_counter = 0;
    bool should_log = (log_counter++ % 10 == 0);

    // 1. ??????袁⑸즴筌?씛彛?????????ㅳ늾??????????ш끽踰椰?????袁ㅻ쇀????????????븐뼐???????????븐뼔???ш끽維뽭뇡硫㏓꺌?용뿪??큺??????????ル??????遺얘턁???????꿔꺂????釉먯춱?癲됱빖???????
    auto positions = risk_manager_->getAllPositions();

    if (!positions.empty() && risk_manager_->isDrawdownExceeded()) {
        LOG_ERROR("Max drawdown exceeded. Forcing protective exits for all open positions.");
        for (const auto& pos : positions) {
            const double price = getCurrentPrice(pos.market);
            if (price <= 0.0) {
                LOG_WARN("{} protective exit skipped: invalid current price", pos.market);
                continue;
            }
            executeSellOrder(pos.market, pos, "max_drawdown", price);
        }
        return;
    }
    
    // 2. ??????⑤벡瑜???? ??????ш끽踰椰?????袁ㅻ쇀???????????源낅펰?????????????癲???沃섅닊?熬곣뫁????????袁⑸즴筌?씛彛??????븐뼐??????????????⑤슢堉??곕????(Batch ??????????????????寃몃탿?????????????뀀???嶺?
    std::set<std::string> market_set;
    for (const auto& pos : positions) {
        market_set.insert(pos.market);
    }

    std::vector<std::shared_ptr<strategy::IStrategy>> strategies;
    if (strategy_manager_) {
        strategies = strategy_manager_->getStrategies();
        for (const auto& strategy : strategies) {
            for (const auto& market : strategy->getActiveMarkets()) {
                market_set.insert(market);
            }
        }
    }

    if (market_set.empty()) {
        return;
    }

    std::vector<std::string> markets;
    markets.reserve(market_set.size());
    for (const auto& market : market_set) {
        markets.push_back(market);
    }
    
    if (should_log) {
        LOG_INFO("===== ??????耀붾굝?????????붾눀????????⑤벡???({}???????욱떌?? =====", markets.size());
    }

    // 3. [????? ?????????HTTP ????癲??????????????븐뼐???????????븐뼔????????????源낅펰????????袁⑸즴筌?씛彛?????????レ몖?????????쎛 ?????????????(Batch Processing)
    std::map<std::string, double> price_map;
    
    try {
        // MarketScanner??????????????嚥▲굧?먩뤆??getTickerBatch ?????
        auto tickers = http_client_->getTickerBatch(markets);
        
        for (const auto& t : tickers) {
            std::string market_code = t["market"].get<std::string>();
            double trade_price = t["trade_price"].get<double>();
            price_map[market_code] = trade_price;
        }
        
    } catch (const std::exception& e) {
        LOG_ERROR("?????????????????????⑥ル????????????ㅼ뒩?? {}", e.what());
        return; // ????????????????????????????곕츧???????????? ????븐뼐?곭춯?竊??????? (???????????븐뼐??????????遺븍き?????????獄쏅챶留??逆곷틳源븃떋?)
    }

    // 4. ????븐뼐?????????????獄쏅챶留????????????泥???????????? ????????????????????????????????룸㎗?ルき????(??????????????대첉?? ????븐뼐??????????????壤??猷고겳???
    for (auto& pos : positions) {
        // ??????????븐뼐??????????????????????????源낅펰?????????ル?????????븐뼐??????????嫄?紐???????
        if (price_map.find(pos.market) == price_map.end()) {
            LOG_WARN("{} ticker data missing", pos.market);
            continue;
        }

        double current_price = price_map[pos.market];
        
        // RiskManager ??????椰?????????????怨룸셾???????꾩룆梨띰쭕???(????븐뼐?????????????獄쏅챶留???????????癲ル슢????
        risk_manager_->updatePosition(pos.market, current_price);
        
        // [????????熬곣몿?????????????????깆궔???? ??????袁⑸즴筌?씛彛??????"?????????怨룸셾???????꾩룆梨띰쭕?????" ????????????????븐뼐??????轅붽틓?????獄쎼끇????===================
        std::shared_ptr<strategy::IStrategy> strategy;
        if (strategy_manager_) {
            // ??????????????⑤슢堉??곕????????????뀀맩鍮??????룸챶猷??????????袁⑸즴筌?씛彛??????븐뼐??????????嫄?紐???????(pos.strategy_name ????
            strategy = strategy_manager_->getStrategy(pos.strategy_name);
            
            if (strategy) {
                // ????????????IStrategy????????熬곣몿?????updateState ???遺얘턁???????
                // ???????ル탛???????????袁⑸즴筌?씛彛?????????怨뚮뼺?됰뗀????????????????泥?耀붾굝????癲ル슢???苡?????????ル탛????????猷멤꼻??????븐뼐????????????곕??????ㅼ뒧??????????????????癲됱빖???嶺???????⑥ル?????
                // ?????轅붽틓?????蹂?뀎????????袁⑸즴筌?씛彛?????????怨뚮뼺?됰뗀??????????ル탛???????????????????????눫?????????ㅻ깹???????????濡?씀?濾????ㅼ굡????(???????롮쾸?椰?嚥싲갭怡????
                strategy->updateState(pos.market, current_price);
            }
        }

        // ???????ル????????????????????????袁④뎬?????????ル??????遺얘턁???????꿔꺂????釉먯춱?癲됱빖???????(??????⑤슢堉??곕?????????????????????????遺얘턁??????????
        auto* updated_pos = risk_manager_->getPosition(pos.market);
        if (!updated_pos) continue;
        
        if (should_log) {
            LOG_INFO("  {} - ?????獄쏅챶留?? {} - ?耀붾굝??????? {:.0f}, ?????獄쏅챶留?? {:.0f}, ????? {:.0f} ({:+.2f}%)",
                     pos.market, updated_pos->strategy_name, updated_pos->entry_price, current_price,
                     updated_pos->unrealized_pnl, updated_pos->unrealized_pnl_pct * 100.0);
        }

        // ?????轅붽틓?????蹂?뀎?? ???????????????????????????????遺얘턁???????????????????븐뼐???????????癲ル슢??룸퀬苑??
        auto scalping_strategy = std::dynamic_pointer_cast<strategy::ScalpingStrategy>(strategy);
        if (scalping_strategy && updated_pos->strategy_name == "Advanced Scalping") {
            if (updated_pos->breakeven_trigger > 0.0 &&
                current_price >= updated_pos->breakeven_trigger &&
                updated_pos->stop_loss < updated_pos->entry_price) {
                risk_manager_->moveStopToBreakeven(updated_pos->market);
            }

            if (updated_pos->trailing_start > 0.0 && current_price >= updated_pos->trailing_start) {
                double new_stop = scalping_strategy->updateTrailingStop(
                    updated_pos->entry_price,
                    updated_pos->highest_price,
                    current_price
                );
                risk_manager_->updateStopLoss(updated_pos->market, new_stop, "trailing");
            }
        }
        
        // --- ????븐뼐??????????遺븍き???????????雅?퍔瑗?땟???(??????袁⑸즴筌?씛彛??????????????????????????????) ---

        // ??????袁⑸즴筌?씛彛??????????????????????????(?????????????????
        if (strategy) {
            long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();
            double holding_time_seconds = (now_ms - updated_pos->entry_time) / 1000.0;

            if (strategy->shouldExit(pos.market, updated_pos->entry_price, current_price, holding_time_seconds)) {
                LOG_INFO("?????獄쏅챶留??????????⑥ル??????????β뼯援?癒れ땡????? {} (?????獄쏅챶留?? {})", pos.market, updated_pos->strategy_name);
                executeSellOrder(pos.market, *updated_pos, "strategy_exit", current_price);
                continue;
            }
        }

        // 1???????????븐뼐???????????(50% ????
        if (!updated_pos->half_closed && current_price >= updated_pos->take_profit_1) {
            LOG_INFO("1???????????⑥ル??????????β뼯援?癒れ땡????? (?????곌떽釉붾???{:+.2f}%)", updated_pos->unrealized_pnl_pct * 100.0);
            executePartialSell(pos.market, *updated_pos, current_price);
            continue; // ???????깆궔????????븐뼐??????????遺븍き?????????????롮쾸?椰???????????源낅펰?????????
        }
        
        // ??????袁⑸즴筌?씛彛??????????븐뼐???????????(?????or 2???????
        if (risk_manager_->shouldExitPosition(pos.market)) {
            std::string reason = "unknown";
            
            // ???????? ????????
            if (current_price <= updated_pos->stop_loss) {
                reason = "stop_loss";
                LOG_INFO("?????????⑥ル??????????β뼯援?癒れ땡?????(?????곌떽釉붾???{:+.2f}%)", updated_pos->unrealized_pnl_pct * 100.0);
            } else if (current_price >= updated_pos->take_profit_2) {
                reason = "take_profit";
                LOG_INFO("2????????耀붾굝??????鶯???蹂μ삏?? ????⑥ル??????????β뼯援?癒れ땡?????(?????곌떽釉붾???{:+.2f}%)", updated_pos->unrealized_pnl_pct * 100.0);
            } else {
                reason = "strategy_exit"; // ??????袁⑸즴筌?씛彛???????????????????됰젿????????⑤벡瑜????(TS ??
            }
            
            executeSellOrder(pos.market, *updated_pos, reason, current_price);
        }
    }

    if (!strategies.empty()) {
        for (const auto& strategy : strategies) {
            auto active_markets = strategy->getActiveMarkets();
            for (const auto& market : active_markets) {
                if (risk_manager_->getPosition(market)) {
                    continue;
                }

                auto price_it = price_map.find(market);
                if (price_it == price_map.end()) {
                    continue;
                }

                strategy->updateState(market, price_it->second);
            }
        }

        for (const auto& strategy : strategies) {
            auto order_requests = strategy->drainOrderRequests();
            for (const auto& request : order_requests) {
                autolife::strategy::OrderResult result;
                result.market = request.market;
                result.side = request.side;
                result.level_id = request.level_id;
                result.reason = request.reason;

                double order_amount = request.price * request.quantity;
                if (order_amount < config_.min_order_krw) {
                    LOG_WARN("?????醫딇떍??????????밸쫫??꿔꺂????댁슦?????雅?굛肄?????????????堉온???{:.0f} < {:.0f}", order_amount, config_.min_order_krw);
                    strategy->onOrderResult(result);
                    continue;
                }

                if (request.side == autolife::strategy::OrderSide::BUY) {
                    if (risk_manager_->isDailyLossLimitExceeded()) {
                        LOG_WARN("{} skipped: daily loss limit exceeded", request.market);
                        strategy->onOrderResult(result);
                        continue;
                    }

                    double reserved_capital = risk_manager_->getReservedGridCapital(request.market);
                    if (reserved_capital <= 0.0 || order_amount > reserved_capital) {
                        LOG_WARN("{} ?????醫딇떍?????耀붾굝????????耀붾굝????곌램????節뗪텤???? ??????쒙쭫????????????堉온???({:.0f} < {:.0f})",
                                 request.market, reserved_capital, order_amount);
                        strategy->onOrderResult(result);
                        continue;
                    }
                }

                if (config_.mode == TradingMode::LIVE && !config_.dry_run) {
                    if (request.side == autolife::strategy::OrderSide::BUY) {
                        auto exec = executeLimitBuyOrder(
                            request.market,
                            request.price,
                            request.quantity,
                            0,
                            500
                        );
                        result.success = exec.success;
                        result.executed_price = exec.executed_price;
                        result.executed_volume = exec.executed_volume;
                    } else {
                        auto exec = executeLimitSellOrder(
                            request.market,
                            request.price,
                            request.quantity,
                            0,
                            500
                        );
                        result.success = exec.success;
                        result.executed_price = exec.executed_price;
                        result.executed_volume = exec.executed_volume;
                    }
                } else {
                    result.success = true;
                    result.executed_price = request.price;
                    result.executed_volume = request.quantity;
                }

                if (result.success && result.executed_volume > 0.0) {
                    risk_manager_->applyGridFill(
                        result.market,
                        result.side,
                        result.executed_price,
                        result.executed_volume
                    );
                }

                strategy->onOrderResult(result);
            }

            auto released_markets = strategy->drainReleasedMarkets();
            for (const auto& market : released_markets) {
                risk_manager_->releaseGridCapital(market);
            }
        }
    }
}

bool TradingEngine::executeSellOrder(
    const std::string& market,
    const risk::Position& position,
    const std::string& reason,
    double current_price
) {
    LOG_INFO("?????獄쏅챶留???耀붾굝??????雅?퍔瑗?땟??????????: {} (????: {})", market, reason);
    
    //double current_price = getCurrentPrice(market);
    if (current_price <= 0) {
        LOG_ERROR("?????獄쏅챶留????⑥る뜤???????쎛 ????⑥ル????????????ㅼ뒩?? {}", market);
        return false;
    }
    
    // ??????袁⑸즴筌?씛彛??????븐뼐??????????遺븍き????
    double sell_quantity = std::floor(position.quantity * 0.9999 * 100000000.0) / 100000000.0;
    double invest_amount = sell_quantity * current_price;
    
    // 1. ????븐뼐????????????????獄쏅챷???饔낅떽??????怨몃뮡????????댁뢿援????????????븐뼐???????????(????븐뼐?곭춯?竊???????????븐뼐?????????? 5,000 KRW)
    if (invest_amount < config_.min_order_krw) {
        LOG_WARN("?耀붾굝??????雅?퍔瑗?땟?????雅?굛肄?????????????堉온???{:.0f} < {:.0f} (?耀붾굝梨루땟???????耀붾굝??????鶯??", invest_amount, config_.min_order_krw);
        return false;
    }
    
    // 2. ???????????????⑤벡瑜??????std::to_string ?????????? ?????????stringstream ????(????븐뼐??????????????ш끽踰椰?????袁ㅻ쇀???
        std::stringstream ss;
        ss << std::fixed << std::setprecision(8) << sell_quantity;
        std::string quantity_str = ss.str();

    // 2-1. ???????븐뼐????????????????븐뼐??????????????븐뼐??????????遺븍き??????? ????????????(????븐뼐????????? ????븐뼐??????????遺븍き??????????????뀀??
    double sell_price = current_price;
    try {
        auto orderbook = http_client_->getOrderBook(market);
        sell_price = calculateOptimalSellPrice(market, current_price, orderbook);
        double slippage_pct = estimateOrderbookSlippagePct(
            orderbook,
            sell_quantity,
            false,
            current_price
        );

        if (slippage_pct > config_.max_slippage_pct) {
            LOG_WARN("{} ?????? ????? {:.3f}% > {:.3f}% (?????耀붾굝????곌램????節뗪텤????",
                     market, slippage_pct * 100.0, config_.max_slippage_pct * 100.0);
            return false;
        }
        LOG_INFO("?耀붾굝??????雅?퍔瑗?땟????? ?耀붾굝??????鶯??? {} (?????獄쏅챶留????⑥る뜤???????쎛: {})", sell_price, current_price);
    } catch (const std::exception& e) {
        LOG_WARN("??? ????⑥ル????????????ㅼ뒩??(?????獄쏅챶留????⑥る뜤???????쎛 ????: {}", e.what());
        sell_price = current_price;
    }

    // 2. ???????롮쾸?椰?嚥싲갭怡?????????獄쏅챷???饔낅떽??????怨몃뮡?????????? (????븐뼐????????? ????????? ?????獄쏅챶留덌┼???????
    double executed_price = current_price;
    if (config_.mode == TradingMode::LIVE) {
        if (config_.dry_run) {
            LOG_WARN("DRY RUN: sell simulation completed");
        } else {
            auto order_result = executeLimitSellOrder(
                market,
                sell_price,
                sell_quantity,
                0,
                500
            );

            if (!order_result.success || order_result.executed_volume <= 0.0) {
                LOG_ERROR("?耀붾굝??????雅?퍔瑗?땟???耀붾굝???????????????ㅼ뒩?? {}", order_result.error_message);
                return false;
            }

            executed_price = order_result.executed_price;
            sell_quantity = order_result.executed_volume;  // [Phase 3] ???????롮쾸?椰?嚥싲갭怡???????븐뼐???????????????獄쏅챶留덌┼???????
            
            // [Phase 3] ???????깆궔????????븐뼐????????????????ル????
            double fill_ratio = order_result.executed_volume / position.quantity;
            if (fill_ratio < 0.999) {
                LOG_WARN("?????堉온????耀붾굝?????????????ル뒌???: {:.8f}/{:.8f} ({:.1f}%)",
                         order_result.executed_volume, position.quantity, fill_ratio * 100.0);
            }
            
            LOG_INFO("?耀붾굝??????雅?퍔瑗?땟???耀붾굝??????????饔낅떽????????? ??? {:.0f} (?????{})",
                     executed_price, order_result.retry_count);
        }
    }
    
    // 3. ??????⑤슢堉??곕?????????????????
    double realized_qty = sell_quantity;
    
    // 4. [Phase 3] ???????깆궔????????븐뼐?????????vs ??????袁⑸즴筌?씛彛??????븐뼐???????????????????已???
    double fill_ratio = sell_quantity / position.quantity;
    const bool fully_closed = fill_ratio >= 0.999;
    if (fully_closed) {
        realized_qty = position.quantity;
        // ??????袁⑸즴筌?씛彛??????븐뼐??????????????????????????雅?퍔瑗?땟???(???????????袁⑸즴筌?씛彛??????
        risk_manager_->exitPosition(market, executed_price, reason);
        nlohmann::json close_payload;
        close_payload["exit_price"] = executed_price;
        close_payload["quantity"] = realized_qty;
        close_payload["reason"] = reason;
        close_payload["gross_pnl"] = (executed_price - position.entry_price) * realized_qty;
        appendJournalEvent(
            core::JournalEventType::POSITION_CLOSED,
            market,
            "sell_exit",
            close_payload
        );
    } else {
        // Partial fill must realize PnL/fees using actual fill price.
        const bool applied = risk_manager_->applyPartialSellFill(
            market,
            executed_price,
            sell_quantity,
            reason + "_partial_fill"
        );
        if (!applied) {
            LOG_ERROR("Partial fill accounting failed: {} qty {:.8f} @ {:.0f}",
                      market, sell_quantity, executed_price);
            return false;
        }
        nlohmann::json reduce_payload;
        reduce_payload["exit_price"] = executed_price;
        reduce_payload["quantity"] = sell_quantity;
        reduce_payload["reason"] = reason + "_partial_fill";
        reduce_payload["gross_pnl"] = (executed_price - position.entry_price) * realized_qty;
        appendJournalEvent(
            core::JournalEventType::POSITION_REDUCED,
            market,
            "sell_partial",
            reduce_payload
        );
    }
    
    // 5. [???????????⑤슢堉??곕???? StrategyManager?????????????袁⑸즴筌?씛彛???????븐뼐??????????????????????????怨룸셾???????꾩룆梨띰쭕???& ?????鶯ㅺ동???????????????대첐??
    if (fully_closed && strategy_manager_ && !position.strategy_name.empty()) {
        // Position ???????????????????濚밸Ŧ援욃퐲?????????蹂㏓??嶺뚮㉡???????strategy_name("Advanced Scalping" ????????????????袁⑸즴筌?씛彛??????븐뼐??????????嫄?紐???????
        auto strategy = strategy_manager_->getStrategy(position.strategy_name);
        
        if (strategy) {
            // Strategy stats are aligned to the same fee-inclusive basis as trade_history.
            const double fee_rate = Config::getInstance().getFeeRate();
            const double exit_value = executed_price * position.quantity;
            const double entry_fee = position.invested_amount * fee_rate;
            const double exit_fee = exit_value * fee_rate;
            const double net_pnl = exit_value - position.invested_amount - entry_fee - exit_fee;
            strategy->updateStatistics(market, net_pnl > 0.0, net_pnl);
            LOG_INFO("?熬곣뫁??{}) ????????낆몥??袁⑤콦 ?????????怨몄젷", position.strategy_name);
        } else if (position.strategy_name != "RECOVERED") {
            // RECOVERED ?????? ????轅붽틓???????????棺堉?뤃???? ?????????뼿????癲됱빖???嶺????????癲됱빖???嶺????
            LOG_WARN("Sell complete but strategy object missing: {}", position.strategy_name);
        }
    }
    
    LOG_INFO("????????????욱떌???????獄쏅챶留?? {} (????? {:.0f} KRW)",
             market, (executed_price - position.entry_price) * realized_qty);
    
    return true;
}


bool TradingEngine::executePartialSell(const std::string& market, const risk::Position& position, double current_price) {

    //double current_price = getCurrentPrice(market);

        if (current_price <= 0) {
        LOG_ERROR("?????獄쏅챶留????⑥る뜤???????쎛 ????⑥ル????????????ㅼ뒩?? {}", market);
        return false;
    }
    
    // 50% ??????????????????
    double sell_quantity = std::floor(position.quantity * 0.5 * 100000000.0) / 100000000.0;
    double invest_amount = sell_quantity * current_price;
    
    // 1. ????븐뼐????????????????獄쏅챷???饔낅떽??????怨몃뮡????????댁뢿援????????????븐뼐?????????????????(????븐뼐?곭춯?竊???????????븐뼐?????????? 5,000 KRW)
    // 1. ????븐뼐????????????????獄쏅챷???饔낅떽??????怨몃뮡????????댁뢿援????????????븐뼐?????????????????(????븐뼐?곭춯?竊???????????븐뼐?????????? 5,000 KRW)
    if (invest_amount < config_.min_order_krw) {
        LOG_WARN("?????堉온??????Β?レ름???????雅?굛肄?????????????堉온???{:.0f} < ?耀붾굝??????鶯??{:.0f}) -> ?????堉온???????????轅붽틓?????(Half Closed ?耀붾굝????鶯ㅺ동??筌믡룓愿??", 
                 invest_amount, config_.min_order_krw);
        
        // [Fix] ??????댁뢿援??????????????????耀붾굝????癲ル슢???苡?????????????깆궔?????????????耀붾굝??????????癲됱빖???嶺????????????筌뤾퍓愿??????????????????????類ㅻ첐???嶺뚮ㅎ??轅깆????? (????轅붽틓????????????룸㎗?ルき?????????獄쏅챶留??逆곷틳源븃떋?)
        // RiskManager??????븐뼐??????????????????????耀붾굝????????half_closed = true ???????롮쾸?椰???
        if (risk_manager_) {
            // Safe method call
            risk_manager_->setHalfClosed(market, true);
        }
        return true; // ?????嚥????????ㅼ뒧??띤겫?????????????븐뼐???????????癲ル슢??룸퀬苑????耀붾굝?????????????????븐뼐?????????
    }
    
    LOG_INFO("?????堉온????耀붾굝??????雅?퍔瑗?땟?????????? (50%): {}", market);
    
    LOG_INFO("  ?耀붾굝?????????: {:.0f}, ??????嚥싲갭큔???: {:.0f}, ?????堉온??????Β?レ름???????獄쏅챶留?? {:.8f}",
             position.entry_price, current_price, sell_quantity);
    
    // 2. ???????????????⑤벡瑜??????std::to_string ?????????? ?????????stringstream ????(????븐뼐??????????????ш끽踰椰?????袁ㅻ쇀???
    std::stringstream ss;
    ss << std::fixed << std::setprecision(8) << sell_quantity;
    std::string quantity_str = ss.str();

    // 2-1. ???????븐뼐????????????????븐뼐??????????????븐뼐??????????遺븍き??????? ????????????(????븐뼐????????? ????븐뼐??????????遺븍き??????????????뀀??
    double sell_price = current_price;
    try {
        auto orderbook = http_client_->getOrderBook(market);
        sell_price = calculateOptimalSellPrice(market, current_price, orderbook);
        double slippage_pct = estimateOrderbookSlippagePct(
            orderbook,
            sell_quantity,
            false,
            current_price
        );

        if (slippage_pct > config_.max_slippage_pct) {
            LOG_WARN("{} ?????? ????? {:.3f}% > {:.3f}% (?????堉온????????耀붾굝????곌램????節뗪텤????",
                     market, slippage_pct * 100.0, config_.max_slippage_pct * 100.0);
            return false;
        }
    } catch (const std::exception&) {
        sell_price = current_price;
    }

    auto apply_partial_fill = [&](double fill_price, double fill_qty, const std::string& reason_tag) -> bool {
        const bool applied = risk_manager_->applyPartialSellFill(market, fill_price, fill_qty, reason_tag);
        if (!applied) {
            LOG_ERROR("Partial sell accounting failed: {} qty {:.8f} @ {:.0f}",
                      market, fill_qty, fill_price);
            return false;
        }
        risk_manager_->setHalfClosed(market, true);
        risk_manager_->moveStopToBreakeven(market);
        return true;
    };

    // 2. ???????롮쾸?椰?嚥싲갭怡?????????獄쏅챷???饔낅떽??????怨몃뮡?????????? (????븐뼐????????? ????????? ?????獄쏅챶留덌┼???????
    if (config_.mode == TradingMode::LIVE) {
        if (config_.dry_run) {
            LOG_WARN("DRY RUN: partial sell simulated");
            if (!apply_partial_fill(current_price, sell_quantity, "partial_take_profit_dry_run")) {
                return false;
            }
            nlohmann::json reduce_payload;
            reduce_payload["exit_price"] = current_price;
            reduce_payload["quantity"] = sell_quantity;
            reduce_payload["reason"] = "partial_take_profit_dry_run";
            appendJournalEvent(
                core::JournalEventType::POSITION_REDUCED,
                market,
                "partial_sell",
                reduce_payload
            );
            return true;
        }

        auto order_result = executeLimitSellOrder(
            market,
            sell_price,
            sell_quantity,
            0,
            500
        );

        if (!order_result.success || order_result.executed_volume <= 0.0) {
            LOG_ERROR("?????堉온????耀붾굝??????雅?퍔瑗?땟???耀붾굝???????????????ㅼ뒩?? {}", order_result.error_message);
            return false;
        }

        LOG_INFO("?????堉온????耀붾굝??????雅?퍔瑗?땟???耀붾굝??????????饔낅떽????????? ??? {:.0f} (?????{})",
                 order_result.executed_price, order_result.retry_count);
        if (!apply_partial_fill(order_result.executed_price, order_result.executed_volume, "partial_take_profit")) {
            return false;
        }
        nlohmann::json reduce_payload;
        reduce_payload["exit_price"] = order_result.executed_price;
        reduce_payload["quantity"] = order_result.executed_volume;
        reduce_payload["reason"] = "partial_take_profit";
        appendJournalEvent(
            core::JournalEventType::POSITION_REDUCED,
            market,
            "partial_sell",
            reduce_payload
        );
        return true;
    }

    // Paper Trading
    if (!apply_partial_fill(current_price, sell_quantity, "partial_take_profit_paper")) {
        return false;
    }
    nlohmann::json reduce_payload;
    reduce_payload["exit_price"] = current_price;
    reduce_payload["quantity"] = sell_quantity;
    reduce_payload["reason"] = "partial_take_profit_paper";
    appendJournalEvent(
        core::JournalEventType::POSITION_REDUCED,
        market,
        "partial_sell",
        reduce_payload
    );
    return true;
}

// ===== ??????獄쏅챷???饔낅떽??????怨몃뮡??????=====

double TradingEngine::calculateOptimalBuyPrice(
    const std::string& market,
    double base_price,
    const nlohmann::json& orderbook
) {
    (void)market;
    nlohmann::json units;

    if (orderbook.is_array() && !orderbook.empty()) {
        units = orderbook[0]["orderbook_units"];
    } else if (orderbook.contains("orderbook_units")) {
        units = orderbook["orderbook_units"];
    }

    if (units.is_array() && !units.empty()) {
        double ask = units[0].value("ask_price", base_price);
        return common::roundUpToTickSize(ask);  // [Phase 3] ??? ????????⑤벡苑?????
    }

    return common::roundUpToTickSize(base_price);
}

double TradingEngine::calculateOptimalSellPrice(
    const std::string& market,
    double base_price,
    const nlohmann::json& orderbook
) {
    (void)market;
    nlohmann::json units;

    if (orderbook.is_array() && !orderbook.empty()) {
        units = orderbook[0]["orderbook_units"];
    } else if (orderbook.contains("orderbook_units")) {
        units = orderbook["orderbook_units"];
    }

    if (units.is_array() && !units.empty()) {
        double bid = units[0].value("bid_price", base_price);
        return common::roundDownToTickSize(bid);  // [Phase 3] ??? ????????⑤벡苑??????
    }

    return common::roundDownToTickSize(base_price);
}

double TradingEngine::estimateOrderbookVWAPPrice(
    const nlohmann::json& orderbook,
    double target_volume,
    bool is_buy
) const {
    if (target_volume <= 0.0) {
        return 0.0;
    }

    nlohmann::json units;
    if (orderbook.is_array() && !orderbook.empty()) {
        units = orderbook[0].value("orderbook_units", nlohmann::json::array());
    } else {
        units = orderbook.value("orderbook_units", nlohmann::json::array());
    }

    if (!units.is_array() || units.empty()) {
        return 0.0;
    }

    double reference_price = 0.0;
    if (!units.empty()) {
        reference_price = units[0].value(is_buy ? "ask_price" : "bid_price", 0.0);
    }

    if (reference_price <= 0.0) {
        return 0.0;
    }

    double target_notional = reference_price * target_volume;
    return analytics::OrderbookAnalyzer::estimateVWAPForNotional(
        units,
        target_notional,
        is_buy
    );
}

double TradingEngine::estimateOrderbookSlippagePct(
    const nlohmann::json& orderbook,
    double target_volume,
    bool is_buy,
    double reference_price
) const {
    if (reference_price <= 0.0) {
        return 0.0;
    }

    nlohmann::json units;
    if (orderbook.is_array() && !orderbook.empty()) {
        units = orderbook[0].value("orderbook_units", nlohmann::json::array());
    } else {
        units = orderbook.value("orderbook_units", nlohmann::json::array());
    }

    if (!units.is_array() || units.empty()) {
        return 0.0;
    }

    double target_notional = reference_price * target_volume;
    double slippage = analytics::OrderbookAnalyzer::estimateSlippagePctForNotional(
        units,
        target_notional,
        is_buy,
        reference_price
    );

    return std::max(0.0, slippage);
}

TradingEngine::OrderFillInfo TradingEngine::verifyOrderFill(
    const std::string& uuid,
    const std::string& market,
    double order_volume
) {
    OrderFillInfo info{};
    (void)market;
    (void)order_volume;

    auto toDouble = [](const nlohmann::json& value) -> double {
        try {
            if (value.is_string()) {
                return std::stod(value.get<std::string>());
            }
            if (value.is_number()) {
                return value.get<double>();
            }
        } catch (...) {
        }
        return 0.0;
    };

    try {
        auto check = http_client_->getOrder(uuid);
        std::string state = check.value("state", "");

        double total_funds = 0.0;
        double total_vol = 0.0;

        if (check.contains("trades") && check["trades"].is_array()) {
            for (const auto& trade : check["trades"]) {
                double trade_vol = toDouble(trade["volume"]);
                double trade_price = toDouble(trade["price"]);
                total_vol += trade_vol;
                total_funds += trade_vol * trade_price;
            }
        } else if (check.contains("executed_volume")) {
            total_vol = toDouble(check["executed_volume"]);
        }

        if (total_vol > 0.0 && total_funds > 0.0) {
            info.avg_price = total_funds / total_vol;
        } else if (check.contains("price")) {
            info.avg_price = toDouble(check["price"]);
        }

        info.filled_volume = total_vol;
        info.is_filled = (state == "done") && total_vol > 0.0;
        info.is_partially_filled = (!info.is_filled && total_vol > 0.0);
        info.fee = 0.0;
    } catch (const std::exception& e) {
        LOG_WARN("??????밸쫫??꿔꺂????댁슦???耀붾굝??????????饔낅떽????????????????ㅼ뒩?? {}", e.what());
    }

    return info;
}

TradingEngine::LimitOrderResult TradingEngine::executeLimitBuyOrder(
    const std::string& market,
    double entry_price,
    double quantity,
    int max_retries,
    int retry_wait_ms
) {
    LimitOrderResult result{};
    result.success = false;
    result.retry_count = 0;
    result.executed_price = 0.0;
    result.executed_volume = 0.0;

    double remaining = quantity;
    double total_filled = 0.0;
    double total_funds = 0.0;

    int max_attempts = max_retries > 0 ? max_retries : 3;
    int attempt_count = 0;

    while (running_ && remaining > 0.00000001 && attempt_count < max_attempts) {
        std::stringstream ss;
        ss << std::fixed << std::setprecision(8) << remaining;
        std::string vol_str = ss.str();
        // [Phase 3] ??? ????????⑤벡苑?????+ ??? ???????????????⑤벡瑜????
        double tick_price = common::roundUpToTickSize(entry_price);
        std::string price_str = common::priceToString(tick_price);

        nlohmann::json order_res;
        try {
            order_res = http_client_->placeOrder(market, "bid", vol_str, price_str, "limit");
        } catch (const std::exception& e) {
            result.error_message = e.what();
            return result;
        }

        if (!order_res.contains("uuid")) {
            result.error_message = "No UUID returned";
            return result;
        }

        std::string uuid = order_res["uuid"].get<std::string>();
        LOG_INFO("?耀붾굝?????????????밸쫫??꿔꺂????댁슦???????獄쏅챶留덌┼??뭬?怨좊뭽?(UUID: {}, ?????ル뒌????{:.0f}, ?????? {})", uuid, entry_price, vol_str);

        // 10???????????戮겸뵽 ????븐뼐????????????遺얘턁??????????(500ms * 20)
        for (int attempt = 0; attempt < 20; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            auto fill = verifyOrderFill(uuid, market, remaining);

            if (fill.filled_volume > 0.0) {
                total_filled += fill.filled_volume;
                total_funds += fill.avg_price * fill.filled_volume;
                remaining -= fill.filled_volume;
            }

            if (fill.is_filled || remaining <= 0.00000001) {
                break;
            }
        }

        if (remaining <= 0.00000001) {
            break;
        }

        // ??????곗뿨????嚥▲굧?먩뤆????猷멤꼻???????????癲???? ??????獄쏅챷???饔낅떽??????怨몃뮡???????????????????
        try {
            http_client_->cancelOrder(uuid);
            LOG_WARN("?耀붾굝???????????釉랁닑???롪퍓媛???猷매???????10?? -> ??????밸쫫??꿔꺂????댁슦???????????????????");
        } catch (const std::exception& e) {
            LOG_WARN("?耀붾굝?????????????밸쫫??꿔꺂????댁슦???????????????ㅼ뒩?? {}", e.what());
        }

        attempt_count++;
        result.retry_count = attempt_count;

        if (retry_wait_ms > 0 && attempt_count < max_attempts) {
            int wait_ms = retry_wait_ms * (1 + attempt_count);
            std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
        }

        try {
            auto orderbook = http_client_->getOrderBook(market);
            entry_price = calculateOptimalBuyPrice(market, entry_price, orderbook);
        } catch (const std::exception& e) {
            LOG_WARN("????????????????ш내?℡ㅇ?⑤뼰?踰????? ????⑥ル????????????ㅼ뒩?? {}", e.what());
        }
    }

    if (total_filled > 0.0) {
        result.success = true;
        result.executed_volume = total_filled;
        result.executed_price = total_funds / total_filled;
    } else {
        result.error_message = (attempt_count >= max_attempts) ? "Max retries exceeded" : "No fills";
    }

    return result;
}

TradingEngine::LimitOrderResult TradingEngine::executeLimitSellOrder(
    const std::string& market,
    double exit_price,
    double quantity,
    int max_retries,
    int retry_wait_ms
) {
    LimitOrderResult result{};
    result.success = false;
    result.retry_count = 0;
    result.executed_price = 0.0;
    result.executed_volume = 0.0;

    double remaining = quantity;
    double total_filled = 0.0;
    double total_funds = 0.0;

    int max_attempts = max_retries > 0 ? max_retries : 3;
    int attempt_count = 0;

    while (running_ && remaining > 0.00000001 && attempt_count < max_attempts) {
        std::stringstream ss;
        ss << std::fixed << std::setprecision(8) << remaining;
        std::string vol_str = ss.str();
        // [Phase 3] ??? ????????⑤벡苑??????+ ??? ???????????????⑤벡瑜????
        double tick_price = common::roundDownToTickSize(exit_price);
        std::string price_str = common::priceToString(tick_price);

        nlohmann::json order_res;
        try {
            order_res = http_client_->placeOrder(market, "ask", vol_str, price_str, "limit");
        } catch (const std::exception& e) {
            result.error_message = e.what();
            return result;
        }

        if (!order_res.contains("uuid")) {
            result.error_message = "No UUID returned";
            return result;
        }

        std::string uuid = order_res["uuid"].get<std::string>();
        LOG_INFO("?耀붾굝??????雅?퍔瑗?땟????????밸쫫??꿔꺂????댁슦???????獄쏅챶留덌┼??뭬?怨좊뭽?(UUID: {}, ?????ル뒌????{:.0f}, ?????? {})", uuid, exit_price, vol_str);

        // 10???????????戮겸뵽 ????븐뼐????????????遺얘턁??????????(500ms * 20)
        for (int attempt = 0; attempt < 20; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            auto fill = verifyOrderFill(uuid, market, remaining);

            if (fill.filled_volume > 0.0) {
                total_filled += fill.filled_volume;
                total_funds += fill.avg_price * fill.filled_volume;
                remaining -= fill.filled_volume;
            }

            if (fill.is_filled || remaining <= 0.00000001) {
                break;
            }
        }

        if (remaining <= 0.00000001) {
            break;
        }

        // ??????곗뿨????嚥▲굧?먩뤆????猷멤꼻???????????癲???? ??????獄쏅챷???饔낅떽??????怨몃뮡???????????????????
        try {
            http_client_->cancelOrder(uuid);
            LOG_WARN("?耀붾굝??????雅?퍔瑗?땟??????釉랁닑???롪퍓媛???猷매???????10?? -> ??????밸쫫??꿔꺂????댁슦???????????????????");
        } catch (const std::exception& e) {
            LOG_WARN("?耀붾굝??????雅?퍔瑗?땟????????밸쫫??꿔꺂????댁슦???????????????ㅼ뒩?? {}", e.what());
        }

        attempt_count++;
        result.retry_count = attempt_count;

        if (retry_wait_ms > 0 && attempt_count < max_attempts) {
            int wait_ms = retry_wait_ms * (1 + attempt_count);
            std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
        }

        try {
            auto orderbook = http_client_->getOrderBook(market);
            exit_price = calculateOptimalSellPrice(market, exit_price, orderbook);
        } catch (const std::exception& e) {
            LOG_WARN("????????????????ш내?℡ㅇ?⑤뼰?踰????? ????⑥ル????????????ㅼ뒩?? {}", e.what());
        }
    }

    if (total_filled > 0.0) {
        result.success = true;
        result.executed_volume = total_filled;
        result.executed_price = total_funds / total_filled;
    } else {
        result.error_message = (attempt_count >= max_attempts) ? "Max retries exceeded" : "No fills";
    }

    return result;
}


// ===== ????븐뼐??????????癲ル슢??????????????怨룸셾???????꾩룆梨띰쭕???=====

void TradingEngine::updateMetrics() {
    auto metrics = risk_manager_->getRiskMetrics();

    LOG_INFO("===== Metrics =====");
    LOG_INFO("Total capital: {:.0f} KRW ({:+.2f}%)",
             metrics.total_capital + metrics.invested_capital,
             metrics.total_pnl_pct * 100);
    LOG_INFO("Available/Invested: {:.0f} / {:.0f}",
             metrics.available_capital, metrics.invested_capital);
    LOG_INFO("Realized/Unrealized PnL: {:.0f} / {:.0f}",
             metrics.realized_pnl, metrics.unrealized_pnl);
    LOG_INFO("Trades: {} (W {} / L {}, WinRate {:.1f}%)",
             metrics.total_trades,
             metrics.winning_trades,
             metrics.losing_trades,
             metrics.win_rate * 100);
    LOG_INFO("Active positions: {}/{}, Drawdown: {:.2f}%",
             metrics.active_positions,
             metrics.max_positions,
             metrics.current_drawdown * 100);
    LOG_INFO("====================");
}
risk::RiskManager::RiskMetrics TradingEngine::getMetrics() const {
    return risk_manager_->getRiskMetrics();
}

std::vector<risk::Position> TradingEngine::getPositions() const {
    return risk_manager_->getAllPositions();
}

std::vector<risk::TradeHistory> TradingEngine::getTradeHistory() const {
    return risk_manager_->getTradeHistory();
}

// ===== ????뀀맩鍮??????됰씮????깅떔????????=====

void TradingEngine::manualScan() {
    LOG_INFO("???黎앸럽????븍쇊?용∥??????癲ル슢????????????");
    scanMarkets();
    generateSignals();
}

// void TradingEngine::manualClosePosition(const std::string& market) {
//     LOG_INFO("????뀀맩鍮??????됰씮????깅떔?????? {}", market);
    
//     auto* pos = risk_manager_->getPosition(market);
//     if (!pos) {
//         LOG_WARN("??????????????대첉?? {}", market);
//         return;
//     }
    
//     executeSellOrder(market, *pos, "manual", current_price);
// }

// void TradingEngine::manualCloseAll() {
//     LOG_INFO("??????袁⑸즴筌?씛彛???????????뀀맩鍮??????됰씮????깅떔??????);
    
//     auto positions = risk_manager_->getAllPositions();
//     for (const auto& pos : positions) {
//         executeSellOrder(pos.market, pos, "manual_all", current_price);
//     }
// }

// ===== ?????????(??????⑤슢堉??곕???? =====

double TradingEngine::getCurrentPrice(const std::string& market) {
    try {
        auto ticker = http_client_->getTicker(market);
        if (ticker.empty()) {
            return 0;
        }
        
        // 2. nlohmann/json ?????????????롮쾸?椰?嚥싲갭怡??????????????⑤벡瑜????
        if (ticker.is_array() && !ticker.empty()) {
            return ticker[0].value("trade_price", 0.0);
        }

        if (ticker.contains("trade_price") && !ticker["trade_price"].is_null()) {
            return ticker.value("trade_price", 0.0);
        }
        
        return 0;
        
    } catch (const std::exception& e) {
        LOG_ERROR("?????獄쏅챶留????⑥る뜤???????쎛 ????⑥ル????????????ㅼ뒩?? {} - {}", market, e.what());
        return 0;
    }
}

bool TradingEngine::hasEnoughBalance(double required_krw) {
    auto metrics = risk_manager_->getRiskMetrics();
    return metrics.available_capital >= required_krw;
}

void TradingEngine::logPerformance() {
    auto metrics = risk_manager_->getRiskMetrics();
    auto history = risk_manager_->getTradeHistory();

    long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    double runtime_hours = (now - start_time_) / (1000.0 * 60.0 * 60.0);

    LOG_INFO("========================================");
    LOG_INFO("Final performance report");
    LOG_INFO("========================================");
    LOG_INFO("Runtime: {:.1f} hours", runtime_hours);
    LOG_INFO("Scans: {}, Signals: {}, Trades: {}",
             total_scans_, total_signals_, metrics.total_trades);
    LOG_INFO("Capital summary");
    LOG_INFO("Initial capital: {:.0f} KRW", config_.initial_capital);
    LOG_INFO("Final capital: {:.0f} KRW", metrics.total_capital);
    LOG_INFO("Total PnL: {:.0f} KRW ({:+.2f}%)",
             metrics.total_pnl, metrics.total_pnl_pct * 100);

    LOG_INFO("Trade performance");
    LOG_INFO("Win rate: {:.1f}% ({}/{})",
             metrics.win_rate * 100,
             metrics.winning_trades,
             metrics.total_trades);
    LOG_INFO("Profit Factor: {:.2f}", metrics.profit_factor);
    LOG_INFO("Sharpe Ratio: {:.2f}", metrics.sharpe_ratio);
    LOG_INFO("Max Drawdown: {:.2f}%", metrics.max_drawdown * 100);

    LOG_INFO("Recent trades (last 10)");
    if (!history.empty()) {
        int count = 0;
        for (auto it = history.rbegin(); it != history.rend() && count < 10; ++it, ++count) {
            std::string status_mark = (it->profit_loss > 0) ? "WIN" : "LOSS";
            LOG_INFO("  {} {} | entry: {:.0f}, exit: {:.0f} | {:+.2f}% | {}",
                     status_mark, it->market,
                     it->entry_price,
                     it->exit_price,
                     it->profit_loss_pct * 100,
                     it->exit_reason);
        }
    } else {
        LOG_INFO("  No trade history");
    }

    LOG_INFO("Realtime monitoring metrics");
    LOG_INFO("Dynamic filter value: {:.3f}", dynamic_filter_value_);
    LOG_INFO("Position scale multiplier: {:.2f}", position_scale_multiplier_);
    LOG_INFO("Cumulative buy/sell orders: {} / {}",
             prometheus_metrics_.total_buy_orders,
             prometheus_metrics_.total_sell_orders);

    auto prom_metrics = exportPrometheusMetrics();
    LOG_INFO("Prometheus metrics preview: {}", prom_metrics.substr(0, 200) + "...");
    LOG_INFO("========================================");
}
void TradingEngine::syncAccountState() {
    LOG_INFO("??傭?끆???????????????거?뜮???????????????轅붽틓???壤굿??걜?..");

    try {
        auto accounts = http_client_->getAccounts();
        bool krw_found = false;
        std::set<std::string> wallet_markets;
        const std::size_t reconcile_candidates = pending_reconcile_positions_.size();
        std::size_t restored_positions = 0;
        std::size_t external_closes = 0;

        for (const auto& acc : accounts) {
            std::string currency = acc["currency"].get<std::string>();
            double balance = std::stod(acc["balance"].get<std::string>());
            double locked = std::stod(acc["locked"].get<std::string>());
            
            // 1. [??????⑤슢堉??곕???? KRW(???????????? ????븐뼐???????????癲ル슢??룸퀬苑?????????雅?퍔瑗?땟?????????熬곣몿???
            if (currency == "KRW") {
                double total_cash = balance + locked; // ???????????+ ??????곗뿨????嚥▲굧?먩뤆????猷멤꼻?????????????꿔꺂?????
                
                // RiskManager???????????⑤슢???????????????롮쾸?椰?嚥싲갭怡?????????????????????????????醫딇떍????
                risk_manager_->resetCapital(total_cash);
                // (2) [????????熬곣몿???] ????癲??????????롮쾸?椰???????????泥??'??????嶺뚮∥??????????????????롮쾸?椰?嚥싲갭怡?????????????????⑤벡瑜????
                config_.initial_capital = total_cash;
                krw_found = true;
                LOG_INFO("?????ш낄????????????????????? {:.0f} KRW (?????ル뒌????{:.0f})", total_cash, balance);
                continue; // ????????????????븐뼐???????????癲ル슢??룸퀬苑??????耀붾굝??????????????熬곣몿??????????롮쾸?椰??????????
            }

            // 2. ??????⑤벡瑜???? ??????꾩룆梨띰쭕????????븐뼐???????????癲ル슢??룸퀬苑??(???????????????雅?퍔瑗?땟??????)
            // ????븐뼐??????????????꾩룆梨띰쭕???????????밸븶筌믩끃????(?? BTC -> KRW-BTC)
            std::string market = "KRW-" + currency;
            wallet_markets.insert(market);
            
            double avg_buy_price = std::stod(acc["avg_buy_price"].get<std::string>());
            
            // ????븐뼐??????轅붽틓????????????Dust) ????轅붽틓???????(????븐뼐?곭춯?竊???????????븐뼐????????????????獄쏅챷???饔낅떽??????怨몃뮡???????댁뢿援???????????????꾩룆梨띰쭕???
            if (balance * avg_buy_price < 5000) continue;

            // ???? RiskManager?????????롮쾸?椰?嚥▲굧???븍툖??????????볧뀮???됀??嶺??
            if (risk_manager_->getPosition(market) != nullptr) continue;

            LOG_INFO("?????????????곕츥???? ?????諛몃마?????????밸븶筌믩끃???ル봿留싷┼?嶺?? {} (?????? {:.8f}, ???????: {:.0f})", 
                     market, balance, avg_buy_price);

            // [Phase 4] ?????????泥?????癲???沃섅닊?熬곣뫁????????耀붾굝??????????SL/TP ??????⑤벡瑜??꿔꺂?????????耀붾굝???????
            const PersistedPosition* persisted = nullptr;
            for (const auto& pp : pending_reconcile_positions_) {
                if (pp.market == market && pp.stop_loss > 0.0) {
                    persisted = &pp;
                    break;
                }
            }

            double safe_stop_loss;
            double tp1, tp2;
            double be_trigger = 0.0, trail_start = 0.0;
            bool half_closed = false;

            if (persisted) {
                // [Phase 4] ???????蹂㏓??嶺뚮㉡??????????????????袁⑸즴筌?씛彛????????(?????)
                safe_stop_loss = persisted->stop_loss;
                tp1 = persisted->take_profit_1;
                tp2 = persisted->take_profit_2;
                be_trigger = persisted->breakeven_trigger;
                trail_start = persisted->trailing_start;
                half_closed = persisted->half_closed;
                LOG_INFO("??????????곕츥?嶺뚮?爰??삠?熬곣뫂?????耀붾굝???????: {} SL={:.0f} TP1={:.0f} TP2={:.0f} BE={:.0f} TS={:.0f}",
                         market, safe_stop_loss, tp1, tp2, be_trigger, trail_start);
            } else {
                // ?????????泥????????????????????대첉??????????????(????븐뼐?????????????????? ?????????뀀맩鍮??????됰씮????깅떔??????븐뼐????????
                double target_sl = avg_buy_price * 0.97;
                double upbit_limit_sl = config_.min_order_krw / (balance > 0 ? balance : 1e-9);

                if (upbit_limit_sl > avg_buy_price * 0.99) {
                    LOG_WARN("{} balance too small; using conservative stop-loss (qty {:.6f}, upbit_min_sl {:.0f})",
                             market, balance, upbit_limit_sl);
                    safe_stop_loss = target_sl;
                } else {
                    safe_stop_loss = std::max(target_sl, upbit_limit_sl);
                }
                tp1 = avg_buy_price * 1.010;
                tp2 = avg_buy_price * 1.015;
                LOG_WARN("??????????곕츥?嶺뚮?爰??삠?熬곣뫂??????????????: {} SL={:.0f} TP1={:.0f} TP2={:.0f} (?耀붾굝???????????????????????ㅻ쑄??",
                         market, safe_stop_loss, tp1, tp2);
            }

            std::string recovered_strategy = "RECOVERED";
            auto recovered_it = recovered_strategy_map_.find(market);
            if (recovered_it != recovered_strategy_map_.end()) {
                recovered_strategy = recovered_it->second;
            }

            // ???????????⑤벡瑜??꿔꺂??????????썹땟????
            risk_manager_->enterPosition(
                market,
                avg_buy_price,
                balance,
                safe_stop_loss,
                tp1,
                tp2,
                recovered_strategy,
                be_trigger,
                trail_start
            );
            ++restored_positions;

            // [Phase 4] ???????깆궔??????????????椰??????????⑤벡瑜??꿔꺂??????
            if (half_closed) {
                auto* pos = risk_manager_->getPosition(market);
                if (pos) {
                    pos->half_closed = true;
                    LOG_INFO("  ?????堉온??????Β?レ름??????????거?뜮???????곕츥?嶺뚮?爰??? {} (half_closed=true)", market);
                }
            }
        }
        
        if (!krw_found) {
            LOG_WARN("??傭?끆??????????KRW?????ル뒌?? ??????源낆┰?????????곸죩! (??????硫멸킐???????0????????????μ떜媛?걫???");
            risk_manager_->resetCapital(0.0);
        }

        // ===== ??????椰???????遺얘턁????????(????븐뼐???????????????) =====
        if (!pending_reconcile_positions_.empty()) {
            std::vector<std::string> missing_markets;
            for (const auto& pos : pending_reconcile_positions_) {
                if (!pos.market.empty() && wallet_markets.find(pos.market) == wallet_markets.end()) {
                    missing_markets.push_back(pos.market);
                }
            }

            std::map<std::string, double> price_map;
            if (!missing_markets.empty()) {
                try {
                    auto tickers = http_client_->getTickerBatch(missing_markets);
                    for (const auto& t : tickers) {
                        std::string market_code = t["market"].get<std::string>();
                        double trade_price = t["trade_price"].get<double>();
                        price_map[market_code] = trade_price;
                    }
                } catch (const std::exception& e) {
                    LOG_WARN("????釉랁닑???雅?퍔瑗?땟?????????饔낅떽????????????????????⑥ル????????????ㅼ뒩?? {}", e.what());
                }
            }

            const double fee_rate = Config::getInstance().getFeeRate();
            for (const auto& pos : pending_reconcile_positions_) {
                if (pos.market.empty()) {
                    continue;
                }
                if (wallet_markets.find(pos.market) != wallet_markets.end()) {
                    continue;
                }

                double exit_price = pos.entry_price;
                auto price_it = price_map.find(pos.market);
                if (price_it != price_map.end()) {
                    exit_price = price_it->second;
                }

                double entry_value = pos.entry_price * pos.quantity;
                double exit_value = exit_price * pos.quantity;
                double entry_fee = entry_value * fee_rate;
                double exit_fee = exit_value * fee_rate;
                double profit_loss = exit_value - entry_value - entry_fee - exit_fee;

                risk::TradeHistory trade;
                trade.market = pos.market;
                trade.entry_price = pos.entry_price;
                trade.exit_price = exit_price;
                trade.quantity = pos.quantity;
                trade.entry_time = pos.entry_time;
                trade.exit_time = getCurrentTimestampMs();
                trade.strategy_name = pos.strategy_name;
                trade.exit_reason = "manual_external";
                trade.signal_filter = pos.signal_filter;
                trade.signal_strength = pos.signal_strength;
                trade.market_regime = pos.market_regime;
                trade.liquidity_score = pos.liquidity_score;
                trade.volatility = pos.volatility;
                trade.expected_value = pos.expected_value;
                trade.reward_risk_ratio = pos.reward_risk_ratio;
                trade.profit_loss = profit_loss;
                trade.profit_loss_pct = (pos.entry_price > 0.0)
                    ? (exit_price - pos.entry_price) / pos.entry_price
                    : 0.0;
                trade.fee_paid = entry_fee + exit_fee;

                risk_manager_->appendTradeHistory(trade);

                nlohmann::json close_payload;
                close_payload["exit_price"] = exit_price;
                close_payload["quantity"] = pos.quantity;
                close_payload["reason"] = "manual_external";
                close_payload["gross_pnl"] = profit_loss;
                close_payload["strategy_name"] = pos.strategy_name;
                appendJournalEvent(
                    core::JournalEventType::POSITION_CLOSED,
                    pos.market,
                    "reconcile_external_close",
                    close_payload
                );

                if (strategy_manager_ && !pos.strategy_name.empty()) {
                    auto strategy_ptr = strategy_manager_->getStrategy(pos.strategy_name);
                    if (strategy_ptr) {
                        strategy_ptr->updateStatistics(pos.market, profit_loss > 0.0, profit_loss);
                    }
                }

                LOG_INFO("????釉랁닑???雅?퍔瑗?땟?????????饔낅떽????????? {} (?????獄쏅챶留?? {}, ????? {:.0f})",
                         pos.market, pos.strategy_name, profit_loss);
                ++external_closes;
            }
        }

        LOG_INFO(
            "Account state sync summary: wallet_markets={}, reconcile_candidates={}, restored_positions={}, external_closes={}",
            wallet_markets.size(),
            reconcile_candidates,
            restored_positions,
            external_closes
        );
        pending_reconcile_positions_.clear();

        // ????븐뼐?????????????ル??????鶯ㅺ동?????룚????????轅붽틓?????????븐뼐??????????????????곕???????
        for (auto it = recovered_strategy_map_.begin(); it != recovered_strategy_map_.end(); ) {
            if (wallet_markets.find(it->first) == wallet_markets.end()) {
                it = recovered_strategy_map_.erase(it);
            } else {
                ++it;
            }
        }
        
        LOG_INFO("Account state synchronization completed");

    } catch (const std::exception& e) {
        LOG_ERROR("??傭?끆???????????????거?뜮???????????????????ㅼ뒩?? {}", e.what());
    }
}

void TradingEngine::runStatePersistence() {
    using namespace std::chrono_literals;
    while (state_persist_running_) {
        std::this_thread::sleep_for(30s);
        if (!state_persist_running_) {
            break;
        }
        saveState();
    }
}

void TradingEngine::appendJournalEvent(
    core::JournalEventType type,
    const std::string& market,
    const std::string& entity_id,
    const nlohmann::json& payload
) {
    if (!event_journal_) {
        return;
    }

    core::JournalEvent event;
    event.ts_ms = getCurrentTimestampMs();
    event.type = type;
    event.market = market;
    event.entity_id = entity_id;
    event.payload = payload;

    if (!event_journal_->append(event)) {
        LOG_WARN("Event journal append failed: type={}, market={}",
                 static_cast<int>(type), market);
    }
}

void TradingEngine::loadLearningState() {
    try {
        if (!learning_state_store_) {
            return;
        }

        auto loaded = learning_state_store_->load();
        if (!loaded.has_value()) {
            return;
        }

        const auto& policy = loaded->policy_params;
        if (policy.contains("dynamic_filter_value")) {
            dynamic_filter_value_ = std::clamp(
                policy.value("dynamic_filter_value", dynamic_filter_value_),
                0.35,
                0.70
            );
        }
        if (policy.contains("position_scale_multiplier")) {
            position_scale_multiplier_ = std::clamp(
                policy.value("position_scale_multiplier", position_scale_multiplier_),
                0.5,
                2.5
            );
        }

        LOG_INFO(
            "Learning state loaded (schema v{}, saved_at={}, filter={:.3f}, scale={:.2f})",
            loaded->schema_version,
            loaded->saved_at_ms,
            dynamic_filter_value_,
            position_scale_multiplier_
        );
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to load learning state: {}", e.what());
    }
}

void TradingEngine::saveLearningState() {
    try {
        if (!learning_state_store_) {
            return;
        }

        core::LearningStateSnapshot snapshot;
        snapshot.schema_version = 1;
        snapshot.saved_at_ms = getCurrentTimestampMs();
        snapshot.policy_params = nlohmann::json::object();
        snapshot.policy_params["dynamic_filter_value"] = dynamic_filter_value_;
        snapshot.policy_params["position_scale_multiplier"] = position_scale_multiplier_;

        nlohmann::json bucket_stats = nlohmann::json::array();
        if (performance_store_) {
            for (const auto& [key, stats] : performance_store_->byBucket()) {
                nlohmann::json row;
                row["strategy_name"] = key.strategy_name;
                row["regime"] = static_cast<int>(key.regime);
                row["liquidity_bucket"] = key.liquidity_bucket;
                row["trades"] = stats.trades;
                row["wins"] = stats.wins;
                row["gross_profit"] = stats.gross_profit;
                row["gross_loss_abs"] = stats.gross_loss_abs;
                row["net_profit"] = stats.net_profit;
                bucket_stats.push_back(std::move(row));
            }
        }
        snapshot.bucket_stats = std::move(bucket_stats);
        snapshot.rollback_point = nlohmann::json::object();
        snapshot.rollback_point["dynamic_filter_value"] = dynamic_filter_value_;
        snapshot.rollback_point["position_scale_multiplier"] = position_scale_multiplier_;

        if (!learning_state_store_->save(snapshot)) {
            LOG_WARN("Learning state save failed");
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to save learning state: {}", e.what());
    }
}

void TradingEngine::saveState() {
    try {
        nlohmann::json state;
        state["version"] = 1;
        state["timestamp"] = getCurrentTimestampMs();
        state["snapshot_last_event_seq"] = event_journal_ ? event_journal_->lastSeq() : 0;
        state["dynamic_filter_value"] = dynamic_filter_value_;
        state["position_scale_multiplier"] = position_scale_multiplier_;

        // Trade history
        nlohmann::json history = nlohmann::json::array();
        for (const auto& trade : risk_manager_->getTradeHistory()) {
            nlohmann::json item;
            item["market"] = trade.market;
            item["entry_price"] = trade.entry_price;
            item["exit_price"] = trade.exit_price;
            item["quantity"] = trade.quantity;
            item["profit_loss"] = trade.profit_loss;
            item["profit_loss_pct"] = trade.profit_loss_pct;
            item["fee_paid"] = trade.fee_paid;
            item["entry_time"] = trade.entry_time;
            item["exit_time"] = trade.exit_time;
            item["strategy_name"] = trade.strategy_name;
            item["exit_reason"] = trade.exit_reason;
            item["signal_filter"] = trade.signal_filter;
            item["signal_strength"] = trade.signal_strength;
            item["market_regime"] = static_cast<int>(trade.market_regime);
            item["liquidity_score"] = trade.liquidity_score;
            item["volatility"] = trade.volatility;
            item["expected_value"] = trade.expected_value;
            item["reward_risk_ratio"] = trade.reward_risk_ratio;
            history.push_back(item);
        }
        state["trade_history"] = history;

        // Strategy stats
        nlohmann::json stats_json;
        if (strategy_manager_) {
            auto stats_map = strategy_manager_->getAllStatistics();
            for (const auto& [name, stats] : stats_map) {
                nlohmann::json s;
                s["total_signals"] = stats.total_signals;
                s["winning_trades"] = stats.winning_trades;
                s["losing_trades"] = stats.losing_trades;
                s["total_profit"] = stats.total_profit;
                s["total_loss"] = stats.total_loss;
                s["win_rate"] = stats.win_rate;
                s["avg_profit"] = stats.avg_profit;
                s["avg_loss"] = stats.avg_loss;
                s["profit_factor"] = stats.profit_factor;
                s["sharpe_ratio"] = stats.sharpe_ratio;
                stats_json[name] = s;
            }
        }
        state["strategy_stats"] = stats_json;

        // Position strategy mapping
        nlohmann::json position_map;
        for (const auto& pos : risk_manager_->getAllPositions()) {
            if (!pos.strategy_name.empty()) {
                position_map[pos.market] = pos.strategy_name;
            }
        }
        state["position_strategy_map"] = position_map;

        // Open positions (for external close reconciliation)
        nlohmann::json open_positions = nlohmann::json::array();
        for (const auto& pos : risk_manager_->getAllPositions()) {
            nlohmann::json p;
            p["market"] = pos.market;
            p["strategy_name"] = pos.strategy_name;
            p["entry_price"] = pos.entry_price;
            p["quantity"] = pos.quantity;
            p["entry_time"] = pos.entry_time;
            p["signal_filter"] = pos.signal_filter;
            p["signal_strength"] = pos.signal_strength;
            p["market_regime"] = static_cast<int>(pos.market_regime);
            p["liquidity_score"] = pos.liquidity_score;
            p["volatility"] = pos.volatility;
            p["expected_value"] = pos.expected_value;
            p["reward_risk_ratio"] = pos.reward_risk_ratio;
            // [Phase 4] ?????????????遺얘턁????????????????????????泥???
            p["stop_loss"] = pos.stop_loss;
            p["take_profit_1"] = pos.take_profit_1;
            p["take_profit_2"] = pos.take_profit_2;
            p["breakeven_trigger"] = pos.breakeven_trigger;
            p["trailing_start"] = pos.trailing_start;
            p["half_closed"] = pos.half_closed;
            open_positions.push_back(p);
        }
        state["open_positions"] = open_positions;

        auto snapshot_path = utils::PathUtils::resolveRelativePath("state/snapshot_state.json");
        auto legacy_path = utils::PathUtils::resolveRelativePath("state/state.json");

        std::filesystem::create_directories(snapshot_path.parent_path());

        auto write_json = [&](const std::filesystem::path& path) {
            std::ofstream out(path.string(), std::ios::binary | std::ios::trunc);
            if (!out.is_open()) {
                throw std::runtime_error("Failed to open state file: " + path.string());
            }
            out << state.dump(2);
        };

        // Stage 3 deepening: primary snapshot path.
        write_json(snapshot_path);
        // Backward compatibility with existing tooling/loaders.
        write_json(legacy_path);

        LOG_INFO(
            "State snapshot saved: snapshot={}, legacy={}, last_event_seq={}",
            snapshot_path.string(),
            legacy_path.string(),
            state.value("snapshot_last_event_seq", 0)
        );

        saveLearningState();
    } catch (const std::exception& e) {
        LOG_ERROR("??????거?뜮?????????????ㅼ뒩?? {}", e.what());
    }
}

void TradingEngine::loadState() {
    try {
        auto snapshot_path = utils::PathUtils::resolveRelativePath("state/snapshot_state.json");
        auto legacy_path = utils::PathUtils::resolveRelativePath("state/state.json");
        std::filesystem::path state_path;

        if (std::filesystem::exists(snapshot_path)) {
            state_path = snapshot_path;
        } else if (std::filesystem::exists(legacy_path)) {
            state_path = legacy_path;
        } else {
            return;
        }

        std::ifstream in(state_path.string(), std::ios::binary);
        if (!in.is_open()) {
            return;
        }
        nlohmann::json state;
        in >> state;

        const long long snapshot_ts_ms = state.value("timestamp", 0LL);
        const std::uint64_t snapshot_last_seq = state.value(
            "snapshot_last_event_seq",
            static_cast<std::uint64_t>(0)
        );

        LOG_INFO(
            "State snapshot loaded: path={}, timestamp={}, last_event_seq={}",
            state_path.string(),
            snapshot_ts_ms,
            snapshot_last_seq
        );

        dynamic_filter_value_ = state.value("dynamic_filter_value", dynamic_filter_value_);
        position_scale_multiplier_ = state.value("position_scale_multiplier", position_scale_multiplier_);

        if (state.contains("trade_history") && state["trade_history"].is_array()) {
            std::vector<risk::TradeHistory> history;
            for (const auto& item : state["trade_history"]) {
                risk::TradeHistory trade;
                trade.market = item.value("market", "");
                trade.entry_price = item.value("entry_price", 0.0);
                trade.exit_price = item.value("exit_price", 0.0);
                trade.quantity = item.value("quantity", 0.0);
                trade.profit_loss = item.value("profit_loss", 0.0);
                trade.profit_loss_pct = item.value("profit_loss_pct", 0.0);
                trade.fee_paid = item.value("fee_paid", 0.0);
                trade.entry_time = item.value("entry_time", 0LL);
                trade.exit_time = item.value("exit_time", 0LL);
                trade.strategy_name = item.value("strategy_name", "");
                trade.exit_reason = item.value("exit_reason", "");
                trade.signal_filter = item.value("signal_filter", 0.5);
                trade.signal_strength = item.value("signal_strength", 0.0);
                trade.market_regime = static_cast<analytics::MarketRegime>(item.value("market_regime", static_cast<int>(analytics::MarketRegime::UNKNOWN)));
                trade.liquidity_score = item.value("liquidity_score", 0.0);
                trade.volatility = item.value("volatility", 0.0);
                trade.expected_value = item.value("expected_value", 0.0);
                trade.reward_risk_ratio = item.value("reward_risk_ratio", 0.0);
                history.push_back(trade);
            }
            risk_manager_->replaceTradeHistory(history);
            LOG_INFO("State restore: trade history {} loaded", history.size());
        }

        if (state.contains("strategy_stats") && state["strategy_stats"].is_object() && strategy_manager_) {
            for (const auto& strategy : strategy_manager_->getStrategies()) {
                auto info = strategy->getInfo();
                if (!state["strategy_stats"].contains(info.name)) {
                    continue;
                }
                const auto& s = state["strategy_stats"][info.name];
                strategy::IStrategy::Statistics stats;
                stats.total_signals = s.value("total_signals", 0);
                stats.winning_trades = s.value("winning_trades", 0);
                stats.losing_trades = s.value("losing_trades", 0);
                stats.total_profit = s.value("total_profit", 0.0);
                stats.total_loss = s.value("total_loss", 0.0);
                stats.win_rate = s.value("win_rate", 0.0);
                stats.avg_profit = s.value("avg_profit", 0.0);
                stats.avg_loss = s.value("avg_loss", 0.0);
                stats.profit_factor = s.value("profit_factor", 0.0);
                stats.sharpe_ratio = s.value("sharpe_ratio", 0.0);
                strategy->setStatistics(stats);
            }
            LOG_INFO("State restore: strategy statistics loaded");
        }

        recovered_strategy_map_.clear();
        if (state.contains("position_strategy_map") && state["position_strategy_map"].is_object()) {
            for (auto it = state["position_strategy_map"].begin(); it != state["position_strategy_map"].end(); ++it) {
                recovered_strategy_map_[it.key()] = it.value().get<std::string>();
            }
        }

        pending_reconcile_positions_.clear();
        if (state.contains("open_positions") && state["open_positions"].is_array()) {
            for (const auto& p : state["open_positions"]) {
                PersistedPosition pos;
                pos.market = p.value("market", "");
                pos.strategy_name = p.value("strategy_name", "");
                pos.entry_price = p.value("entry_price", 0.0);
                pos.quantity = p.value("quantity", 0.0);
                pos.entry_time = p.value("entry_time", 0LL);
                pos.signal_filter = p.value("signal_filter", 0.5);
                pos.signal_strength = p.value("signal_strength", 0.0);
                pos.market_regime = static_cast<analytics::MarketRegime>(p.value("market_regime", static_cast<int>(analytics::MarketRegime::UNKNOWN)));
                pos.liquidity_score = p.value("liquidity_score", 0.0);
                pos.volatility = p.value("volatility", 0.0);
                pos.expected_value = p.value("expected_value", 0.0);
                pos.reward_risk_ratio = p.value("reward_risk_ratio", 0.0);
                // [Phase 4] ?????????????遺얘턁?????????????????????⑤벡瑜??꿔꺂??????
                pos.stop_loss = p.value("stop_loss", 0.0);
                pos.take_profit_1 = p.value("take_profit_1", 0.0);
                pos.take_profit_2 = p.value("take_profit_2", 0.0);
                pos.breakeven_trigger = p.value("breakeven_trigger", 0.0);
                pos.trailing_start = p.value("trailing_start", 0.0);
                pos.half_closed = p.value("half_closed", false);
                if (!pos.market.empty() && pos.entry_price > 0.0 && pos.quantity > 0.0) {
                    pending_reconcile_positions_.push_back(pos);
                }
            }
        }

        if (event_journal_) {
            const std::uint64_t replay_start_seq = (snapshot_last_seq > 0) ? (snapshot_last_seq + 1) : 1;
            auto replay_events = event_journal_->readFrom(replay_start_seq);

            std::map<std::string, PersistedPosition> open_map;
            for (const auto& pos : pending_reconcile_positions_) {
                if (!pos.market.empty()) {
                    open_map[pos.market] = pos;
                }
            }
            std::map<std::string, std::string> strategy_map = recovered_strategy_map_;
            std::vector<risk::TradeHistory> replay_trades;

            auto upsert_position = [&](const std::string& market) -> PersistedPosition& {
                auto it = open_map.find(market);
                if (it == open_map.end()) {
                    PersistedPosition pos;
                    pos.market = market;
                    it = open_map.emplace(market, std::move(pos)).first;
                }
                return it->second;
            };

            std::size_t replay_applied = 0;
            for (const auto& event : replay_events) {
                // Legacy snapshots do not carry seq watermark. In that case, replay only newer events by timestamp.
                if (snapshot_last_seq == 0 && snapshot_ts_ms > 0 && event.ts_ms <= snapshot_ts_ms) {
                    continue;
                }
                if (event.market.empty()) {
                    continue;
                }

                const auto& payload = event.payload;
                switch (event.type) {
                    case core::JournalEventType::POSITION_OPENED: {
                        PersistedPosition& pos = upsert_position(event.market);
                        pos.market = event.market;
                        pos.entry_price = payload.value("entry_price", pos.entry_price);
                        pos.quantity = payload.value("quantity", pos.quantity);
                        pos.entry_time = (pos.entry_time > 0) ? pos.entry_time : event.ts_ms;
                        const std::string strategy_name = payload.value("strategy_name", pos.strategy_name);
                        if (!strategy_name.empty()) {
                            pos.strategy_name = strategy_name;
                            strategy_map[event.market] = strategy_name;
                        }
                        if (payload.contains("stop_loss")) pos.stop_loss = payload.value("stop_loss", pos.stop_loss);
                        if (payload.contains("take_profit_1")) pos.take_profit_1 = payload.value("take_profit_1", pos.take_profit_1);
                        if (payload.contains("take_profit_2")) pos.take_profit_2 = payload.value("take_profit_2", pos.take_profit_2);
                        if (payload.contains("breakeven_trigger")) pos.breakeven_trigger = payload.value("breakeven_trigger", pos.breakeven_trigger);
                        if (payload.contains("trailing_start")) pos.trailing_start = payload.value("trailing_start", pos.trailing_start);
                        if (payload.contains("half_closed")) pos.half_closed = payload.value("half_closed", pos.half_closed);
                        ++replay_applied;
                        break;
                    }
                    case core::JournalEventType::FILL_APPLIED: {
                        std::string side = payload.value("side", "");
                        side = toLowerCopy(side);
                        if (!side.empty() && side != "buy") {
                            break;
                        }
                        const double fill_qty = payload.value("filled_volume", 0.0);
                        const double avg_price = payload.value("avg_price", 0.0);
                        if (fill_qty <= 0.0 || avg_price <= 0.0) {
                            break;
                        }
                        if (open_map.find(event.market) != open_map.end()) {
                            break;
                        }

                        PersistedPosition& pos = upsert_position(event.market);
                        pos.market = event.market;
                        pos.entry_price = avg_price;
                        pos.quantity = fill_qty;
                        pos.entry_time = event.ts_ms;
                        const std::string strategy_name = payload.value("strategy_name", pos.strategy_name);
                        if (!strategy_name.empty()) {
                            pos.strategy_name = strategy_name;
                            strategy_map[event.market] = strategy_name;
                        }
                        pos.stop_loss = payload.value("stop_loss", pos.stop_loss);
                        pos.take_profit_1 = payload.value("take_profit_1", pos.take_profit_1);
                        pos.take_profit_2 = payload.value("take_profit_2", pos.take_profit_2);
                        ++replay_applied;
                        break;
                    }
                    case core::JournalEventType::POSITION_REDUCED: {
                        auto it = open_map.find(event.market);
                        if (it == open_map.end()) {
                            break;
                        }
                        const double reduced_qty = payload.value("quantity", 0.0);
                        const double exit_price = payload.value("exit_price", 0.0);
                        double applied_qty = reduced_qty;
                        if (applied_qty > 0.0 && applied_qty <= it->second.quantity + 1e-12) {
                            if (exit_price > 0.0 && it->second.entry_price > 0.0) {
                                risk::TradeHistory trade;
                                trade.market = event.market;
                                trade.entry_price = it->second.entry_price;
                                trade.exit_price = exit_price;
                                trade.quantity = applied_qty;
                                trade.entry_time = it->second.entry_time;
                                trade.exit_time = event.ts_ms;
                                trade.strategy_name = it->second.strategy_name;
                                trade.exit_reason = payload.value("reason", std::string("position_reduced_replay"));
                                trade.signal_filter = it->second.signal_filter;
                                trade.signal_strength = it->second.signal_strength;
                                trade.market_regime = it->second.market_regime;
                                trade.liquidity_score = it->second.liquidity_score;
                                trade.volatility = it->second.volatility;
                                trade.expected_value = it->second.expected_value;
                                trade.reward_risk_ratio = it->second.reward_risk_ratio;
                                trade.profit_loss = payload.value(
                                    "gross_pnl",
                                    (exit_price - it->second.entry_price) * applied_qty
                                );
                                trade.profit_loss_pct = (it->second.entry_price > 0.0)
                                    ? ((exit_price - it->second.entry_price) / it->second.entry_price)
                                    : 0.0;
                                trade.fee_paid = 0.0;
                                replay_trades.push_back(std::move(trade));
                            }
                            it->second.quantity = std::max(0.0, it->second.quantity - applied_qty);
                        }
                        if (it->second.quantity <= 1e-12) {
                            open_map.erase(it);
                            strategy_map.erase(event.market);
                        }
                        ++replay_applied;
                        break;
                    }
                    case core::JournalEventType::POSITION_CLOSED: {
                        auto it = open_map.find(event.market);
                        if (it != open_map.end()) {
                            double close_qty = payload.value("quantity", 0.0);
                            if (close_qty <= 0.0 || close_qty > it->second.quantity + 1e-12) {
                                close_qty = it->second.quantity;
                            }
                            const double exit_price = payload.value("exit_price", 0.0);
                            if (close_qty > 0.0 && exit_price > 0.0 && it->second.entry_price > 0.0) {
                                risk::TradeHistory trade;
                                trade.market = event.market;
                                trade.entry_price = it->second.entry_price;
                                trade.exit_price = exit_price;
                                trade.quantity = close_qty;
                                trade.entry_time = it->second.entry_time;
                                trade.exit_time = event.ts_ms;
                                trade.strategy_name = it->second.strategy_name;
                                trade.exit_reason = payload.value("reason", std::string("position_closed_replay"));
                                trade.signal_filter = it->second.signal_filter;
                                trade.signal_strength = it->second.signal_strength;
                                trade.market_regime = it->second.market_regime;
                                trade.liquidity_score = it->second.liquidity_score;
                                trade.volatility = it->second.volatility;
                                trade.expected_value = it->second.expected_value;
                                trade.reward_risk_ratio = it->second.reward_risk_ratio;
                                trade.profit_loss = payload.value(
                                    "gross_pnl",
                                    (exit_price - it->second.entry_price) * close_qty
                                );
                                trade.profit_loss_pct = (it->second.entry_price > 0.0)
                                    ? ((exit_price - it->second.entry_price) / it->second.entry_price)
                                    : 0.0;
                                trade.fee_paid = 0.0;
                                replay_trades.push_back(std::move(trade));
                            }
                            open_map.erase(it);
                        }
                        strategy_map.erase(event.market);
                        ++replay_applied;
                        break;
                    }
                    default:
                        break;
                }
            }

            for (const auto& trade : replay_trades) {
                risk_manager_->appendTradeHistory(trade);
            }

            pending_reconcile_positions_.clear();
            for (const auto& [market, pos] : open_map) {
                if (market.empty() || pos.entry_price <= 0.0 || pos.quantity <= 0.0) {
                    continue;
                }
                pending_reconcile_positions_.push_back(pos);
            }
            recovered_strategy_map_ = std::move(strategy_map);

            if (replay_applied > 0) {
                LOG_INFO(
                    "State restore: journal replay applied {} event(s), reconstructed trades={}, open positions={}",
                    replay_applied,
                    replay_trades.size(),
                    pending_reconcile_positions_.size()
                );
            } else {
                LOG_INFO(
                    "State restore: no replay events applied (start_seq={}, journal_last_seq={})",
                    replay_start_seq,
                    event_journal_->lastSeq()
                );
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR("??????거?뜮???????곕츥?嶺뚮?爰??삠?熬곣뫂???????????ㅼ뒩?? {}", e.what());
    }
}

// ===== [NEW] ?????關?쒎첎?嫄?嶺뚮슢梨뜹ㅇ???????????뀀?????됰Ŧ??怨뺢께?????????????????(??????⑤벡瑜??????????????????????????? =====

double TradingEngine::calculateDynamicFilterValue() {
    if (scanned_markets_.empty()) {
        return dynamic_filter_value_;
    }

    double total_volatility = 0.0;
    for (const auto& metrics : scanned_markets_) {
        total_volatility += metrics.volatility;
    }
    const double avg_volatility = total_volatility / static_cast<double>(scanned_markets_.size());

    // Base filter from market volatility.
    double new_filter_value = 0.40;
    if (avg_volatility < 0.3) {
        new_filter_value = 0.40 + (0.3 - avg_volatility) * 0.1667;  // tighter in calm markets
    } else if (avg_volatility > 0.7) {
        new_filter_value = 0.40 - (avg_volatility - 0.7) * 0.1667;  // looser in volatile markets
    }
    new_filter_value = std::clamp(new_filter_value, 0.35, 0.45);

    // Performance overlay from recent net PnL (already fee/slippage realized).
    auto history = risk_manager_->getTradeHistory();
    if (history.size() >= 20) {
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
        const double profit_factor =
            (gross_loss_abs > 1e-12) ? (gross_profit / gross_loss_abs) : ((gross_profit > 1e-12) ? 99.9 : 0.0);
        const double win_rate = static_cast<double>(wins) / static_cast<double>(sample_n);

        if (expectancy < 0.0 || profit_factor < 1.0) {
            new_filter_value += 0.02; // tighten entries when edge is negative
        } else if (expectancy > 0.0 && profit_factor > 1.2 && win_rate >= 0.50) {
            new_filter_value -= 0.01;
        }
    }

    new_filter_value = std::clamp(new_filter_value, 0.35, 0.55);
    if (std::abs(new_filter_value - dynamic_filter_value_) > 0.01) {
        LOG_INFO("Dynamic filter update: {:.3f} -> {:.3f} (avg_vol {:.3f})",
                 dynamic_filter_value_, new_filter_value, avg_volatility);
    }

    dynamic_filter_value_ = new_filter_value;
    return dynamic_filter_value_;
}

// ===== [NEW] ???????? ?????獄쏅챶留덌┼???????????????????(Win Rate & Profit Factor ??????????????? =====

double TradingEngine::calculatePositionScaleMultiplier() {
    // ????????????????:
    // Win Rate >= 60% AND Profit Factor >= 1.5 ?????????? ???????롮쾸?椰???⑤８理??
    // 
    // ??????????獄쏅챶留덌┼?????????癲됱빖???嶺????
    // - WR < 45% || PF < 1.0: 0.5??(??????????뀀????????熬곣몿????
    // - 45% <= WR < 50% || 1.0 <= PF < 1.2: 0.75??(??????⑤벡瑜????
    // - 50% <= WR < 60% || 1.2 <= PF < 1.5: 1.0??(???)
    // - WR >= 60% && PF >= 1.5: 1.5??2.5??(???)
    
    auto metrics = risk_manager_->getRiskMetrics();
    
    // ????븐뼐?곭춯?竊???????????????????깆궔?????????????????됰Ŧ??????????? ?????獄쏅챶留덌┼??????????
    if (metrics.total_trades < 20) {
        LOG_INFO("Not enough trades for position scaling ({}/20). keep 1.0x", metrics.total_trades);
        return 1.0;
    }
    
    double win_rate = metrics.win_rate;
    double profit_factor = metrics.profit_factor;
    
    double new_multiplier;
    
    if (win_rate < 0.45 || profit_factor < 1.0) {
        // ?????嚥????????ㅼ뒧??띤겫???????????얜?裕??? ???????繹먮굞???????????????뀀????????熬곣몿????
        new_multiplier = 0.5;
    } else if (win_rate < 0.50 || profit_factor < 1.2) {
        // ??????⑤벡瑜??????????嚥????????ㅼ뒧??띤겫??????????????????????
        new_multiplier = 0.75;
    } else if (win_rate < 0.60 || profit_factor < 1.5) {
        // ??? ?????嚥????????ㅼ뒧??띤겫?????????????????
        new_multiplier = 1.0;
    } else {
        // ??????????????嚥????????ㅼ뒧??띤겫??????? ???????ル?????
        // PF?? WR???????????????????耀붾굝?????????????獄쏅챶留덌┼?????????癲됱빖???嶺????
        // WR 60%~75%: 1.5??2.0?? PF 1.5~2.5: ??????熬곣몿??? 0.25??
        double wr_bonus = (win_rate - 0.60) * 10.0;  // 0~1.5
        double pf_bonus = std::min(0.5, (profit_factor - 1.5) * 0.5);  // 0~0.5
        new_multiplier = 1.5 + wr_bonus + pf_bonus;
        new_multiplier = std::min(2.5, new_multiplier);  // ????븐뼐????????? 2.5??
    }
    
    // ???????
    if (std::abs(new_multiplier - position_scale_multiplier_) > 0.01) {
        LOG_INFO("Position scale update: {:.2f}x -> {:.2f}x (WR: {:.1f}%, PF: {:.2f}, trades: {})",
                 position_scale_multiplier_, new_multiplier,
                 win_rate * 100.0, profit_factor, metrics.total_trades);
    }
    
    position_scale_multiplier_ = new_multiplier;
    return new_multiplier;
}

// ===== [NEW] ML ???????????????????븐뼐????????????????????뀀?????됰Ŧ??怨뺢께?????????? =====

void TradingEngine::learnOptimalFilterValue() {
    // historical P&L ???????????????????????????뀀?????됰Ŧ??怨뺢께?????????????嚥???癲?????욱룕??癒λ돗???????????已???
    // ?????????
    // 1. ????븐뼐?곭춯?竊???????????????signal_filter ??????????????????????????븐뼐?곭춯?竊????????????????已???????
    // 2. ????????????뀀?????됰Ŧ??怨뺢께?????????????????嚥???癲?????욱룕??癒λ돗?????븐뼐????????????????????(Win Rate, Profit Factor, Sharpe Ratio)
    // 3. ????븐뼐???????????????嚥????????ㅼ뒧??띤겫????????????뀀?????됰Ŧ??怨뺢께???????????熬곣몿????
    
    auto history = risk_manager_->getTradeHistory();
    
    if (history.size() < 50) {
        LOG_INFO("Not enough samples for filter learning ({}/50), skipping", history.size());
        return;
    }
    
    // ??????????뀀?????됰Ŧ??怨뺢께????????????븐뼐?곭춯?竊????????????????已??????????????嚥???癲?????욱룕??癒λ돗?????????????
    std::map<double, std::vector<TradeHistory>> trades_by_filter;
    std::map<double, std::vector<double>> returns_by_filter;  // Sharpe Ratio ?????????????
    
    // ??????????뀀?????됰Ŧ??怨뺢께????????????(0.45 ~ 0.55, 0.01 ????????⑤벡苑?
    for (double filter = 0.45; filter <= 0.55; filter += 0.01) {
        trades_by_filter[filter] = std::vector<TradeHistory>();
        returns_by_filter[filter] = std::vector<double>();
    }
    
    // 1. ????븐뼐?곭춯?竊??????????????????????뀀?????됰Ŧ??怨뺢께?????????????????????已???????
    for (const auto& trade : history) {
        // signal_filter?????????ル????????????ル????????????????0.01 ????????⑤벡苑?????????獄쏅챶留덌┼?????????雅?굛肄?????
        double rounded_filter = std::round(trade.signal_filter * 100.0) / 100.0;
        
        // ???????????????????뀀?????됰Ŧ??怨뺢께???????????????遺얘턁??????????
        if (rounded_filter >= 0.45 && rounded_filter <= 0.55) {
            trades_by_filter[rounded_filter].push_back(trade);
            returns_by_filter[rounded_filter].push_back(trade.profit_loss_pct);
        }
    }
    
    // 2. ????????????뀀?????됰Ŧ??怨뺢께?????????????????嚥???癲?????욱룕??癒λ돗???????????已???
    struct FilterPerformance {
        double filter_value;
        int trade_count;
        double win_rate;
        double avg_return;
        double profit_factor;
        double sharpe_ratio;
        double total_pnl;
        
        FilterPerformance()
            : filter_value(0), trade_count(0), win_rate(0)
            , avg_return(0), profit_factor(0), sharpe_ratio(0), total_pnl(0)
        {}
    };
    
    std::map<double, FilterPerformance> performances;
    double best_sharpe = -999.0;
    double best_filter = 0.5;
    
    for (auto& [filter_val, trades] : trades_by_filter) {
        if (trades.empty()) continue;
        
        FilterPerformance perf;
        perf.filter_value = filter_val;
        perf.trade_count = static_cast<int>(trades.size());
        
        // Win Rate ????????????
        int winning_trades = 0;
        double total_profit = 0.0;
        double total_loss = 0.0;  // ??????븍툖????????????
        
        for (const auto& trade : trades) {
            if (trade.profit_loss > 0) {
                winning_trades++;
                total_profit += trade.profit_loss;
            } else {
                total_loss += std::abs(trade.profit_loss);  // ??????븍툖?????????????????ル????????
            }
        }
        
        perf.win_rate = static_cast<double>(winning_trades) / trades.size();
        perf.total_pnl = total_profit - total_loss;
        
        // Profit Factor ????????????(????????⑤슢堉??곕????/ ???????
        perf.profit_factor = (total_loss > 0) ? (total_profit / total_loss) : total_profit;
        
        // ???????????⑤슢堉??곕?????
        perf.avg_return = perf.total_pnl / trades.size();
        
        // Sharpe Ratio ????????????(??????醫딇떍?????繹먮굞議??????????????????????⑤슢堉??곕?????
        const auto& returns = returns_by_filter[filter_val];
        if (returns.size() > 1) {
            double mean_return = 0.0;
            for (double ret : returns) {
                mean_return += ret;
            }
            mean_return /= returns.size();
            
            // ??????遺얘턁?????????믩베???????????????
            double variance = 0.0;
            for (double ret : returns) {
                double diff = ret - mean_return;
                variance += diff * diff;
            }
            variance /= returns.size();
            double std_dev = std::sqrt(variance);
            
            // Sharpe Ratio = (???????????⑤슢堉??곕?????- ????轅붽틓???????????? / ??????遺얘턁?????????믩베???
            // ????轅붽틓????????????0???????????????ル?????
            perf.sharpe_ratio = (std_dev > 0.0001) ? (mean_return / std_dev) : 0.0;
        }
        
        performances[filter_val] = perf;
        
        // ????븐뼐????????????????????뀀?????됰Ŧ??怨뺢께?????????傭?끆????????(Sharpe Ratio ???????)
        if (perf.sharpe_ratio > best_sharpe) {
            best_sharpe = perf.sharpe_ratio;
            best_filter = filter_val;
        }
        
        LOG_INFO("Filter {:.2f}: trades {}, win {:.1f}%, PF {:.2f}, Sharpe {:.3f}, net {:.0f}",
                 filter_val, perf.trade_count, perf.win_rate * 100.0, 
                 perf.profit_factor, perf.sharpe_ratio, perf.total_pnl);
    }
    
    // 3. ??癲됱빖???嶺??????????????已???????????熬곣몿????
    // ??????熬곣몿??? ??????????????? Win Rate >= 50% ??Profit Factor >= 1.2 ??????????뀀??(???????롮쾸?椰????
    std::vector<double> qualified_filters;
    for (auto& [filter_val, perf] : performances) {
        if (perf.win_rate >= 0.50 && perf.profit_factor >= 1.2 && perf.trade_count >= 10) {
            qualified_filters.push_back(filter_val);
        }
    }
    
    if (!qualified_filters.empty()) {
        // ????????????????????뀀????????ш끽踰椰?????袁ㅻ쇀????Sharpe Ratio ????븐뼐????????????????傭?끆????????
        double best_qualified_sharpe = -999.0;
        for (double f : qualified_filters) {
            if (performances[f].sharpe_ratio > best_qualified_sharpe) {
                best_qualified_sharpe = performances[f].sharpe_ratio;
                best_filter = f;
            }
        }
        
        LOG_INFO("ML filter learning (qualified set):");
        LOG_INFO("  best filter {:.2f} (Sharpe {:.3f}, win {:.1f}%, PF {:.2f})",
                 best_filter, best_qualified_sharpe,
                 performances[best_filter].win_rate * 100.0,
                 performances[best_filter].profit_factor);
    } else {
        // ????????????????????뀀?????됰Ŧ??怨뺢께????????????쎛 ?????????대첉??轅붽틓?????獒뺣폍????????袁⑸즴筌?씛彛??????Sharpe ????븐뼐????????????
        LOG_WARN("ML filter learning fallback (no qualified set).");
        LOG_WARN("  best Sharpe filter {:.2f} (Sharpe {:.3f})", best_filter, best_sharpe);
    }
    
    // [FIX] ?????關?쒎첎?嫄?嶺뚮슢梨뜹ㅇ???????????뀀?????됰Ŧ??怨뺢께??????????????怨룸셾???????꾩룆梨띰쭕???(????븐뼐????????????濚밸Ŧ?뤷젆????ㅼ뒧???????????獄쏅챶留덌┼???????
    if (std::abs(best_filter - dynamic_filter_value_) > 0.001) {
        double direction = (best_filter > dynamic_filter_value_) ? 1.0 : -1.0;
        dynamic_filter_value_ += direction * 0.01; // 0.01???????
        dynamic_filter_value_ = std::clamp(dynamic_filter_value_, 0.45, 0.55);
        
        LOG_INFO("Dynamic filter nudged: {:.2f} -> {:.2f}", 
                 dynamic_filter_value_ - (direction * 0.01), dynamic_filter_value_);
    }
    
    // ??????????뀀???????嚥???癲?????욱룕??癒λ돗??????????(??????熬곣몿??????????????已????
    filter_performance_history_[best_filter] = performances[best_filter].win_rate;
}

// ===== [NEW] Prometheus ????븐뼐??????????癲ル슢????????遺얘턁???????=====

std::string TradingEngine::exportPrometheusMetrics() const {
    // Prometheus ???遺얘턁???????????ㅿ폍???????븐뼐??????????癲ル슢?????????????????????밸븶筌믩끃????
    // Grafana?? ???????ш끽紐?????耀붾굝????????????????袁④뎬???????븐뼐???????????븐뼔???????????산뭐???????븐뼐????????
    
    auto metrics = risk_manager_->getRiskMetrics();
    auto timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    
    std::ostringstream oss;
    
    // ????븐뼐????????????????(??? ??????癲ル슢???먯춹???????熬곣몿???)
    oss << "# HELP autolife_state AutoLife ????븐뼐?곭춯?竊??????????癲?????????椰???????遺얘턁?????????n";
    oss << "# TYPE autolife_state gauge\n";

    // ????????????ㅳ늾??????????癲ル슢???먯춹?
    oss << "# HELP autolife_capital_total ???????(KRW)\n";
    oss << "# TYPE autolife_capital_total gauge\n";
    oss << "# HELP autolife_capital_available ???????????ル?????????款?蹂κ콡??????????????????????ル?????????? KRW)\n";
    oss << "# TYPE autolife_capital_available gauge\n";
    oss << "# HELP autolife_capital_invested ??????????ш끽踰椰?????袁ㅻ쇀?????????댁뢿援??????????????耀붾굝?????????????????????? KRW)\n";
    oss << "# TYPE autolife_capital_invested gauge\n";

    // ????????????ㅳ늾??????????癲ル슢???먯춹?
    oss << "# HELP autolife_pnl_realized ??????????????????袁⑸즴筌?씛彛?? KRW)\n";
    oss << "# TYPE autolife_pnl_realized gauge\n";
    oss << "# HELP autolife_pnl_unrealized ??????곗뿨????嚥▲굧?먩뤆???????????????袁⑸즴筌?씛彛??????????????, KRW)\n";
    oss << "# TYPE autolife_pnl_unrealized gauge\n";
    oss << "# HELP autolife_pnl_total ????????????????????곗뿨????嚥▲굧?먩뤆???? KRW)\n";
    oss << "# TYPE autolife_pnl_total gauge\n";
    oss << "# HELP autolife_pnl_total_pct ??????袁⑸즴筌?씛彛?????????????????⑤슢堉??곕?????%)\n";
    oss << "# TYPE autolife_pnl_total_pct gauge\n";

    // ??????醫딇떍?????繹먮굞議??????????ㅳ늾??????????癲ル슢???먯춹?
    oss << "# HELP autolife_drawdown_max ????븐뼐????????? ??????袁⑸즴筌?씛彛?????????????????????\n";
    oss << "# TYPE autolife_drawdown_max gauge\n";
    oss << "# HELP autolife_drawdown_current ??????袁⑸즴筌?씛彛?????????????????????\n";
    oss << "# TYPE autolife_drawdown_current gauge\n";

    // ????????????ㅳ늾??????????癲ル슢???먯춹?
    oss << "# HELP autolife_positions_active ??????袁⑸즴筌?씛彛????????⑤벡瑜???? ???????n";
    oss << "# TYPE autolife_positions_active gauge\n";
    oss << "# HELP autolife_positions_max ???????롮쾸?椰???⑤８理??????븐뼐????????? ???????n";
    oss << "# TYPE autolife_positions_max gauge\n";

    // ????븐뼐?곭춯?竊?????????????????癲ル슢???먯춹?
    oss << "# HELP autolife_trades_total ??????袁⑸즴筌?씛彛??????븐뼐?곭춯?竊????????n";
    oss << "# TYPE autolife_trades_total counter\n";
    oss << "# HELP autolife_trades_winning ??????袁⑸즴筌?씛彛????????⑤슢堉??곕????????븐뼐?곭춯?竊????????n";
    oss << "# TYPE autolife_trades_winning counter\n";
    oss << "# HELP autolife_trades_losing ??????袁⑸즴筌?씛彛???????????븐뼐?곭춯?竊????????n";
    oss << "# TYPE autolife_trades_losing counter\n";

    // ?????嚥????????ㅼ뒧??띤겫??????븐뼐??????????????癲ル슢???먯춹?
    oss << "# HELP autolife_winrate ??????諛몃마嶺뚮??????????0~1)\n";
    oss << "# TYPE autolife_winrate gauge\n";
    oss << "# HELP autolife_profit_factor ??????⑤슢堉??곕???????癲???Profit Factor)\n";
    oss << "# TYPE autolife_profit_factor gauge\n";
    oss << "# HELP autolife_sharpe_ratio ???????????븐뼐?????????????嚥????????ㅼ뒧??띤겫??????븐뼐?????????\n";
    oss << "# TYPE autolife_sharpe_ratio gauge\n";

    // ????癲?????????椰??????????癲ル슢???먯춹?
    oss << "# HELP autolife_engine_running ????癲??????????? ??????椰????1=??????????0=???)\n";
    oss << "# TYPE autolife_engine_running gauge\n";
    oss << "# HELP autolife_engine_scans_total ??????????????轅붽틓??????????????용맧硅\n";
    oss << "# TYPE autolife_engine_scans_total counter\n";
    oss << "# HELP autolife_engine_signals_total ???????밸븶筌믩끃??????????????됰젿????????붺몭????異??n";
    oss << "# TYPE autolife_engine_signals_total counter\n";

    // ?????關?쒎첎?嫄?嶺뚮슢梨뜹ㅇ???????????뀀?????????????癲ル슢???먯춹?
    oss << "# HELP autolife_filter_value_dynamic ?????關?쒎첎?嫄?嶺뚮슢梨뜹ㅇ???????????뀀?????됰Ŧ??怨뺢께?????(0~1)\n";
    oss << "# TYPE autolife_filter_value_dynamic gauge\n";
    oss << "# HELP autolife_position_scale_multiplier ???????? ?????獄쏅챶留덌┼???????n";
    oss << "# TYPE autolife_position_scale_multiplier gauge\n";

    // ????癲???????븐뼐?곭춯?竊??????????븐뼐??????????癲ル슢???????????癲ル슢???먯춹?
    oss << "# HELP autolife_buy_orders_total ??????袁⑸즴筌?씛彛??????븐뼐??????????????獄쏅챷???饔낅떽??????怨몃뮡????n";
    oss << "# TYPE autolife_buy_orders_total counter\n";
    oss << "# HELP autolife_sell_orders_total ??????袁⑸즴筌?씛彛??????븐뼐??????????遺븍き??????????獄쏅챷???饔낅떽??????怨몃뮡????n";
    oss << "# TYPE autolife_sell_orders_total counter\n";
    oss << "# HELP autolife_pnl_cumulative ??????袁⑸즴筌?씛彛???????????????????????????源낅펰??????????룸챷援???? KRW)\n";
    oss << "# TYPE autolife_pnl_cumulative gauge\n";
    
    // 1. ????????????ㅳ늾????????븐뼐??????????癲ル슢?????
    oss << "autolife_capital_total{mode=\"" 
        << (config_.mode == TradingMode::LIVE ? "LIVE" : "PAPER") << "\"} "
        << metrics.total_capital << " " << timestamp_ms << "\n";
    
    oss << "autolife_capital_available{} " << metrics.available_capital << " " << timestamp_ms << "\n";
    oss << "autolife_capital_invested{} " << metrics.invested_capital << " " << timestamp_ms << "\n";
    
    // 2. ????????????ㅳ늾????????븐뼐??????????癲ル슢?????
    oss << "autolife_pnl_realized{} " << metrics.realized_pnl << " " << timestamp_ms << "\n";
    oss << "autolife_pnl_unrealized{} " << metrics.unrealized_pnl << " " << timestamp_ms << "\n";
    oss << "autolife_pnl_total{} " << metrics.total_pnl << " " << timestamp_ms << "\n";
    oss << "autolife_pnl_total_pct{} " << metrics.total_pnl_pct << " " << timestamp_ms << "\n";
    
    // 3. ??????醫딇떍?????繹먮굞議??????????ㅳ늾????????븐뼐??????????癲ル슢?????
    oss << "autolife_drawdown_max{} " << metrics.max_drawdown << " " << timestamp_ms << "\n";
    oss << "autolife_drawdown_current{} " << metrics.current_drawdown << " " << timestamp_ms << "\n";
    
    // 4. ????????????ㅳ늾????????븐뼐??????????癲ル슢?????
    oss << "autolife_positions_active{} " << metrics.active_positions << " " << timestamp_ms << "\n";
    oss << "autolife_positions_max{} " << config_.max_positions << " " << timestamp_ms << "\n";
    
    // 5. ????븐뼐?곭춯?竊???????????
    oss << "autolife_trades_total{} " << metrics.total_trades << " " << timestamp_ms << "\n";
    oss << "autolife_trades_winning{} " << metrics.winning_trades << " " << timestamp_ms << "\n";
    oss << "autolife_trades_losing{} " << metrics.losing_trades << " " << timestamp_ms << "\n";
    
    // 6. ????븐뼐?곭춯?竊???????????嚥????????ㅼ뒧??띤겫??????븐뼐????????
    oss << "autolife_winrate{} " << metrics.win_rate << " " << timestamp_ms << "\n";
    oss << "autolife_profit_factor{} " << metrics.profit_factor << " " << timestamp_ms << "\n";
    oss << "autolife_sharpe_ratio{} " << metrics.sharpe_ratio << " " << timestamp_ms << "\n";
    
    // 7. ????癲?????????椰????????븐뼐??????????癲ル슢?????
    oss << "autolife_engine_running{} " << (running_ ? 1 : 0) << " " << timestamp_ms << "\n";
    oss << "autolife_engine_scans_total{} " << total_scans_ << " " << timestamp_ms << "\n";
    oss << "autolife_engine_signals_total{} " << total_signals_ << " " << timestamp_ms << "\n";
    
    // 8. [NEW] ?????關?쒎첎?嫄?嶺뚮슢梨뜹ㅇ???????????뀀???????????? ????븐뼐??????????癲ル슢?????
    oss << "autolife_filter_value_dynamic{} " << dynamic_filter_value_ << " " << timestamp_ms << "\n";
    oss << "autolife_position_scale_multiplier{} " << position_scale_multiplier_ << " " << timestamp_ms << "\n";
    
    // 9. [NEW] ????븐뼐?곭춯?竊??????????癲???????븐뼐??????????癲ル슢?????
    oss << "autolife_buy_orders_total{} " << prometheus_metrics_.total_buy_orders << " " << timestamp_ms << "\n";
    oss << "autolife_sell_orders_total{} " << prometheus_metrics_.total_sell_orders << " " << timestamp_ms << "\n";
    oss << "autolife_pnl_cumulative{} " << prometheus_metrics_.cumulative_realized_pnl << " " << timestamp_ms << "\n";
    
    oss << "# End of AutoLife Metrics\n";
    
    return oss.str();
}

// [NEW] Prometheus HTTP ???耀붾굝????????傭?????????筌뤾퍓諭?
void TradingEngine::runPrometheusHttpServer(int port) {
    prometheus_server_port_ = port;
    prometheus_server_running_ = true;
    
    LOG_INFO("Prometheus HTTP ???轅붽틓??????壤????轅붽틓???壤굿??걜?(???? {})", port);
    
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        LOG_ERROR("WSAStartup failed");
        prometheus_server_running_ = false;
        return;
    }
    
    SOCKET listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_socket == INVALID_SOCKET) {
        LOG_ERROR("Socket creation failed");
        prometheus_server_running_ = false;
        WSACleanup();
        return;
    }
    
    // ????????????????롮쾸?椰???(TIME_WAIT ??????椰????????????????????????ル?????
    int reuse = 1;
    if (setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, 
                   reinterpret_cast<char*>(&reuse), sizeof(reuse)) < 0) {
        LOG_WARN("setsockopt(SO_REUSEADDR) failed");
    }
    
    sockaddr_in server_addr = {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(static_cast<u_short>(port));
    
    // Use inet_pton instead of deprecated inet_addr
    if (inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr) != 1) {
        LOG_ERROR("inet_pton failed");
        closesocket(listen_socket);
        prometheus_server_running_ = false;
        WSACleanup();
        return;
    }
    
    if (bind(listen_socket, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) == SOCKET_ERROR) {
        LOG_ERROR("bind ???????ㅼ뒩??(???? {})", port);
        closesocket(listen_socket);
        prometheus_server_running_ = false;
        WSACleanup();
        return;
    }
    
    if (listen(listen_socket, 5) == SOCKET_ERROR) {
        LOG_ERROR("listen failed");
        closesocket(listen_socket);
        prometheus_server_running_ = false;
        WSACleanup();
        return;
    }
    
    LOG_INFO("Prometheus ?耀붾굝?????????筌롫㈇??????轅붽틓??????壤??????袁ⓦ걤?嶺뚯쉶?????렢????????獄쏅챶留??(http://localhost:{}/metrics)", port);
    
    // ???耀붾굝????????傭??????룸㎗?ルき????
    while (prometheus_server_running_) {
        sockaddr_in client_addr = {};
        int client_addr_size = sizeof(client_addr);
        
        // 5??????????袁⑸즴筌?씛彛??????????롮쾸?椰???
        timeval timeout;
        timeout.tv_sec = 5;
        timeout.tv_usec = 0;
        
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(listen_socket, &read_fds);
        
        int select_result = select(0, &read_fds, nullptr, nullptr, &timeout);
        if (select_result == 0) {
            // ????????袁⑸즴筌?씛彛???- ????????袁④뎬??????븐뼐???????????
            continue;
        }
        if (select_result == SOCKET_ERROR) {
            LOG_WARN("select failed");
            break;
        }
        
        SOCKET client_socket = accept(listen_socket, 
                                      reinterpret_cast<sockaddr*>(&client_addr), 
                                      &client_addr_size);
        if (client_socket == INVALID_SOCKET) {
            LOG_WARN("accept failed");
            continue;
        }
        
        // HTTP ????癲????????????숇????
        char buffer[4096] = {0};
        int recv_result = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
        
        if (recv_result > 0) {
            buffer[recv_result] = '\0';
            std::string request(buffer);
            
            // GET /metrics ???遺얘턁??????????
            if (request.find("GET /metrics") == 0) {
                // Prometheus ????븐뼐??????????癲ル슢????????????밸븶筌믩끃????
                std::string metrics = exportPrometheusMetrics();
                
                // HTTP ?????????????
                std::ostringstream response;
                response << "HTTP/1.1 200 OK\r\n"
                         << "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n"
                         << "Content-Length: " << metrics.length() << "\r\n"
                         << "Connection: close\r\n"
                         << "\r\n"
                         << metrics;
                
                std::string response_str = response.str();
                send(client_socket, response_str.c_str(), static_cast<int>(response_str.length()), 0);
            } 
            else if (request.find("GET /health") == 0) {
                // ????????븐뼐???????????????癲???????
                std::string health_response = "OK";
                std::ostringstream response;
                response << "HTTP/1.1 200 OK\r\n"
                         << "Content-Type: text/plain; charset=utf-8\r\n"
                         << "Content-Length: " <<health_response.length() << "\r\n"
                         << "Connection: close\r\n"
                         << "\r\n"
                         << health_response;
                
                std::string response_str = response.str();
                send(client_socket, response_str.c_str(), static_cast<int>(response_str.length()), 0);
            }
            else {
                // 404 ???????
                std::string error_response = "Not Found";
                std::ostringstream response;
                response << "HTTP/1.1 404 Not Found\r\n"
                         << "Content-Type: text/plain; charset=utf-8\r\n"
                         << "Content-Length: " << error_response.length() << "\r\n"
                         << "Connection: close\r\n"
                         << "\r\n"
                         << error_response;
                
                std::string response_str = response.str();
                send(client_socket, response_str.c_str(), static_cast<int>(response_str.length()), 0);
            }
        }
        
        closesocket(client_socket);
    }
    
    closesocket(listen_socket);
    WSACleanup();
    
    LOG_INFO("Prometheus HTTP server stopped");
}

} // namespace engine
} // namespace autolife



