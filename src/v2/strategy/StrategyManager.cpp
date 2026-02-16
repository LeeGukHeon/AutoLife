#include "v2/strategy/StrategyManager.h"
#include "common/Logger.h"
#include "common/Config.h"
#include "risk/RiskManager.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <map>

#undef max
#undef min

namespace autolife {
namespace strategy {
namespace {
std::string toLowerCopy(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

int directionFromType(SignalType type) {
    switch (type) {
        case SignalType::BUY:
        case SignalType::STRONG_BUY:
            return 1;
        case SignalType::SELL:
        case SignalType::STRONG_SELL:
            return -1;
        default:
            return 0;
    }
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
    double profitFactor() const {
        return (gross_loss_abs > 1e-12) ? (gross_profit / gross_loss_abs) : 0.0;
    }
    double winRate() const {
        return (trades > 0) ? (static_cast<double>(wins) / static_cast<double>(trades)) : 0.0;
    }
};

double estimateSignalRoundTripCostPctForManager(const Signal& signal) {
    const double fee_rate_per_side = autolife::Config::getInstance().getFeeRate();
    double slippage_per_side = std::clamp(
        autolife::Config::getInstance().getMaxSlippagePct() * 0.35,
        0.00045,
        0.00120
    );

    if (signal.liquidity_score >= 70.0 && signal.volatility <= 3.5) {
        slippage_per_side *= 0.85;
    } else if (signal.liquidity_score > 0.0 && signal.liquidity_score < 50.0) {
        slippage_per_side *= 1.10;
    }

    if (signal.market_regime == analytics::MarketRegime::HIGH_VOLATILITY ||
        signal.market_regime == analytics::MarketRegime::TRENDING_DOWN) {
        slippage_per_side *= 1.08;
    }
    slippage_per_side = std::clamp(slippage_per_side, 0.00035, 0.00140);

    double spread_buffer = 0.00015;
    if (signal.liquidity_score >= 75.0) {
        spread_buffer = 0.00008;
    } else if (signal.liquidity_score > 0.0 && signal.liquidity_score < 50.0) {
        spread_buffer = 0.00022;
    }

    return (fee_rate_per_side * 2.0) + (slippage_per_side * 2.0) + spread_buffer;
}

double computeImpliedWinProbForManager(const Signal& signal) {
    double implied_win = std::clamp(signal.strength, 0.35, 0.75);

    if (signal.strategy_trade_count >= 8) {
        const double sample_weight = std::clamp(
            (static_cast<double>(signal.strategy_trade_count) - 8.0) / 40.0,
            0.0,
            1.0
        );
        const double history_win = std::clamp(signal.strategy_win_rate, 0.20, 0.82);
        implied_win = ((1.0 - sample_weight) * implied_win) + (sample_weight * history_win);

        if (signal.strategy_profit_factor > 0.0) {
            implied_win += std::clamp((signal.strategy_profit_factor - 1.0) * 0.03, -0.04, 0.04);
        }
    }

    return std::clamp(implied_win, 0.18, 0.85);
}

double computeRewardRiskRatioForManager(const Signal& signal) {
    if (!(signal.entry_price > 0.0) || !(signal.stop_loss > 0.0)) {
        return 0.0;
    }
    const double take_profit = signal.getTakeProfitForLegacy();
    if (!(take_profit > 0.0)) {
        return 0.0;
    }
    const double risk = std::abs(signal.entry_price - signal.stop_loss);
    if (risk <= 1e-12) {
        return 0.0;
    }
    const double reward = std::abs(take_profit - signal.entry_price);
    return reward / risk;
}

double computeExpectedValueTighteningForManager(
    const Signal& signal,
    analytics::MarketRegime regime
) {
    double tighten = 0.0;

    if (regime == analytics::MarketRegime::TRENDING_DOWN) {
        tighten += 0.00012;
    } else if (regime == analytics::MarketRegime::HIGH_VOLATILITY) {
        tighten += 0.00010;
    } else if (regime == analytics::MarketRegime::RANGING) {
        tighten += 0.00003;
    }

    if (signal.liquidity_score > 0.0 && signal.liquidity_score < 55.0) {
        tighten += std::clamp((55.0 - signal.liquidity_score) * 0.0000030, 0.0, 0.00018);
    }
    if (signal.volatility > 4.5) {
        tighten += std::clamp((signal.volatility - 4.5) * 0.000025, 0.0, 0.00012);
    }

    const double reward_risk_ratio = computeRewardRiskRatioForManager(signal);
    if (reward_risk_ratio > 0.0) {
        if (reward_risk_ratio < 1.10) {
            tighten += 0.00010;
        } else if (reward_risk_ratio < 1.25) {
            tighten += 0.00006;
        }
    }

    if (signal.strategy_trade_count >= 20 &&
        (signal.strategy_win_rate < 0.45 ||
         (signal.strategy_profit_factor > 0.0 && signal.strategy_profit_factor < 1.00))) {
        tighten += 0.00012;
    }

    if (signal.reason == "alpha_head_fallback_candidate") {
        // Keep fallback path usable, but still quality-bound by the rest of this function.
        tighten = std::max(0.0, tighten - 0.00003);
    }

    return std::clamp(tighten, 0.0, 0.00040);
}

bool alphaHeadModeEnabledForManager() {
    return Config::getInstance().getEngineConfig().use_strategy_alpha_head_mode;
}

bool coreSignalOwnershipEnabledForManager() {
    const auto& cfg = Config::getInstance().getEngineConfig();
    return cfg.enable_core_plane_bridge && cfg.enable_core_risk_plane;
}

double computeCoreRescueStrength(
    const analytics::CoinMetrics& metrics,
    analytics::MarketRegime regime
) {
    double strength = 0.44;
    strength += std::clamp((metrics.liquidity_score - 55.0) / 220.0, -0.06, 0.10);
    strength += std::clamp((metrics.volume_surge_ratio - 1.0) * 0.05, -0.03, 0.08);
    strength += std::clamp(metrics.order_book_imbalance * 0.07, -0.05, 0.06);

    switch (regime) {
        case analytics::MarketRegime::TRENDING_UP:
            strength += 0.05;
            break;
        case analytics::MarketRegime::RANGING:
            strength += 0.02;
            break;
        case analytics::MarketRegime::TRENDING_DOWN:
            strength -= 0.04;
            break;
        case analytics::MarketRegime::HIGH_VOLATILITY:
            strength -= 0.02;
            break;
        default:
            break;
    }

    return std::clamp(strength, 0.36, 0.78);
}

Signal buildCoreRescueCandidate(
    const std::shared_ptr<IStrategy>& strategy,
    const std::string& market,
    const analytics::CoinMetrics& metrics,
    const std::vector<Candle>& candles,
    double current_price,
    double available_capital,
    const analytics::RegimeAnalysis& regime
) {
    Signal out;
    if (!strategy || candles.size() < 20 || current_price <= 0.0 || available_capital <= 0.0) {
        return out;
    }

    if (!strategy->shouldEnter(market, metrics, candles, current_price, regime)) {
        return out;
    }

    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    if (!candles.empty() && candles.back().timestamp > 0) {
        now_ms = candles.back().timestamp;
    }

    const auto info = strategy->getInfo();
    const double entry_price = current_price;
    double stop_loss = strategy->calculateStopLoss(entry_price, candles);
    if (!(stop_loss > 0.0) || stop_loss >= entry_price) {
        stop_loss = entry_price * 0.992;
    }

    double take_profit_2 = strategy->calculateTakeProfit(entry_price, candles);
    if (!(take_profit_2 > entry_price)) {
        take_profit_2 = entry_price * 1.012;
    }
    double take_profit_1 = entry_price + ((take_profit_2 - entry_price) * 0.55);
    if (!(take_profit_1 > entry_price) || take_profit_1 > take_profit_2) {
        take_profit_1 = entry_price + ((take_profit_2 - entry_price) * 0.45);
    }

    double position_size = strategy->calculatePositionSize(
        available_capital,
        entry_price,
        stop_loss,
        metrics
    );
    if (!(position_size > 0.0)) {
        const double min_lot = 5000.0 / std::max(1.0, available_capital);
        position_size = std::clamp(min_lot, 0.01, 0.06);
    }
    position_size = std::clamp(position_size, 0.0, 1.0);

    out.type = SignalType::BUY;
    out.market = market;
    out.strategy_name = info.name;
    out.timestamp = now_ms;
    out.strength = computeCoreRescueStrength(metrics, regime.regime);
    out.signal_filter = out.strength;
    out.entry_price = entry_price;
    out.stop_loss = stop_loss;
    out.take_profit_1 = take_profit_1;
    out.take_profit_2 = take_profit_2;
    out.position_size = position_size;
    out.buy_order_type = OrderTypePolicy::LIMIT_WITH_FALLBACK;
    out.sell_order_type = OrderTypePolicy::LIMIT_WITH_FALLBACK;
    out.max_retries = 2;
    out.retry_wait_ms = 700;
    out.market_regime = regime.regime;
    out.entry_archetype = "CORE_RESCUE_SHOULD_ENTER";
    out.reason = "core_rescue_candidate";
    out.used_preloaded_tf_5m =
        (metrics.candles_by_tf.find("5m") != metrics.candles_by_tf.end() &&
         metrics.candles_by_tf.at("5m").size() >= 20);
    out.used_preloaded_tf_1h =
        (metrics.candles_by_tf.find("1h") != metrics.candles_by_tf.end() &&
         metrics.candles_by_tf.at("1h").size() >= 10);
    out.used_resampled_tf_fallback = !out.used_preloaded_tf_5m;

    return out;
}

void incrementRejectionReason(std::map<std::string, int>* bucket, const char* reason) {
    if (bucket == nullptr || reason == nullptr || reason[0] == '\0') {
        return;
    }
    (*bucket)[reason]++;
}

Signal buildAlphaHeadFallbackSignal(
    const std::string& strategy_name,
    const std::string& market,
    const analytics::CoinMetrics& metrics,
    const std::vector<Candle>& candles,
    double current_price,
    const analytics::RegimeAnalysis& regime
) {
    Signal out;
    if (candles.size() < 30 || current_price <= 0.0) {
        return out;
    }

    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    auto safe_close_from_end = [&](size_t idx_from_end) -> double {
        if (candles.size() <= idx_from_end) {
            return 0.0;
        }
        return candles[candles.size() - 1 - idx_from_end].close;
    };

    const double c0 = safe_close_from_end(0);
    const double c5 = safe_close_from_end(5);
    const double c20 = safe_close_from_end(20);
    if (c0 <= 0.0 || c5 <= 0.0 || c20 <= 0.0) {
        return out;
    }

    const double ret5 = (c0 - c5) / c5;
    const double ret20 = (c0 - c20) / c20;
    const double pressure_total = std::max(1e-6, std::abs(metrics.buy_pressure) + std::abs(metrics.sell_pressure));
    const double buy_bias = std::clamp((metrics.buy_pressure - metrics.sell_pressure) / pressure_total, -1.0, 1.0);
    const bool liquid_ok = metrics.liquidity_score >= 45.0;
    double hostility = 0.0;
    switch (regime.regime) {
        case analytics::MarketRegime::HIGH_VOLATILITY:
            hostility += 0.72;
            break;
        case analytics::MarketRegime::TRENDING_DOWN:
            hostility += 0.62;
            break;
        case analytics::MarketRegime::RANGING:
            hostility += 0.34;
            break;
        case analytics::MarketRegime::TRENDING_UP:
            hostility += 0.12;
            break;
        default:
            hostility += 0.28;
            break;
    }
    if (metrics.volatility > 0.0) {
        hostility += std::clamp((metrics.volatility - 1.8) / 6.0, 0.0, 0.28);
    }
    if (metrics.liquidity_score > 0.0) {
        hostility += std::clamp((55.0 - metrics.liquidity_score) / 90.0, 0.0, 0.20);
    }
    if (metrics.orderbook_snapshot.valid) {
        const double spread_pct = metrics.orderbook_snapshot.spread_pct * 100.0;
        hostility += std::clamp((spread_pct - 0.18) / 0.40, 0.0, 0.18);
    }
    hostility = std::clamp(hostility, 0.0, 1.0);
    const bool hostile_band = hostility >= 0.62;
    const bool severe_hostile_band = hostility >= 0.78;
    if (severe_hostile_band) {
        return out;
    }

    const std::string lower = toLowerCopy(strategy_name);
    const bool is_scalping = (lower.find("scalping") != std::string::npos);
    const bool is_momentum = (lower.find("momentum") != std::string::npos);
    const bool is_breakout = (lower.find("breakout") != std::string::npos);
    const bool is_mean_reversion =
        (lower.find("mean reversion") != std::string::npos) ||
        (lower.find("mean_reversion") != std::string::npos);

    bool candidate_ok = false;
    double base_strength = 0.0;
    std::string archetype = "FALLBACK_GENERIC";

    if (is_scalping || is_momentum || is_breakout) {
        const bool regime_ok =
            (regime.regime == analytics::MarketRegime::TRENDING_UP ||
             regime.regime == analytics::MarketRegime::RANGING ||
             regime.regime == analytics::MarketRegime::UNKNOWN);
        candidate_ok = regime_ok && liquid_ok && ret5 >= -0.0015 && ret20 >= -0.01 && buy_bias >= -0.06;
        base_strength = 0.45
            + std::clamp(ret5 * 40.0, -0.08, 0.14)
            + std::clamp(ret20 * 18.0, -0.06, 0.12)
            + std::clamp((metrics.liquidity_score - 55.0) / 240.0, -0.05, 0.08)
            + std::clamp(buy_bias * 0.12, -0.05, 0.08);
        if (regime.regime == analytics::MarketRegime::TRENDING_UP) {
            base_strength += 0.05;
        } else if (regime.regime == analytics::MarketRegime::RANGING) {
            base_strength += 0.02;
        }
        if (metrics.volume_surge_ratio >= 1.35) {
            base_strength += 0.03;
        }
        if (hostile_band) {
            candidate_ok = candidate_ok &&
                metrics.liquidity_score >= 55.0 &&
                ret5 >= 0.0 &&
                buy_bias >= 0.02;
            base_strength -= 0.03;
        } else if (hostility <= 0.35) {
            base_strength += 0.02;
        }
        if (is_scalping) {
            archetype = "FALLBACK_SCALP_FLOW";
        } else if (is_momentum) {
            archetype = "FALLBACK_MOMENTUM_CONT";
        } else {
            archetype = "FALLBACK_BREAKOUT_CONT";
        }
    } else if (is_mean_reversion) {
        const bool regime_ok =
            (regime.regime == analytics::MarketRegime::RANGING ||
             regime.regime == analytics::MarketRegime::HIGH_VOLATILITY ||
             regime.regime == analytics::MarketRegime::UNKNOWN);
        candidate_ok = regime_ok && liquid_ok && ret5 <= 0.002 && ret20 <= 0.01 && buy_bias >= -0.12;
        base_strength = 0.44
            + std::clamp((-ret5) * 28.0, -0.04, 0.12)
            + std::clamp((-ret20) * 10.0, -0.04, 0.08)
            + std::clamp((metrics.liquidity_score - 55.0) / 260.0, -0.05, 0.08)
            + std::clamp(buy_bias * 0.10, -0.05, 0.06);
        if (regime.regime == analytics::MarketRegime::RANGING) {
            base_strength += 0.04;
        } else if (regime.regime == analytics::MarketRegime::HIGH_VOLATILITY) {
            base_strength += 0.01;
        }
        if (hostile_band) {
            candidate_ok = candidate_ok &&
                metrics.liquidity_score >= 55.0 &&
                ret5 <= -0.0005;
            base_strength -= 0.02;
        } else if (hostility <= 0.35) {
            base_strength += 0.02;
        }
        archetype = "FALLBACK_MEAN_REV_PULLBACK";
    } else {
        return out;
    }

    if (!candidate_ok) {
        return out;
    }
    if (regime.regime == analytics::MarketRegime::TRENDING_DOWN && buy_bias < 0.08) {
        return out;
    }

    const double strength = std::clamp(base_strength, 0.38, 0.80);
    const double vol_pct = std::max(0.2, metrics.volatility) / 100.0;
    const double risk_pct = std::clamp((vol_pct * 0.85) + 0.0042, 0.0045, 0.0180);
    double rr_target = 1.45;
    if (is_momentum || is_breakout) {
        rr_target = (regime.regime == analytics::MarketRegime::TRENDING_UP) ? 1.85 : 1.70;
    } else if (is_mean_reversion) {
        rr_target = 1.50;
    }
    const double tp2_pct = std::clamp(risk_pct * rr_target, 0.0085, 0.0320);
    const double tp1_pct = std::clamp(tp2_pct * 0.62, 0.0045, 0.0200);

    out.type = (strength >= 0.62) ? SignalType::STRONG_BUY : SignalType::BUY;
    out.market = market;
    out.strategy_name = strategy_name;
    out.strength = strength;
    out.entry_price = current_price;
    out.stop_loss = current_price * (1.0 - risk_pct);
    out.take_profit_1 = current_price * (1.0 + tp1_pct);
    out.take_profit_2 = current_price * (1.0 + tp2_pct);
    out.position_size = std::clamp(0.022 + (strength - 0.40) * 0.060, 0.010, 0.045);
    out.buy_order_type = OrderTypePolicy::LIMIT_WITH_FALLBACK;
    out.sell_order_type = OrderTypePolicy::LIMIT_WITH_FALLBACK;
    out.max_retries = 2;
    out.retry_wait_ms = 700;
    out.signal_filter = strength;
    out.market_regime = regime.regime;
    out.entry_archetype = archetype;
    out.used_preloaded_tf_5m =
        (metrics.candles_by_tf.find("5m") != metrics.candles_by_tf.end() &&
         metrics.candles_by_tf.at("5m").size() >= 20);
    out.used_preloaded_tf_1h =
        (metrics.candles_by_tf.find("1h") != metrics.candles_by_tf.end() &&
         metrics.candles_by_tf.at("1h").size() >= 10);
    out.used_resampled_tf_fallback = !out.used_preloaded_tf_5m;
    out.expected_return_pct = tp2_pct;
    out.expected_risk_pct = risk_pct;
    out.expected_value = (tp2_pct * std::clamp(0.42 + (strength * 0.28), 0.38, 0.68)) - (risk_pct * 0.58);
    out.reason = "alpha_head_fallback_candidate";
    out.timestamp = now_ms;
    return out;
}
}

StrategyManager::StrategyManager(std::shared_ptr<network::UpbitHttpClient> client)
    : client_(client)
{
    LOG_INFO("StrategyManager initialized");
}

void StrategyManager::setLiveSignalBottleneckHint(const LiveSignalBottleneckHint& hint) {
    std::lock_guard<std::mutex> lock(mutex_);
    live_bottleneck_hint_ = hint;
}

StrategyManager::LiveSignalBottleneckHint StrategyManager::getLiveSignalBottleneckHint() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return live_bottleneck_hint_;
}

void StrategyManager::registerStrategy(std::shared_ptr<IStrategy> strategy) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto info = strategy->getInfo();
    strategies_.push_back(strategy);

    LOG_INFO("Strategy registered: {} (expected win rate: {:.1f}%, risk: {}/10)",
             info.name, info.expected_winrate * 100, info.risk_level);
}

std::shared_ptr<IStrategy> StrategyManager::getStrategy(const std::string& name) {
    for (const auto& strategy : strategies_) {
        if (strategy->getInfo().name == name) {
            return strategy;
        }
    }
    return nullptr;
}

Signal StrategyManager::processStrategySignal(
    std::shared_ptr<IStrategy> strategy,
    const std::string& market,
    const analytics::CoinMetrics& metrics,
    const std::vector<Candle>& candles,
    double current_price,
    double available_capital,
    const analytics::RegimeAnalysis& regime
) {
    try {
        if (!strategy->isEnabled()) {
            return Signal();
        }

        const bool core_signal_ownership = coreSignalOwnershipEnabledForManager();
        auto signal = strategy->generateSignal(market, metrics, candles, current_price, available_capital, regime);
        if (signal.type == SignalType::NONE && core_signal_ownership) {
            signal = buildCoreRescueCandidate(
                strategy,
                market,
                metrics,
                candles,
                current_price,
                available_capital,
                regime
            );
        }
        if (signal.type == SignalType::NONE && alphaHeadModeEnabledForManager()) {
            signal = buildAlphaHeadFallbackSignal(
                strategy->getInfo().name,
                market,
                metrics,
                candles,
                current_price,
                regime
            );
        }

        if (signal.type != SignalType::NONE) {
            auto stats = strategy->getStatistics();

            signal.strategy_win_rate = stats.win_rate;
            signal.strategy_profit_factor = stats.profit_factor;
            signal.strategy_trade_count = stats.total_signals;
            signal.liquidity_score = metrics.liquidity_score;
            signal.volatility = metrics.volatility;

            if (signal.entry_price > 0 && signal.take_profit_2 > 0) {
                signal.expected_return_pct = (signal.take_profit_2 - signal.entry_price) / signal.entry_price;
            }

            if (signal.entry_price > 0 && signal.stop_loss > 0) {
                signal.expected_risk_pct = (signal.entry_price - signal.stop_loss) / signal.entry_price;
            }

            const double implied_win = computeImpliedWinProbForManager(signal);
            if (signal.expected_return_pct > 0.0 && signal.expected_risk_pct > 0.0) {
                const double expected_gross_pct =
                    (implied_win * signal.expected_return_pct) -
                    ((1.0 - implied_win) * signal.expected_risk_pct);
                // Pre-filter stage stays slightly permissive; final gate uses full cost in engine.
                const double prefilter_cost_pct = estimateSignalRoundTripCostPctForManager(signal) * 0.55;
                signal.expected_value = expected_gross_pct - prefilter_cost_pct;
            } else {
                signal.expected_value = 0.0;
            }

            signal.score = calculateSignalScore(signal);
            if (signal.reason == "alpha_head_fallback_candidate") {
                LOG_INFO("{} - {} fallback candidate generated (strength {:.2f}, archetype {})",
                         market, signal.strategy_name, signal.strength, signal.entry_archetype);
            } else if (signal.reason == "core_rescue_candidate") {
                LOG_INFO("{} - {} core rescue candidate generated (strength {:.2f}, archetype {})",
                         market, signal.strategy_name, signal.strength, signal.entry_archetype);
            }

            return signal;
        }
    } catch (const std::exception& e) {
        LOG_ERROR("{} - {} analysis failed: {}", market, strategy->getInfo().name, e.what());
    }

    return Signal();
}

std::vector<Signal> StrategyManager::collectSignals(
    const std::string& market,
    const analytics::CoinMetrics& metrics,
    const std::vector<Candle>& candles,
    double current_price,
    double available_capital,
    const analytics::RegimeAnalysis& regime
) {
    std::vector<Signal> signals;

    // Execute sequentially to avoid API burst/race in live mode.
    for (auto& strategy : strategies_) {
        try {
            Signal signal = processStrategySignal(
                strategy, market, metrics, candles, current_price, available_capital, regime
            );
            if (signal.type != SignalType::NONE) {
                signals.push_back(signal);
                LOG_INFO(
                    "{} - {} signal generated: strength {:.2f}, score {:.2f}, tf5m_preloaded={}, tf1h_preloaded={}, fallback={}",
                    market,
                    signal.strategy_name,
                    signal.strength,
                    signal.score,
                    signal.used_preloaded_tf_5m ? "Y" : "N",
                    signal.used_preloaded_tf_1h ? "Y" : "N",
                    signal.used_resampled_tf_fallback ? "Y" : "N");
            }
        } catch (const std::exception& e) {
            LOG_ERROR("Strategy execution exception ({}): {}", strategy->getInfo().name, e.what());
        }
    }

    if (!signals.empty()) {
        LOG_INFO("{} - strategy analysis complete ({} signals)", market, signals.size());
    }

    return signals;
}

Signal StrategyManager::selectBestSignal(const std::vector<Signal>& signals) {
    if (signals.empty()) {
        return Signal();
    }

    auto best = std::max_element(signals.begin(), signals.end(),
        [this](const Signal& a, const Signal& b) {
            return calculateSignalScore(a) < calculateSignalScore(b);
        }
    );

    return *best;
}

Signal StrategyManager::selectRobustSignalWithDiagnostics(
    const std::vector<Signal>& signals,
    analytics::MarketRegime regime,
    SelectionDiagnostics* diagnostics
) {
    if (diagnostics != nullptr) {
        diagnostics->input_count = static_cast<int>(signals.size());
        diagnostics->directional_candidate_count = 0;
        diagnostics->scored_candidate_count = 0;
        diagnostics->rejection_reason_counts.clear();
        diagnostics->live_hint_adjusted_candidate_count = 0;
        diagnostics->live_hint_adjustment_counts.clear();
    }

    if (signals.empty()) {
        return Signal();
    }

    const LiveSignalBottleneckHint live_hint = getLiveSignalBottleneckHint();
    const bool core_signal_ownership = coreSignalOwnershipEnabledForManager();

    std::map<int, int> direction_votes;
    std::map<StrategyRole, int> role_votes;
    for (const auto& signal : signals) {
        const int direction = directionFromType(signal.type);
        if (direction == 0) {
            continue;
        }
        direction_votes[direction]++;
        role_votes[detectStrategyRole(signal.strategy_name)]++;
    }

    int preferred_direction = 0;
    int best_vote = 0;
    for (const auto& [direction, votes] : direction_votes) {
        if (votes > best_vote) {
            best_vote = votes;
            preferred_direction = direction;
        }
    }

    std::vector<Signal> directional_candidates;
    for (const auto& signal : signals) {
        const int direction = directionFromType(signal.type);
        if (preferred_direction == 0 || direction == preferred_direction) {
            directional_candidates.push_back(signal);
        }
    }

    if (diagnostics != nullptr) {
        diagnostics->directional_candidate_count = static_cast<int>(directional_candidates.size());
    }

    if (directional_candidates.empty()) {
        incrementRejectionReason(
            diagnostics != nullptr ? &diagnostics->rejection_reason_counts : nullptr,
            "no_best_signal_no_directional_candidates"
        );
        return Signal();
    }

    // High-stress regimes require stronger agreement to avoid fragile one-strategy entries.
    if ((regime == analytics::MarketRegime::TRENDING_DOWN ||
         regime == analytics::MarketRegime::HIGH_VOLATILITY) &&
        static_cast<int>(directional_candidates.size()) == 1) {
        const auto& only = directional_candidates.front();
        if (only.strength < 0.78 || only.expected_value < 0.0008) {
            if (core_signal_ownership) {
                incrementRejectionReason(
                    diagnostics != nullptr ? &diagnostics->rejection_reason_counts : nullptr,
                    "softened_high_stress_single_candidate_core_mode"
                );
            } else {
                incrementRejectionReason(
                    diagnostics != nullptr ? &diagnostics->rejection_reason_counts : nullptr,
                    "no_best_signal_high_stress_single_candidate"
                );
                return Signal();
            }
        }
    }

    if (static_cast<int>(directional_candidates.size()) == 1) {
        const auto& only = directional_candidates.front();
        const double rr_ratio = computeRewardRiskRatioForManager(only);
        const bool weak_quality_single =
            (only.expected_value < 0.00010 || (rr_ratio > 0.0 && rr_ratio < 1.15)) &&
            only.strength < 0.70;
        if (weak_quality_single) {
            if (core_signal_ownership) {
                incrementRejectionReason(
                    diagnostics != nullptr ? &diagnostics->rejection_reason_counts : nullptr,
                    "softened_single_candidate_quality_floor_core_mode"
                );
            } else {
                incrementRejectionReason(
                    diagnostics != nullptr ? &diagnostics->rejection_reason_counts : nullptr,
                    "no_best_signal_single_candidate_quality_floor"
                );
                return Signal();
            }
        }
    }

    double best_score = -1e18;
    Signal best_signal;
    for (const auto& signal : directional_candidates) {
        const StrategyRole role = detectStrategyRole(signal.strategy_name);
        const RegimePolicy policy = getRegimePolicy(role, regime);
        RegimePolicy effective_policy = policy;
        if (policy == RegimePolicy::BLOCK && core_signal_ownership) {
            incrementRejectionReason(
                diagnostics != nullptr ? &diagnostics->rejection_reason_counts : nullptr,
                "softened_policy_block_core_mode"
            );
            effective_policy = RegimePolicy::HOLD;
        }
        const double reward_risk_ratio = computeRewardRiskRatioForManager(signal);
        const bool trend_role = (role == StrategyRole::MOMENTUM || role == StrategyRole::BREAKOUT);
        const bool off_trend_regime =
            trend_role &&
            regime != analytics::MarketRegime::TRENDING_UP;
        if (effective_policy == RegimePolicy::BLOCK) {
            incrementRejectionReason(
                diagnostics != nullptr ? &diagnostics->rejection_reason_counts : nullptr,
                "no_best_signal_policy_block"
            );
            continue;
        }

        double score = calculateSignalScore(signal);
        if (policy == RegimePolicy::BLOCK && core_signal_ownership) {
            score *= 0.78;
        }

        if (effective_policy == RegimePolicy::ALLOW) {
            score *= 1.08;
        } else if (effective_policy == RegimePolicy::HOLD) {
            score *= 0.92;
        }

        const PerformanceGate gate = getPerformanceGate(role, regime);
        const bool has_sample = signal.strategy_trade_count >= gate.min_sample_trades;
        if (has_sample) {
            const bool gate_pass =
                signal.strategy_win_rate >= gate.min_win_rate &&
                signal.strategy_profit_factor >= gate.min_profit_factor;
            if (gate_pass) {
                score *= 1.05;
            } else {
                // Let the engine layer make the final reject decision with full context.
                score *= 0.86;
            }
        }

        const int role_count = role_votes[role];
        if (role_count >= 2) {
            score *= 1.04;
        }

        const int direction = directionFromType(signal.type);
        if (direction != 0 && direction_votes[direction] >= 2) {
            score *= 1.05;
        }

        if (signal.expected_value < 0.0) {
            incrementRejectionReason(
                diagnostics != nullptr ? &diagnostics->rejection_reason_counts : nullptr,
                "no_best_signal_negative_expected_value"
            );
            score *= 0.80;
        }

        if (off_trend_regime) {
            if (signal.expected_value < 0.00020 ||
                (reward_risk_ratio > 0.0 && reward_risk_ratio < 1.28)) {
                incrementRejectionReason(
                    diagnostics != nullptr ? &diagnostics->rejection_reason_counts : nullptr,
                    "no_best_signal_trend_off_regime_low_edge"
                );
                score *= 0.62;
            } else {
                score *= 0.90;
            }
        }

        if (reward_risk_ratio > 0.0 && reward_risk_ratio < 1.25) {
            incrementRejectionReason(
                diagnostics != nullptr ? &diagnostics->rejection_reason_counts : nullptr,
                "no_best_signal_low_reward_risk"
            );
            score *= (reward_risk_ratio < 1.10) ? 0.74 : 0.84;
        }

        // Runtime hint parity with tuning loop: when signal-generation bottleneck
        // dominates, modestly prioritize scalable alpha-head strategies.
        if (live_hint.enabled &&
            live_hint.top_group == "signal_generation" &&
            !live_hint.no_trade_bias_active) {
            if ((role == StrategyRole::SCALPING || role == StrategyRole::MOMENTUM) &&
                signal.strength >= 0.40 &&
                signal.expected_value >= -0.0002) {
                double boost = 1.03;
                if (live_hint.signal_generation_share >= 0.60) {
                    boost += 0.02;
                } else if (live_hint.signal_generation_share >= 0.50) {
                    boost += 0.01;
                }
                if (signal.reason == "alpha_head_fallback_candidate") {
                    boost += 0.01;
                }
                score *= boost;
                if (diagnostics != nullptr) {
                    diagnostics->live_hint_adjusted_candidate_count++;
                    if (role == StrategyRole::SCALPING) {
                        diagnostics->live_hint_adjustment_counts["boost_scalping"]++;
                    } else {
                        diagnostics->live_hint_adjustment_counts["boost_momentum"]++;
                    }
                    if (signal.reason == "alpha_head_fallback_candidate") {
                        diagnostics->live_hint_adjustment_counts["boost_alpha_fallback"]++;
                    }
                }
            } else if (role == StrategyRole::GRID) {
                score *= 0.98;
                if (diagnostics != nullptr) {
                    diagnostics->live_hint_adjusted_candidate_count++;
                    diagnostics->live_hint_adjustment_counts["dampen_grid"]++;
                }
            }
        } else if (live_hint.enabled && live_hint.no_trade_bias_active) {
            if (signal.expected_value < 0.00020) {
                incrementRejectionReason(
                    diagnostics != nullptr ? &diagnostics->rejection_reason_counts : nullptr,
                    "no_best_signal_no_trade_bias_low_edge"
                );
                score *= 0.86;
            }
            if (reward_risk_ratio > 0.0 && reward_risk_ratio < 1.25) {
                incrementRejectionReason(
                    diagnostics != nullptr ? &diagnostics->rejection_reason_counts : nullptr,
                    "no_best_signal_no_trade_bias_low_reward_risk"
                );
                score *= 0.88;
            }
        }

        // Harder reliability down-weight for strategies that have accumulated poor edge.
        if (signal.strategy_trade_count >= 20) {
            if (signal.strategy_win_rate < 0.42) {
                incrementRejectionReason(
                    diagnostics != nullptr ? &diagnostics->rejection_reason_counts : nullptr,
                    "no_best_signal_low_win_rate_history"
                );
                score *= 0.75;
            }
            if (signal.strategy_profit_factor > 0.0 && signal.strategy_profit_factor < 0.95) {
                incrementRejectionReason(
                    diagnostics != nullptr ? &diagnostics->rejection_reason_counts : nullptr,
                    "no_best_signal_low_profit_factor_history"
                );
                score *= 0.78;
            }
            if ((signal.strategy_win_rate < 0.42 || signal.strategy_profit_factor < 0.95) &&
                signal.expected_value < 0.0002) {
                incrementRejectionReason(
                    diagnostics != nullptr ? &diagnostics->rejection_reason_counts : nullptr,
                    "no_best_signal_low_reliability_combo"
                );
                score *= 0.55;
            }
        }

        if (diagnostics != nullptr) {
            diagnostics->scored_candidate_count++;
        }

        if (score > best_score) {
            best_score = score;
            best_signal = signal;
            best_signal.score = score;
        }
    }

    if (best_signal.type == SignalType::NONE) {
        incrementRejectionReason(
            diagnostics != nullptr ? &diagnostics->rejection_reason_counts : nullptr,
            "no_best_signal_no_scored_candidates"
        );
    }

    return best_signal;
}

std::vector<Signal> StrategyManager::filterSignalsWithDiagnostics(
    const std::vector<Signal>& signals,
    double min_strength,
    double min_expected_value,
    analytics::MarketRegime regime,
    FilterDiagnostics* diagnostics
) {
    std::vector<Signal> filtered;
    const bool core_signal_ownership = coreSignalOwnershipEnabledForManager();
    const LiveSignalBottleneckHint live_hint = getLiveSignalBottleneckHint();
    if (diagnostics != nullptr) {
        diagnostics->input_count = static_cast<int>(signals.size());
        diagnostics->output_count = 0;
        diagnostics->rejection_reason_counts.clear();
    }

    for (const auto& signal : signals) {
        const StrategyRole role = detectStrategyRole(signal.strategy_name);
        const RegimePolicy policy = getRegimePolicy(role, regime);
        RegimePolicy effective_policy = policy;
        if (policy == RegimePolicy::BLOCK && core_signal_ownership) {
            incrementRejectionReason(
                diagnostics != nullptr ? &diagnostics->rejection_reason_counts : nullptr,
                "softened_filtered_policy_block_core_mode"
            );
            effective_policy = RegimePolicy::HOLD;
        }
        const double reward_risk_ratio = computeRewardRiskRatioForManager(signal);
        const bool trend_role = (role == StrategyRole::MOMENTUM || role == StrategyRole::BREAKOUT);
        const bool off_trend_regime =
            trend_role &&
            regime != analytics::MarketRegime::TRENDING_UP;
        if (effective_policy == RegimePolicy::BLOCK) {
            incrementRejectionReason(
                diagnostics != nullptr ? &diagnostics->rejection_reason_counts : nullptr,
                "filtered_out_by_manager_policy_block"
            );
            continue;
        }

        double required_strength = min_strength;
        double required_expected_value = min_expected_value;

        // Core ownership mode keeps manager prefilter permissive and delegates
        // final edge/risk rejection to engine/core quality gates.
        if (core_signal_ownership) {
            required_strength = std::max(0.0, required_strength - 0.02);
            required_expected_value = std::min(required_expected_value, -0.00005);
        }

        if (policy == RegimePolicy::BLOCK && core_signal_ownership) {
            required_strength = std::min(0.95, required_strength + 0.07);
            required_expected_value += 0.00012;
        }

        if (effective_policy == RegimePolicy::HOLD) {
            required_strength = std::min(0.95, required_strength + 0.07);
            required_expected_value += core_signal_ownership ? 0.00012 : 0.00020;
            if (signal.strategy_trade_count > 0 && signal.strategy_trade_count < 15) {
                required_strength = std::min(0.95, required_strength + 0.03);
            }
        }

        if (off_trend_regime) {
            required_strength = std::min(0.95, required_strength + 0.06);
            required_expected_value += core_signal_ownership ? 0.00008 : 0.00015;
            if (regime == analytics::MarketRegime::RANGING && signal.liquidity_score < 60.0) {
                required_expected_value += core_signal_ownership ? 0.00004 : 0.00008;
            }
        }

        const PerformanceGate gate = getPerformanceGate(role, regime);
        const bool has_sample = signal.strategy_trade_count >= gate.min_sample_trades;
        if (has_sample) {
            if (signal.strategy_win_rate < gate.min_win_rate ||
                signal.strategy_profit_factor < gate.min_profit_factor) {
                // Soft-pressure only; final risk/quality rejection is done in engine layer.
                required_strength = std::min(0.95, required_strength + 0.04);
                required_expected_value += core_signal_ownership ? 0.00005 : 0.00010;
            }
        }

        if (reward_risk_ratio > 0.0 && reward_risk_ratio < 1.15) {
            required_strength = std::min(0.95, required_strength + 0.05);
            required_expected_value += core_signal_ownership ? 0.00004 : 0.00008;
        }

        // Keep pre-filter permissive; engine layer applies final EV/edge gates.
        const double core_ev_relief = core_signal_ownership ? 0.00018 : 0.0;
        const double permissive_ev_floor =
            std::min(0.0, required_expected_value) - (0.00015 + core_ev_relief);
        double ev_tightening = computeExpectedValueTighteningForManager(signal, regime);
        if (core_signal_ownership) {
            ev_tightening = std::min(ev_tightening, 0.00022);
            if (signal.reason == "core_rescue_candidate") {
                ev_tightening = std::max(0.0, ev_tightening - 0.00005);
            }
        }
        double adaptive_ev_floor = permissive_ev_floor + ev_tightening;
        double live_prefilter_ev_relief = 0.0;
        if (live_hint.enabled && live_hint.top_group == "manager_prefilter") {
            live_prefilter_ev_relief = 0.00003;
            if (live_hint.manager_prefilter_share >= 0.55) {
                live_prefilter_ev_relief += 0.00003;
            } else if (live_hint.manager_prefilter_share >= 0.45) {
                live_prefilter_ev_relief += 0.00002;
            }
            if (live_hint.no_trade_bias_active) {
                live_prefilter_ev_relief += 0.00002;
            }
            if (off_trend_regime ||
                regime == analytics::MarketRegime::HIGH_VOLATILITY ||
                regime == analytics::MarketRegime::TRENDING_DOWN) {
                live_prefilter_ev_relief *= 0.55;
            }
            if (signal.reason == "core_rescue_candidate") {
                live_prefilter_ev_relief += 0.00001;
            }
            live_prefilter_ev_relief = std::clamp(live_prefilter_ev_relief, 0.0, 0.00010);
            adaptive_ev_floor -= live_prefilter_ev_relief;
            adaptive_ev_floor = std::max(adaptive_ev_floor, permissive_ev_floor - 0.00010);
        }
        if (signal.strength >= required_strength && signal.expected_value >= adaptive_ev_floor) {
            filtered.push_back(signal);
        } else if (signal.strength < required_strength) {
            incrementRejectionReason(
                diagnostics != nullptr ? &diagnostics->rejection_reason_counts : nullptr,
                off_trend_regime
                    ? "filtered_out_by_manager_trend_off_regime_strength"
                    : "filtered_out_by_manager_strength"
            );
        } else if (signal.expected_value < adaptive_ev_floor) {
            incrementRejectionReason(
                diagnostics != nullptr ? &diagnostics->rejection_reason_counts : nullptr,
                off_trend_regime
                    ? "filtered_out_by_manager_trend_off_regime_ev"
                    : "filtered_out_by_manager_ev_quality_floor"
            );
        } else if (reward_risk_ratio > 0.0 && reward_risk_ratio < 1.15) {
            incrementRejectionReason(
                diagnostics != nullptr ? &diagnostics->rejection_reason_counts : nullptr,
                "filtered_out_by_manager_low_reward_risk"
            );
        } else {
            incrementRejectionReason(
                diagnostics != nullptr ? &diagnostics->rejection_reason_counts : nullptr,
                "filtered_out_by_manager_expected_value"
            );
        }
    }

    if (diagnostics != nullptr) {
        diagnostics->output_count = static_cast<int>(filtered.size());
    }
    return filtered;
}

std::map<std::string, IStrategy::Statistics> StrategyManager::getAllStatistics() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::map<std::string, IStrategy::Statistics> stats_map;
    for (const auto& strategy : strategies_) {
        auto info = strategy->getInfo();
        stats_map[info.name] = strategy->getStatistics();
    }
    return stats_map;
}

std::vector<std::shared_ptr<IStrategy>> StrategyManager::getStrategies() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return strategies_;
}

void StrategyManager::refreshStrategyStatesFromHistory(
    const std::vector<risk::TradeHistory>& history,
    analytics::MarketRegime dominant_regime,
    bool avoid_high_volatility,
    bool avoid_trending_down,
    int min_trades_for_ev,
    double min_expectancy_krw,
    double min_profit_factor
) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::map<std::string, StrategyEdgeStats> edge;
    for (const auto& trade : history) {
        if (trade.strategy_name.empty()) {
            continue;
        }
        auto& s = edge[trade.strategy_name];
        s.trades++;
        s.net_profit += trade.profit_loss;
        if (trade.profit_loss > 0.0) {
            s.wins++;
            s.gross_profit += trade.profit_loss;
        } else if (trade.profit_loss < 0.0) {
            s.gross_loss_abs += std::abs(trade.profit_loss);
        }
    }

    for (auto& strategy : strategies_) {
        const auto info = strategy->getInfo();
        const std::string strategy_name = info.name;
        const StrategyRole role = detectStrategyRole(strategy_name);
        bool enable = true;

        if (avoid_high_volatility && dominant_regime == analytics::MarketRegime::HIGH_VOLATILITY) {
            enable = false;
        }
        if (avoid_trending_down && dominant_regime == analytics::MarketRegime::TRENDING_DOWN) {
            enable = false;
        }

        const RegimePolicy policy = getRegimePolicy(role, dominant_regime);
        if (policy == RegimePolicy::BLOCK) {
            enable = false;
        }

        const auto stat_it = edge.find(strategy_name);
        if (stat_it != edge.end()) {
            const auto& stat = stat_it->second;
            if (stat.trades >= min_trades_for_ev) {
                const bool ev_ok =
                    (stat.expectancy() >= min_expectancy_krw) &&
                    (stat.profitFactor() >= min_profit_factor);
                if (!ev_ok) {
                    enable = false;
                }
            }

            const PerformanceGate gate = getPerformanceGate(role, dominant_regime);
            if (stat.trades >= gate.min_sample_trades) {
                if (stat.winRate() < gate.min_win_rate ||
                    stat.profitFactor() < gate.min_profit_factor) {
                    enable = false;
                }
            }
        }

        if (strategy->isEnabled() != enable) {
            strategy->setEnabled(enable);
            LOG_INFO("Strategy state changed: {} -> {} (manager policy)",
                     strategy_name, enable ? "ON" : "OFF");
        }
    }
}

double StrategyManager::calculateSignalScore(const Signal& signal) const {
    double score = signal.strength;

    switch (signal.type) {
        case SignalType::STRONG_BUY:
        case SignalType::STRONG_SELL:
            score *= 1.5;
            break;
        case SignalType::BUY:
        case SignalType::SELL:
            score *= 1.0;
            break;
        default:
            score *= 0.5;
            break;
    }

    double tp = signal.getTakeProfitForLegacy();
    if (signal.entry_price > 0 && signal.stop_loss > 0 && tp > 0) {
        double risk = std::abs(signal.entry_price - signal.stop_loss);
        double reward = std::abs(tp - signal.entry_price);

        if (risk > 0) {
            double rr_ratio = reward / risk;
            score *= std::min(2.0, rr_ratio / 2.0);
        }
    }

    if (signal.strategy_trade_count >= 30) {
        if (signal.strategy_profit_factor >= 1.5) {
            score *= 1.15;
        } else if (signal.strategy_profit_factor < 1.1) {
            score *= 0.90;
        }

        if (signal.strategy_win_rate >= 0.60) {
            score *= 1.10;
        } else if (signal.strategy_win_rate < 0.45) {
            score *= 0.90;
        }
    }

    if (signal.liquidity_score > 0.0) {
        if (signal.liquidity_score < 30.0) {
            score *= 0.75;
        } else if (signal.liquidity_score < 50.0) {
            score *= 0.90;
        } else if (signal.liquidity_score >= 80.0) {
            score *= 1.05;
        }
    }

    if (signal.volatility > 0.0) {
        if (signal.volatility < 0.6) {
            score *= 0.85;
        } else if (signal.volatility > 6.0) {
            score *= 0.90;
        }
    }

    if (signal.expected_value != 0.0) {
        double ev_boost = std::clamp(signal.expected_value * 10.0, -0.5, 0.5);
        score *= (1.0 + ev_boost);
    }

    return score;
}

StrategyManager::StrategyRole StrategyManager::detectStrategyRole(const std::string& strategy_name) const {
    const std::string n = toLowerCopy(strategy_name);
    if (n.find("scalping") != std::string::npos) {
        return StrategyRole::SCALPING;
    }
    if (n.find("momentum") != std::string::npos) {
        return StrategyRole::MOMENTUM;
    }
    if (n.find("breakout") != std::string::npos) {
        return StrategyRole::BREAKOUT;
    }
    if (n.find("mean reversion") != std::string::npos || n.find("mean_reversion") != std::string::npos) {
        return StrategyRole::MEAN_REVERSION;
    }
    if (n.find("grid") != std::string::npos) {
        return StrategyRole::GRID;
    }
    return StrategyRole::OTHER;
}

StrategyManager::RegimePolicy StrategyManager::getRegimePolicy(
    StrategyRole role,
    analytics::MarketRegime regime
) const {
    switch (regime) {
        case analytics::MarketRegime::TRENDING_UP:
            switch (role) {
                case StrategyRole::BREAKOUT:
                case StrategyRole::MOMENTUM:
                case StrategyRole::SCALPING:
                    return RegimePolicy::ALLOW;
                case StrategyRole::MEAN_REVERSION:
                case StrategyRole::GRID:
                case StrategyRole::OTHER:
                    return RegimePolicy::HOLD;
            }
            break;

        case analytics::MarketRegime::TRENDING_DOWN:
            switch (role) {
                case StrategyRole::MOMENTUM:
                case StrategyRole::BREAKOUT:
                case StrategyRole::GRID:
                    return RegimePolicy::BLOCK;
                case StrategyRole::SCALPING:
                case StrategyRole::MEAN_REVERSION:
                case StrategyRole::OTHER:
                    return RegimePolicy::HOLD;
            }
            break;

        case analytics::MarketRegime::RANGING:
            switch (role) {
                case StrategyRole::MEAN_REVERSION:
                case StrategyRole::GRID:
                    return RegimePolicy::ALLOW;
                case StrategyRole::SCALPING:
                case StrategyRole::MOMENTUM:
                case StrategyRole::BREAKOUT:
                case StrategyRole::OTHER:
                    return RegimePolicy::HOLD;
            }
            break;

        case analytics::MarketRegime::HIGH_VOLATILITY:
            switch (role) {
                case StrategyRole::MOMENTUM:
                case StrategyRole::BREAKOUT:
                    return RegimePolicy::BLOCK;
                case StrategyRole::SCALPING:
                case StrategyRole::MEAN_REVERSION:
                case StrategyRole::GRID:
                case StrategyRole::OTHER:
                    return RegimePolicy::HOLD;
            }
            break;

        default:
            return RegimePolicy::HOLD;
    }

    return RegimePolicy::HOLD;
}

StrategyManager::PerformanceGate StrategyManager::getPerformanceGate(
    StrategyRole role,
    analytics::MarketRegime regime
) const {
    PerformanceGate gate;
    gate.min_sample_trades = 20;

    // Objective separation by strategy role.
    switch (role) {
        case StrategyRole::SCALPING:
            gate.min_win_rate = 0.60;      // high win-rate, lower R
            gate.min_profit_factor = 1.05;
            break;
        case StrategyRole::BREAKOUT:
            gate.min_win_rate = 0.42;      // low frequency, high R
            gate.min_profit_factor = 1.25;
            gate.min_sample_trades = 12;
            break;
        case StrategyRole::MEAN_REVERSION:
            gate.min_win_rate = 0.57;      // ranging specialist
            gate.min_profit_factor = 1.10;
            break;
        case StrategyRole::MOMENTUM:
            gate.min_win_rate = 0.50;
            gate.min_profit_factor = 1.15;
            break;
        case StrategyRole::GRID:
            gate.min_win_rate = 0.56;
            gate.min_profit_factor = 1.08;
            break;
        case StrategyRole::OTHER:
            gate.min_win_rate = 0.52;
            gate.min_profit_factor = 1.10;
            break;
    }

    // Regime-specific dual-gate tuning.
    if (regime == analytics::MarketRegime::TRENDING_DOWN) {
        gate.min_win_rate += 0.03;
        gate.min_profit_factor += 0.05;
    } else if (regime == analytics::MarketRegime::HIGH_VOLATILITY) {
        gate.min_win_rate += 0.02;
        gate.min_profit_factor += 0.04;
    } else if (regime == analytics::MarketRegime::RANGING) {
        if (role == StrategyRole::MEAN_REVERSION || role == StrategyRole::GRID) {
            gate.min_win_rate = std::max(0.0, gate.min_win_rate - 0.02);
            gate.min_profit_factor = std::max(1.0, gate.min_profit_factor - 0.03);
        } else {
            gate.min_profit_factor += 0.03;
        }
    } else if (regime == analytics::MarketRegime::TRENDING_UP) {
        if (role == StrategyRole::BREAKOUT || role == StrategyRole::MOMENTUM) {
            gate.min_win_rate = std::max(0.0, gate.min_win_rate - 0.02);
            gate.min_profit_factor += 0.02;
        }
    }

    return gate;
}

} // namespace strategy
} // namespace autolife




