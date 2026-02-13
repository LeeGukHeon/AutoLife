#include "strategy/StrategyManager.h"
#include "common/Logger.h"
#include "risk/RiskManager.h"
#include <algorithm>
#include <cctype>
#include <map>

#undef max
#undef min
#include <numeric>

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
}

StrategyManager::StrategyManager(std::shared_ptr<network::UpbitHttpClient> client)
    : client_(client)
{
    LOG_INFO("StrategyManager initialized");
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

        auto signal = strategy->generateSignal(market, metrics, candles, current_price, available_capital, regime);

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

            double implied_win = signal.strategy_trade_count >= 30
                ? signal.strategy_win_rate
                : std::clamp(signal.strength, 0.35, 0.75);

            signal.expected_value = (signal.expected_return_pct > 0.0 && signal.expected_risk_pct > 0.0)
                ? (implied_win * signal.expected_return_pct - (1.0 - implied_win) * signal.expected_risk_pct)
                : 0.0;

            signal.score = calculateSignalScore(signal);

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
                LOG_INFO("{} - {} signal generated: strength {:.2f}, score {:.2f}",
                         market, signal.strategy_name, signal.strength, signal.score);
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

Signal StrategyManager::selectRobustSignal(
    const std::vector<Signal>& signals,
    analytics::MarketRegime regime
) {
    if (signals.empty()) {
        return Signal();
    }

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

    if (directional_candidates.empty()) {
        return Signal();
    }

    // High-stress regimes require stronger agreement to avoid fragile one-strategy entries.
    if ((regime == analytics::MarketRegime::TRENDING_DOWN ||
         regime == analytics::MarketRegime::HIGH_VOLATILITY) &&
        static_cast<int>(directional_candidates.size()) == 1) {
        const auto& only = directional_candidates.front();
        if (only.strength < 0.78 || only.expected_value < 0.0008) {
            return Signal();
        }
    }

    double best_score = -1e18;
    Signal best_signal;
    for (const auto& signal : directional_candidates) {
        const StrategyRole role = detectStrategyRole(signal.strategy_name);
        const RegimePolicy policy = getRegimePolicy(role, regime);
        if (policy == RegimePolicy::BLOCK) {
            continue;
        }

        double score = calculateSignalScore(signal);

        if (policy == RegimePolicy::ALLOW) {
            score *= 1.08;
        } else if (policy == RegimePolicy::HOLD) {
            score *= 0.92;
        }

        const PerformanceGate gate = getPerformanceGate(role, regime);
        const bool has_sample = signal.strategy_trade_count >= gate.min_sample_trades;
        if (has_sample) {
            const bool gate_pass =
                signal.strategy_win_rate >= gate.min_win_rate &&
                signal.strategy_profit_factor >= gate.min_profit_factor;
            if (!gate_pass) {
                continue;
            }
            score *= 1.05;
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
            score *= 0.80;
        }

        // Harder reliability down-weight for strategies that have accumulated poor edge.
        if (signal.strategy_trade_count >= 20) {
            if (signal.strategy_win_rate < 0.42) {
                score *= 0.75;
            }
            if (signal.strategy_profit_factor > 0.0 && signal.strategy_profit_factor < 0.95) {
                score *= 0.78;
            }
            if ((signal.strategy_win_rate < 0.42 || signal.strategy_profit_factor < 0.95) &&
                signal.expected_value < 0.0002) {
                continue;
            }
        }

        if (score > best_score) {
            best_score = score;
            best_signal = signal;
            best_signal.score = score;
        }
    }

    return best_signal;
}

std::vector<Signal> StrategyManager::filterSignals(
    const std::vector<Signal>& signals,
    double min_strength,
    double min_expected_value,
    analytics::MarketRegime regime
) {
    std::vector<Signal> filtered;

    for (const auto& signal : signals) {
        const StrategyRole role = detectStrategyRole(signal.strategy_name);
        const RegimePolicy policy = getRegimePolicy(role, regime);
        if (policy == RegimePolicy::BLOCK) {
            continue;
        }

        double required_strength = min_strength;
        double required_expected_value = min_expected_value;

        if (policy == RegimePolicy::HOLD) {
            required_strength = std::min(0.95, required_strength + 0.07);
            required_expected_value += 0.0002;
            if (signal.strategy_trade_count > 0 && signal.strategy_trade_count < 15) {
                required_strength = std::min(0.95, required_strength + 0.03);
            }
        }

        const PerformanceGate gate = getPerformanceGate(role, regime);
        const bool has_sample = signal.strategy_trade_count >= gate.min_sample_trades;
        if (has_sample) {
            if (signal.strategy_win_rate < gate.min_win_rate ||
                signal.strategy_profit_factor < gate.min_profit_factor) {
                continue;
            }
        }

        if (signal.strength >= required_strength && signal.expected_value >= required_expected_value) {
            filtered.push_back(signal);
        }
    }

    return filtered;
}

Signal StrategyManager::synthesizeSignals(const std::vector<Signal>& signals) {
    if (signals.empty()) {
        return Signal();
    }

    Signal synthesized;

    int buy_count = 0;
    int sell_count = 0;
    double total_strength = 0.0;

    for (const auto& signal : signals) {
        if (signal.type == SignalType::BUY || signal.type == SignalType::STRONG_BUY) {
            buy_count++;
        } else if (signal.type == SignalType::SELL || signal.type == SignalType::STRONG_SELL) {
            sell_count++;
        }
        total_strength += signal.strength;
    }

    if (buy_count > sell_count) {
        synthesized.type = buy_count > sell_count * 2 ? SignalType::STRONG_BUY : SignalType::BUY;
    } else if (sell_count > buy_count) {
        synthesized.type = sell_count > buy_count * 2 ? SignalType::STRONG_SELL : SignalType::SELL;
    } else {
        synthesized.type = SignalType::HOLD;
    }

    synthesized.strength = total_strength / signals.size();

    std::vector<double> entry_prices;
    std::vector<double> stop_losses;
    std::vector<double> take_profit1s;
    std::vector<double> take_profit2s;
    for (const auto& signal : signals) {
        if (signal.entry_price > 0) entry_prices.push_back(signal.entry_price);
        if (signal.stop_loss > 0) stop_losses.push_back(signal.stop_loss);
        if (signal.take_profit_1 > 0) take_profit1s.push_back(signal.take_profit_1);
        if (signal.take_profit_2 > 0) take_profit2s.push_back(signal.take_profit_2);
    }

    if (!entry_prices.empty()) {
        std::sort(entry_prices.begin(), entry_prices.end());
        synthesized.entry_price = entry_prices[entry_prices.size() / 2];
    }

    if (!stop_losses.empty()) {
        std::sort(stop_losses.begin(), stop_losses.end());
        synthesized.stop_loss = stop_losses[stop_losses.size() / 2];
    }

    if (!take_profit2s.empty()) {
        std::sort(take_profit2s.begin(), take_profit2s.end());
        synthesized.take_profit_2 = take_profit2s[take_profit2s.size() / 2];
    }
    if (!take_profit1s.empty()) {
        std::sort(take_profit1s.begin(), take_profit1s.end());
        synthesized.take_profit_1 = take_profit1s[take_profit1s.size() / 2];
    }

    synthesized.reason = "Synthesized from " + std::to_string(signals.size()) + " strategies";

    return synthesized;
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

void StrategyManager::enableStrategy(const std::string& name, bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& strategy : strategies_) {
        if (strategy->getInfo().name == name) {
            strategy->setEnabled(enabled);
            LOG_INFO("Strategy {}: {}", name, enabled ? "enabled" : "disabled");
            return;
        }
    }
}

std::vector<std::string> StrategyManager::getActiveStrategies() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::string> active;
    for (const auto& strategy : strategies_) {
        if (strategy->isEnabled()) {
            active.push_back(strategy->getInfo().name);
        }
    }
    return active;
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

double StrategyManager::getOverallWinRate() const {
    std::lock_guard<std::mutex> lock(mutex_);

    int total_trades = 0;
    int total_wins = 0;

    for (const auto& strategy : strategies_) {
        auto stats = strategy->getStatistics();
        total_trades += stats.winning_trades + stats.losing_trades;
        total_wins += stats.winning_trades;
    }

    if (total_trades == 0) return 0.0;
    return static_cast<double>(total_wins) / total_trades;
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



