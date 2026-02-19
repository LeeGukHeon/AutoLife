#include "common/StrategyEdgeStatsShared.h"

namespace autolife::common::strategy_edge {

namespace {

void accumulateTrade(StrategyEdgeStats& s, const risk::TradeHistory& trade) {
    s.trades++;
    s.net_profit += trade.profit_loss;
    if (trade.profit_loss > 0.0) {
        s.wins++;
        s.gross_profit += trade.profit_loss;
    } else if (trade.profit_loss < 0.0) {
        s.gross_loss_abs += std::abs(trade.profit_loss);
    }
}

} // namespace

std::string makeStrategyRegimeKey(const std::string& strategy_name, analytics::MarketRegime regime) {
    return strategy_name + "|" + std::to_string(static_cast<int>(regime));
}

std::string makeMarketStrategyRegimeKey(
    const std::string& market,
    const std::string& strategy_name,
    analytics::MarketRegime regime
) {
    return market + "|" + strategy_name + "|" + std::to_string(static_cast<int>(regime));
}

std::map<std::string, StrategyEdgeStats> buildStrategyEdgeStats(
    const std::vector<risk::TradeHistory>& history,
    int max_recent_trades_per_strategy
) {
    std::map<std::string, StrategyEdgeStats> out;
    if (max_recent_trades_per_strategy > 0) {
        std::map<std::string, int> strategy_counts;
        for (auto it = history.rbegin(); it != history.rend(); ++it) {
            const auto& trade = *it;
            if (trade.strategy_name.empty()) {
                continue;
            }
            auto& used = strategy_counts[trade.strategy_name];
            if (used >= max_recent_trades_per_strategy) {
                continue;
            }
            used++;
            accumulateTrade(out[trade.strategy_name], trade);
        }
        return out;
    }

    for (const auto& trade : history) {
        if (trade.strategy_name.empty()) {
            continue;
        }
        accumulateTrade(out[trade.strategy_name], trade);
    }
    return out;
}

std::map<std::string, StrategyEdgeStats> buildStrategyRegimeEdgeStats(
    const std::vector<risk::TradeHistory>& history,
    int max_recent_trades_per_key
) {
    std::map<std::string, StrategyEdgeStats> out;
    if (max_recent_trades_per_key > 0) {
        std::map<std::string, int> key_counts;
        for (auto it = history.rbegin(); it != history.rend(); ++it) {
            const auto& trade = *it;
            if (trade.strategy_name.empty()) {
                continue;
            }
            const std::string key = makeStrategyRegimeKey(trade.strategy_name, trade.market_regime);
            auto& used = key_counts[key];
            if (used >= max_recent_trades_per_key) {
                continue;
            }
            used++;
            accumulateTrade(out[key], trade);
        }
        return out;
    }

    for (const auto& trade : history) {
        if (trade.strategy_name.empty()) {
            continue;
        }
        const std::string key = makeStrategyRegimeKey(trade.strategy_name, trade.market_regime);
        accumulateTrade(out[key], trade);
    }
    return out;
}

std::map<std::string, StrategyEdgeStats> buildMarketStrategyRegimeEdgeStats(
    const std::vector<risk::TradeHistory>& history,
    int max_recent_trades_per_key
) {
    std::map<std::string, StrategyEdgeStats> out;
    if (max_recent_trades_per_key > 0) {
        std::map<std::string, int> key_counts;
        for (auto it = history.rbegin(); it != history.rend(); ++it) {
            const auto& trade = *it;
            if (trade.strategy_name.empty() || trade.market.empty()) {
                continue;
            }
            const std::string key =
                makeMarketStrategyRegimeKey(trade.market, trade.strategy_name, trade.market_regime);
            auto& used = key_counts[key];
            if (used >= max_recent_trades_per_key) {
                continue;
            }
            used++;
            accumulateTrade(out[key], trade);
        }
        return out;
    }

    for (const auto& trade : history) {
        if (trade.strategy_name.empty() || trade.market.empty()) {
            continue;
        }
        const std::string key =
            makeMarketStrategyRegimeKey(trade.market, trade.strategy_name, trade.market_regime);
        accumulateTrade(out[key], trade);
    }
    return out;
}

} // namespace autolife::common::strategy_edge
