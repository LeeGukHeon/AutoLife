#include "strategy/StrategyManager.h"
#include "strategy/FoundationRiskPipeline.h"
#include "analytics/TechnicalIndicators.h"
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

bool managerSoftQueueEnabled() {
    const auto& cfg = Config::getInstance().getEngineConfig();
    return (
        cfg.manager_soft_queue_enabled &&
        cfg.enable_probabilistic_runtime_model &&
        cfg.probabilistic_runtime_primary_mode
    );
}

bool allowManagerSoftQueuePromotion(
    const foundation::FilterDecision& decision,
    const Signal& signal,
    analytics::MarketRegime regime
) {
    if (regime != analytics::MarketRegime::RANGING &&
        regime != analytics::MarketRegime::TRENDING_UP) {
        return false;
    }
    const std::string reason = toLowerCopy(decision.reject_reason);
    const bool reason_ok =
        reason == "filtered_out_by_manager_ev_quality_floor" ||
        reason == "filtered_out_by_manager_strength" ||
        reason == "filtered_out_by_manager_rr_guard_ev" ||
        reason == "filtered_out_by_manager_rr_guard_strength" ||
        reason == "filtered_out_by_manager_history_guard_ev" ||
        reason == "filtered_out_by_manager_history_guard_strength" ||
        reason == "filtered_out_by_manager_trend_off_regime_ev" ||
        reason == "filtered_out_by_manager_trend_off_regime_strength";
    if (!reason_ok) {
        return false;
    }

    const auto& cfg = Config::getInstance().getEngineConfig();
    const double strength_gap = std::max(0.0, decision.required_strength - signal.strength);
    const double ev_gap = std::max(0.0, decision.required_expected_value - signal.expected_value);
    double allowed_strength_gap = cfg.manager_soft_queue_max_strength_gap;
    double allowed_ev_gap = cfg.manager_soft_queue_max_ev_gap;
    double min_liquidity = 28.0;
    if (signal.probabilistic_runtime_applied) {
        const double margin = signal.probabilistic_h5_margin;
        const double confidence = std::clamp(
            (std::clamp((signal.probabilistic_h5_calibrated - 0.50) / 0.25, 0.0, 1.0) * 0.65) +
            (std::clamp((margin + 0.02) / 0.12, 0.0, 1.0) * 0.35),
            0.0,
            1.0
        );
        allowed_strength_gap += confidence * 0.06;
        allowed_ev_gap += confidence * 0.00020;
        min_liquidity = std::max(12.0, 26.0 - (confidence * 14.0));
        if (margin < -0.015) {
            min_liquidity += 4.0;
        }
    }
    if (strength_gap > allowed_strength_gap || ev_gap > allowed_ev_gap) {
        return false;
    }
    if (signal.liquidity_score < min_liquidity) {
        return false;
    }
    if (signal.volatility > 0.0 && signal.volatility > 3.8) {
        return false;
    }
    if (signal.expected_value < -0.00035) {
        return false;
    }
    const double rr = foundation::rewardRiskRatio(signal);
    if (rr > 0.0 && rr < 0.92) {
        return false;
    }
    return true;
}

bool probabilisticPrimaryModeEnabledForManager() {
    const auto& cfg = Config::getInstance().getEngineConfig();
    return cfg.enable_probabilistic_runtime_model && cfg.probabilistic_runtime_primary_mode;
}

bool isProbabilisticPromotionEligibleReason(const std::string& reason) {
    return (
        reason == "filtered_out_by_manager_strength" ||
        reason == "filtered_out_by_manager_ev_quality_floor" ||
        reason == "filtered_out_by_manager_rr_guard_strength" ||
        reason == "filtered_out_by_manager_rr_guard_ev" ||
        reason == "filtered_out_by_manager_history_guard_strength" ||
        reason == "filtered_out_by_manager_history_guard_ev" ||
        reason == "filtered_out_by_manager_trend_off_regime_strength" ||
        reason == "filtered_out_by_manager_trend_off_regime_ev"
    );
}

bool allowProbabilisticPrimaryPromotion(
    const foundation::FilterDecision& decision,
    const Signal& signal,
    analytics::MarketRegime regime
) {
    if (!probabilisticPrimaryModeEnabledForManager()) {
        return false;
    }
    if (!signal.probabilistic_runtime_applied) {
        return false;
    }
    if (!isProbabilisticPromotionEligibleReason(decision.reject_reason)) {
        return false;
    }

    const bool hostile_regime =
        regime == analytics::MarketRegime::HIGH_VOLATILITY ||
        regime == analytics::MarketRegime::TRENDING_DOWN;
    const auto& cfg = Config::getInstance().getEngineConfig();
    const double margin = signal.probabilistic_h5_margin;
    double required_margin = cfg.probabilistic_primary_promotion_min_margin;
    if (hostile_regime) {
        required_margin = std::max(0.004, required_margin + 0.012);
    } else {
        required_margin = std::max(-0.028, required_margin - 0.012);
    }
    if (margin < required_margin) {
        return false;
    }
    double min_calibrated = cfg.probabilistic_primary_promotion_min_calibrated;
    if (hostile_regime) {
        min_calibrated = std::max(0.52, min_calibrated + 0.04);
    } else {
        min_calibrated = std::clamp(min_calibrated - 0.03, 0.42, 0.90);
    }
    if (signal.probabilistic_h5_calibrated < min_calibrated) {
        return false;
    }
    if (signal.liquidity_score < (hostile_regime ? 48.0 : 20.0)) {
        return false;
    }
    if (hostile_regime && signal.expected_value < 0.0) {
        return false;
    }
    if (signal.expected_value < -0.00045) {
        return false;
    }

    const double strength_gap = std::max(0.0, decision.required_strength - signal.strength);
    const double ev_gap = std::max(0.0, decision.required_expected_value - signal.expected_value);
    double max_strength_gap = cfg.probabilistic_primary_promotion_max_strength_gap;
    double max_ev_gap = cfg.probabilistic_primary_promotion_max_ev_gap;
    if (signal.probabilistic_h5_margin > 0.02) {
        max_strength_gap += 0.04;
        max_ev_gap += 0.00012;
    }
    if (hostile_regime) {
        max_strength_gap = std::min(max_strength_gap, 0.16);
        max_ev_gap = std::min(max_ev_gap, 0.00060);
    } else if (signal.probabilistic_h5_margin >= -0.010) {
        max_strength_gap = std::max(max_strength_gap + 0.06, 0.20);
        max_ev_gap = std::max(max_ev_gap + 0.00020, 0.00080);
    }
    if (strength_gap > max_strength_gap || ev_gap > max_ev_gap) {
        return false;
    }
    if (signal.volatility > 0.0 && signal.volatility > (hostile_regime ? 5.6 : 7.0)) {
        return false;
    }
    const double rr = foundation::rewardRiskRatio(signal);
    if (rr > 0.0 && rr < (hostile_regime ? 0.98 : 0.88)) {
        return false;
    }
    return true;
}

bool allowProbabilisticPrimaryFastPass(
    const Signal& signal,
    analytics::MarketRegime regime
) {
    if (!probabilisticPrimaryModeEnabledForManager()) {
        return false;
    }
    if (!signal.probabilistic_runtime_applied) {
        return false;
    }

    const bool hostile_regime =
        regime == analytics::MarketRegime::HIGH_VOLATILITY ||
        regime == analytics::MarketRegime::TRENDING_DOWN;
    const auto& cfg = Config::getInstance().getEngineConfig();

    double required_margin = cfg.probabilistic_primary_promotion_min_margin;
    required_margin += hostile_regime ? 0.014 : -0.002;
    required_margin = std::max(required_margin, hostile_regime ? 0.010 : -0.020);
    if (signal.probabilistic_h5_margin < required_margin) {
        return false;
    }

    double required_calibrated = cfg.probabilistic_primary_promotion_min_calibrated;
    required_calibrated += hostile_regime ? 0.05 : -0.03;
    required_calibrated = std::clamp(required_calibrated, hostile_regime ? 0.53 : 0.43, 0.90);
    if (signal.probabilistic_h5_calibrated < required_calibrated) {
        return false;
    }

    if (signal.liquidity_score < (hostile_regime ? 48.0 : 16.0)) {
        return false;
    }
    if (signal.expected_value < (hostile_regime ? 0.0 : -0.00045)) {
        return false;
    }
    if (signal.volatility > 0.0 && signal.volatility > (hostile_regime ? 5.4 : 7.0)) {
        return false;
    }
    if (!hostile_regime &&
        signal.probabilistic_h5_calibrated < 0.40 &&
        signal.probabilistic_h5_margin < -0.030) {
        return false;
    }

    const double rr = foundation::rewardRiskRatio(signal);
    if (rr > 0.0 && rr < (hostile_regime ? 0.95 : 0.88)) {
        return false;
    }
    return true;
}

double probabilisticPromotionPositionScale(
    const Signal& signal,
    analytics::MarketRegime regime
) {
    const double margin = signal.probabilistic_h5_margin;
    double scale = 0.55 + (std::clamp((margin + 0.02) / 0.14, 0.0, 1.0) * 0.25);
    if (regime == analytics::MarketRegime::HIGH_VOLATILITY ||
        regime == analytics::MarketRegime::TRENDING_DOWN) {
        scale *= 0.85;
    }
    return std::clamp(scale, 0.45, 0.85);
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
    const double take_profit = signal.getPrimaryTakeProfit();
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

    return std::clamp(tighten, 0.0, 0.00040);
}

double estimateLossToWinRatioFromWinRateAndPf(double win_rate, double profit_factor) {
    if (profit_factor <= 1e-9 || win_rate <= 1e-6 || win_rate >= (1.0 - 1e-6)) {
        return 0.0;
    }
    const double loss_rate = 1.0 - win_rate;
    if (loss_rate <= 1e-9) {
        return 0.0;
    }
    return win_rate / (loss_rate * profit_factor);
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

bool isCoreRescueReasonEligible(const std::string& reason) {
    return (
        reason == "foundation_no_signal_liquidity_volume_gate" ||
        reason == "foundation_no_signal_ranging_structure" ||
        reason == "foundation_no_signal_bear_rebound_guard" ||
        reason == "foundation_no_signal_uptrend_thin_context" ||
        reason == "foundation_no_signal_uptrend_exhaustion_guard"
    );
}

bool passesCoreRescueSafetyFloor(
    const analytics::CoinMetrics& metrics,
    const std::vector<Candle>& candles,
    double current_price,
    const analytics::RegimeAnalysis& regime
) {
    if (candles.size() < 80 || current_price <= 0.0) {
        return false;
    }
    if (metrics.liquidity_score < 32.0 || metrics.volume_surge_ratio < 0.40) {
        return false;
    }
    if (metrics.volatility > 0.0 && metrics.volatility > 7.8) {
        return false;
    }
    if (metrics.orderbook_snapshot.valid && metrics.orderbook_snapshot.spread_pct > 0.0035) {
        return false;
    }

    if (metrics.buy_pressure > 0.0 || metrics.sell_pressure > 0.0) {
        const double sell_guard = std::max(1e-9, metrics.sell_pressure);
        const double buy_sell_ratio = metrics.buy_pressure / sell_guard;
        if (buy_sell_ratio < 0.80) {
            return false;
        }
    }

    if (regime.regime == analytics::MarketRegime::TRENDING_DOWN ||
        regime.regime == analytics::MarketRegime::HIGH_VOLATILITY) {
        if (metrics.order_book_imbalance < -0.16) {
            return false;
        }
        if (metrics.buy_pressure < metrics.sell_pressure * 0.92) {
            return false;
        }
    }

    const auto closes = analytics::TechnicalIndicators::extractClosePrices(candles);
    if (closes.size() < 24) {
        return false;
    }
    const double close_now = closes.back();
    const double close_8 = closes[closes.size() - 9];
    const double close_20 = closes[closes.size() - 21];
    if (close_now <= 0.0 || close_8 <= 0.0 || close_20 <= 0.0) {
        return false;
    }
    const double ret8 = (close_now - close_8) / close_8;
    const double ret20 = (close_now - close_20) / close_20;
    if (ret20 < -0.028 || ret8 < -0.015) {
        return false;
    }

    return true;
}

Signal buildCoreRescueCandidate(
    const std::shared_ptr<IStrategy>& strategy,
    const std::string& market,
    const analytics::CoinMetrics& metrics,
    const std::vector<Candle>& candles,
    double current_price,
    double available_capital,
    const analytics::RegimeAnalysis& regime,
    const std::string& upstream_no_signal_reason
) {
    Signal out;
    if (!strategy || candles.size() < 20 || current_price <= 0.0 || available_capital <= 0.0) {
        return out;
    }

    const auto info = strategy->getInfo();
    const bool is_foundation_strategy = (toLowerCopy(info.name).find("foundation") != std::string::npos);

    if (is_foundation_strategy) {
        // Rescue path should only activate for known bottleneck rejections and still pass a strict safety floor.
        if (!isCoreRescueReasonEligible(upstream_no_signal_reason)) {
            return out;
        }
        if (!passesCoreRescueSafetyFloor(metrics, candles, current_price, regime)) {
            return out;
        }
    } else if (!strategy->shouldEnter(market, metrics, candles, current_price, regime)) {
        return out;
    }

    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    if (!candles.empty() && candles.back().timestamp > 0) {
        now_ms = candles.back().timestamp;
    }

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
    // Rescue path is uncertainty-heavy by design; keep size conservative
    // and let probabilistic confidence promote higher sizing later.
    position_size *= 0.65;
    position_size = std::min(position_size, 0.04);
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
    Signal signal;
    try {
        if (!strategy->isEnabled()) {
            return signal;
        }

        const bool core_signal_ownership = coreSignalOwnershipEnabledForManager();
        signal = strategy->generateSignal(market, metrics, candles, current_price, available_capital, regime);
        std::string no_signal_reason = signal.reason;
        if (signal.type == SignalType::NONE && core_signal_ownership) {
            Signal rescue_signal = buildCoreRescueCandidate(
                strategy,
                market,
                metrics,
                candles,
                current_price,
                available_capital,
                regime,
                no_signal_reason
            );
            if (rescue_signal.type != SignalType::NONE) {
                signal = std::move(rescue_signal);
            } else if (no_signal_reason.empty() && !rescue_signal.reason.empty()) {
                no_signal_reason = rescue_signal.reason;
            }
        }
        if (signal.type == SignalType::NONE && !no_signal_reason.empty()) {
            signal.reason = no_signal_reason;
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
            if (signal.reason == "core_rescue_candidate") {
                LOG_INFO("{} - {} core rescue candidate generated (strength {:.2f}, archetype {})",
                         market, signal.strategy_name, signal.strength, signal.entry_archetype);
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR("{} - {} analysis failed: {}", market, strategy->getInfo().name, e.what());
        signal.reason = "strategy_execution_exception";
    }

    return signal;
}

std::vector<Signal> StrategyManager::collectSignals(
    const std::string& market,
    const analytics::CoinMetrics& metrics,
    const std::vector<Candle>& candles,
    double current_price,
    double available_capital,
    const analytics::RegimeAnalysis& regime,
    CollectDiagnostics* diagnostics
) {
    std::vector<Signal> signals;
    if (diagnostics != nullptr) {
        diagnostics->strategy_total = 0;
        diagnostics->strategy_enabled = 0;
        diagnostics->generated_signal_count = 0;
        diagnostics->skipped_disabled_count = 0;
        diagnostics->no_signal_count = 0;
        diagnostics->exception_count = 0;
        diagnostics->generated_by_strategy.clear();
        diagnostics->skipped_disabled_by_strategy.clear();
        diagnostics->no_signal_by_strategy.clear();
        diagnostics->no_signal_reason_counts.clear();
        diagnostics->exception_by_strategy.clear();
    }

    // Execute sequentially to avoid API burst/race in live mode.
    for (auto& strategy : strategies_) {
        const std::string strategy_name = strategy ? strategy->getInfo().name : std::string("unknown");
        if (diagnostics != nullptr) {
            diagnostics->strategy_total++;
        }
        if (!strategy || !strategy->isEnabled()) {
            if (diagnostics != nullptr) {
                diagnostics->skipped_disabled_count++;
                diagnostics->skipped_disabled_by_strategy[strategy_name]++;
            }
            continue;
        }
        if (diagnostics != nullptr) {
            diagnostics->strategy_enabled++;
        }
        try {
            Signal signal = processStrategySignal(
                strategy, market, metrics, candles, current_price, available_capital, regime
            );
            if (signal.type != SignalType::NONE) {
                signals.push_back(signal);
                if (diagnostics != nullptr) {
                    diagnostics->generated_signal_count++;
                    diagnostics->generated_by_strategy[strategy_name]++;
                }
                LOG_INFO(
                    "{} - {} signal generated: strength {:.2f}, score {:.2f}, tf5m_preloaded={}, tf1h_preloaded={}, fallback={}",
                    market,
                    signal.strategy_name,
                    signal.strength,
                    signal.score,
                    signal.used_preloaded_tf_5m ? "Y" : "N",
                    signal.used_preloaded_tf_1h ? "Y" : "N",
                    signal.used_resampled_tf_fallback ? "Y" : "N");
            } else {
                if (diagnostics != nullptr) {
                    diagnostics->no_signal_count++;
                    diagnostics->no_signal_by_strategy[strategy_name]++;
                    const std::string no_signal_reason = signal.reason.empty()
                        ? "no_signal_reason_unspecified"
                        : signal.reason;
                    diagnostics->no_signal_reason_counts[no_signal_reason]++;
                }
            }
        } catch (const std::exception& e) {
            if (diagnostics != nullptr) {
                diagnostics->exception_count++;
                diagnostics->exception_by_strategy[strategy_name]++;
            }
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

    const LiveSignalBottleneckHint foundation_live_hint = getLiveSignalBottleneckHint();
    const bool foundation_core_signal_ownership = coreSignalOwnershipEnabledForManager();

    if (signals.empty()) {
        incrementRejectionReason(
            diagnostics != nullptr ? &diagnostics->rejection_reason_counts : nullptr,
            "no_best_signal_no_candidates"
        );
        return Signal();
    }

    std::map<int, int> direction_votes;
    std::map<StrategyRole, int> role_votes;
    for (const auto& signal : signals) {
        const int direction = directionFromType(signal.type);
        if (direction != 0) {
            direction_votes[direction]++;
        }
        role_votes[detectStrategyRole(signal.strategy_name)]++;
    }

    Signal foundation_best_signal;
    double foundation_best_score = -std::numeric_limits<double>::infinity();
    for (const auto& signal : signals) {
        const int direction = directionFromType(signal.type);
        if (direction <= 0) {
            incrementRejectionReason(
                diagnostics != nullptr ? &diagnostics->rejection_reason_counts : nullptr,
                "no_best_signal_non_long_direction"
            );
            continue;
        }
        if (diagnostics != nullptr) {
            diagnostics->directional_candidate_count++;
        }

        const StrategyRole role = detectStrategyRole(signal.strategy_name);
        const RegimePolicy policy = getRegimePolicy(role, regime);
        const bool trend_role = (role == StrategyRole::MOMENTUM || role == StrategyRole::BREAKOUT);
        const bool off_trend_regime = trend_role && regime != analytics::MarketRegime::TRENDING_UP;
        const bool hostile_regime =
            regime == analytics::MarketRegime::HIGH_VOLATILITY ||
            regime == analytics::MarketRegime::TRENDING_DOWN;

        if (policy == RegimePolicy::BLOCK && !foundation_core_signal_ownership) {
            incrementRejectionReason(
                diagnostics != nullptr ? &diagnostics->rejection_reason_counts : nullptr,
                "no_best_signal_policy_block"
            );
            continue;
        }

        double score = calculateSignalScore(signal);
        if (policy == RegimePolicy::ALLOW) {
            score *= 1.05;
        } else if (policy == RegimePolicy::HOLD) {
            score *= 0.94;
        } else {
            score *= 0.80;
            incrementRejectionReason(
                diagnostics != nullptr ? &diagnostics->rejection_reason_counts : nullptr,
                "softened_policy_block_core_mode"
            );
        }

        const PerformanceGate gate = getPerformanceGate(role, regime);
        if (signal.strategy_trade_count >= gate.min_sample_trades) {
            if (signal.strategy_win_rate < gate.min_win_rate ||
                signal.strategy_profit_factor < gate.min_profit_factor) {
                score *= 0.82;
                incrementRejectionReason(
                    diagnostics != nullptr ? &diagnostics->rejection_reason_counts : nullptr,
                    "no_best_signal_history_guard"
                );
            } else {
                score *= 1.04;
            }
        }

        const double rr = foundation::rewardRiskRatio(signal);
        if (rr > 0.0 && rr < 1.10) {
            score *= 0.82;
            incrementRejectionReason(
                diagnostics != nullptr ? &diagnostics->rejection_reason_counts : nullptr,
                "no_best_signal_low_reward_risk"
            );
        } else if (rr >= 1.30) {
            score *= 1.04;
        }

        if (off_trend_regime) {
            score *= 0.88;
            if (signal.expected_value < 0.0002) {
                incrementRejectionReason(
                    diagnostics != nullptr ? &diagnostics->rejection_reason_counts : nullptr,
                    "no_best_signal_trend_off_regime_low_edge"
                );
            }
        }
        if (hostile_regime) {
            score *= 0.93;
        }
        if (signal.expected_value < 0.0) {
            score *= 0.78;
            incrementRejectionReason(
                diagnostics != nullptr ? &diagnostics->rejection_reason_counts : nullptr,
                "no_best_signal_negative_expected_value"
            );
        }
        if (foundation_live_hint.enabled &&
            foundation_live_hint.no_trade_bias_active &&
            signal.expected_value < 0.0002) {
            score *= 0.85;
            incrementRejectionReason(
                diagnostics != nullptr ? &diagnostics->rejection_reason_counts : nullptr,
                "no_best_signal_no_trade_bias_low_edge"
            );
        }

        if (direction_votes[direction] >= 2) {
            score *= 1.03;
        }
        if (role_votes[role] >= 2) {
            score *= 1.02;
        }

        if (diagnostics != nullptr) {
            diagnostics->scored_candidate_count++;
        }

        if (score > foundation_best_score) {
            foundation_best_score = score;
            foundation_best_signal = signal;
            foundation_best_signal.score = score;
        }
    }

    if (foundation_best_signal.type == SignalType::NONE) {
        incrementRejectionReason(
            diagnostics != nullptr ? &diagnostics->rejection_reason_counts : nullptr,
            "no_best_signal_no_scored_candidates"
        );
    }
    return foundation_best_signal;
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
    const bool manager_soft_queue_enabled = managerSoftQueueEnabled();
    const auto& cfg = Config::getInstance().getEngineConfig();
    const LiveSignalBottleneckHint live_hint = getLiveSignalBottleneckHint();
    if (diagnostics != nullptr) {
        diagnostics->input_count = static_cast<int>(signals.size());
        diagnostics->output_count = 0;
        diagnostics->rejection_reason_counts.clear();
    }

    // Rebuilt from baseline: deterministic, decomposed filter pipeline.
    for (const auto& signal : signals) {
        const StrategyRole role = detectStrategyRole(signal.strategy_name);
        const RegimePolicy policy = getRegimePolicy(role, regime);
        const bool trend_role = (role == StrategyRole::MOMENTUM || role == StrategyRole::BREAKOUT);
        const bool off_trend_regime = trend_role && regime != analytics::MarketRegime::TRENDING_UP;
        const bool hostile_regime =
            regime == analytics::MarketRegime::HIGH_VOLATILITY ||
            regime == analytics::MarketRegime::TRENDING_DOWN;

        const PerformanceGate gate = getPerformanceGate(role, regime);
        const bool no_trade_bias_active = live_hint.enabled && live_hint.no_trade_bias_active;

        foundation::FilterInput input;
        input.signal = &signal;
        input.regime = regime;
        input.min_strength = min_strength;
        input.min_expected_value = min_expected_value;
        input.core_signal_ownership = core_signal_ownership;
        input.policy_block = (policy == RegimePolicy::BLOCK) && !core_signal_ownership;
        input.policy_hold = (policy == RegimePolicy::HOLD) ||
            ((policy == RegimePolicy::BLOCK) && core_signal_ownership);
        input.off_trend_regime = off_trend_regime;
        input.hostile_regime = hostile_regime;
        input.no_trade_bias_active = no_trade_bias_active;
        input.min_history_sample = gate.min_sample_trades;
        input.min_history_win_rate = gate.min_win_rate;
        input.min_history_profit_factor = gate.min_profit_factor;

        const foundation::FilterDecision decision = foundation::evaluateFilter(input);
        if (decision.pass) {
            filtered.push_back(signal);
            continue;
        }

        if (allowProbabilisticPrimaryFastPass(signal, regime)) {
            Signal promoted = signal;
            if (promoted.position_size > 0.0) {
                promoted.position_size *= std::clamp(
                    probabilisticPromotionPositionScale(promoted, regime) * 1.10,
                    0.45,
                    1.00
                );
            }
            promoted.reason = "manager_probabilistic_primary_fastpass";
            filtered.push_back(promoted);
            incrementRejectionReason(
                diagnostics != nullptr ? &diagnostics->rejection_reason_counts : nullptr,
                "manager_probabilistic_primary_fastpass"
            );
            continue;
        }

        if (manager_soft_queue_enabled &&
            allowManagerSoftQueuePromotion(decision, signal, regime)) {
            Signal promoted = signal;
            if (promoted.position_size > 0.0) {
                promoted.position_size *= cfg.manager_soft_queue_position_scale;
            }
            promoted.reason = "manager_soft_queue_promoted";
            filtered.push_back(promoted);
            incrementRejectionReason(
                diagnostics != nullptr ? &diagnostics->rejection_reason_counts : nullptr,
                "manager_soft_queue_promoted"
            );
            continue;
        }

        if (allowProbabilisticPrimaryPromotion(decision, signal, regime)) {
            Signal promoted = signal;
            if (promoted.position_size > 0.0) {
                promoted.position_size *= probabilisticPromotionPositionScale(promoted, regime);
            }
            promoted.reason = "manager_probabilistic_primary_promoted";
            filtered.push_back(promoted);
            incrementRejectionReason(
                diagnostics != nullptr ? &diagnostics->rejection_reason_counts : nullptr,
                "manager_probabilistic_primary_promoted"
            );
            continue;
        }

        incrementRejectionReason(
            diagnostics != nullptr ? &diagnostics->rejection_reason_counts : nullptr,
            decision.reject_reason.empty()
                ? "filtered_out_by_manager_unknown"
                : decision.reject_reason.c_str()
        );
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

    double tp = signal.getPrimaryTakeProfit();
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

    if (signal.probabilistic_runtime_applied) {
        const bool hostile_regime =
            signal.market_regime == analytics::MarketRegime::HIGH_VOLATILITY ||
            signal.market_regime == analytics::MarketRegime::TRENDING_DOWN;
        const double margin_conf =
            std::clamp((signal.probabilistic_h5_margin + 0.02) / 0.16, 0.0, 1.0);
        const double calibrated_conf =
            std::clamp((signal.probabilistic_h5_calibrated - 0.50) / 0.25, 0.0, 1.0);
        double boost = 1.0 + (margin_conf * 0.30) + (calibrated_conf * 0.10);
        if (hostile_regime) {
            boost = 1.0 + ((boost - 1.0) * 0.70);
        }
        score *= std::clamp(boost, 0.90, 1.35);
    }

    return score;
}

StrategyManager::StrategyRole StrategyManager::detectStrategyRole(const std::string& strategy_name) const {
    const std::string n = toLowerCopy(strategy_name);
    if (n.find("probabilistic primary") != std::string::npos) {
        return StrategyRole::FOUNDATION;
    }
    if (n.find("foundation") != std::string::npos) {
        return StrategyRole::FOUNDATION;
    }
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
                case StrategyRole::FOUNDATION:
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
                case StrategyRole::FOUNDATION:
                    return RegimePolicy::HOLD;
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
                case StrategyRole::FOUNDATION:
                    return RegimePolicy::ALLOW;
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
                case StrategyRole::FOUNDATION:
                    return RegimePolicy::HOLD;
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
        case StrategyRole::FOUNDATION:
            gate.min_win_rate = 0.50;
            gate.min_profit_factor = 1.10;
            gate.min_sample_trades = 16;
            break;
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






