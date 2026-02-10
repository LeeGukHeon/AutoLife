#include "analytics/OrderbookAnalyzer.h"
#include <algorithm>
#include <cmath>

namespace autolife {
namespace analytics {

namespace {

bool hasUnitsArray(const nlohmann::json& orderbook_units) {
    return orderbook_units.is_array() && !orderbook_units.empty();
}

}

OrderbookSnapshot OrderbookAnalyzer::analyze(
    const nlohmann::json& orderbook_units,
    double target_notional_krw,
    int depth_limit
) {
    OrderbookSnapshot snapshot;
    snapshot.target_notional_krw = target_notional_krw;

    if (!hasUnitsArray(orderbook_units)) {
        return snapshot;
    }

    int depth = std::min(depth_limit, (int)orderbook_units.size());
    if (depth <= 0) {
        return snapshot;
    }

    snapshot.best_bid = orderbook_units[0].value("bid_price", 0.0);
    snapshot.best_ask = orderbook_units[0].value("ask_price", 0.0);

    if (snapshot.best_bid > 0.0 && snapshot.best_ask > 0.0) {
        snapshot.mid_price = (snapshot.best_bid + snapshot.best_ask) * 0.5;
        if (snapshot.mid_price > 0.0) {
            snapshot.spread_pct = (snapshot.best_ask - snapshot.best_bid) / snapshot.mid_price;
        }
    }

    double bid_notional = 0.0;
    double ask_notional = 0.0;

    for (int i = 0; i < depth; ++i) {
        double bid_price = orderbook_units[i].value("bid_price", 0.0);
        double bid_size = orderbook_units[i].value("bid_size", 0.0);
        double ask_price = orderbook_units[i].value("ask_price", 0.0);
        double ask_size = orderbook_units[i].value("ask_size", 0.0);

        bid_notional += bid_price * bid_size;
        ask_notional += ask_price * ask_size;
    }

    snapshot.bid_notional = bid_notional;
    snapshot.ask_notional = ask_notional;

    double total = bid_notional + ask_notional;
    if (total > 0.0) {
        snapshot.imbalance = (bid_notional - ask_notional) / total;
    }

    snapshot.vwap_buy = estimateVWAPForNotional(orderbook_units, target_notional_krw, true, depth_limit);
    snapshot.vwap_sell = estimateVWAPForNotional(orderbook_units, target_notional_krw, false, depth_limit);
    snapshot.valid = snapshot.mid_price > 0.0;

    return snapshot;
}

double OrderbookAnalyzer::estimateVWAPForNotional(
    const nlohmann::json& orderbook_units,
    double target_notional_krw,
    bool is_buy,
    int depth_limit
) {
    if (!hasUnitsArray(orderbook_units) || target_notional_krw <= 0.0) {
        return 0.0;
    }

    double remaining = target_notional_krw;
    double total_qty = 0.0;
    double total_cost = 0.0;
    int depth = std::min(depth_limit, (int)orderbook_units.size());

    for (int i = 0; i < depth && remaining > 0.0; ++i) {
        double price = orderbook_units[i].value(is_buy ? "ask_price" : "bid_price", 0.0);
        double size = orderbook_units[i].value(is_buy ? "ask_size" : "bid_size", 0.0);

        if (price <= 0.0 || size <= 0.0) {
            continue;
        }

        double level_notional = price * size;
        double take_notional = std::min(remaining, level_notional);
        double take_qty = take_notional / price;

        total_qty += take_qty;
        total_cost += take_qty * price;
        remaining -= take_notional;
    }

    if (total_qty <= 0.0) {
        return 0.0;
    }

    return total_cost / total_qty;
}

double OrderbookAnalyzer::estimateSlippagePctForNotional(
    const nlohmann::json& orderbook_units,
    double target_notional_krw,
    bool is_buy,
    double reference_price,
    int depth_limit
) {
    if (reference_price <= 0.0) {
        return 0.0;
    }

    double vwap = estimateVWAPForNotional(orderbook_units, target_notional_krw, is_buy, depth_limit);
    if (vwap <= 0.0) {
        return 0.0;
    }

    return is_buy ? (vwap - reference_price) / reference_price
                  : (reference_price - vwap) / reference_price;
}

} // namespace analytics
} // namespace autolife
