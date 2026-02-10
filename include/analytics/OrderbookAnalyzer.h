#pragma once

#include <nlohmann/json.hpp>

namespace autolife {
namespace analytics {

struct OrderbookSnapshot {
    double best_bid;
    double best_ask;
    double mid_price;
    double spread_pct;
    double bid_notional;
    double ask_notional;
    double imbalance;
    double vwap_buy;
    double vwap_sell;
    double target_notional_krw;
    bool valid;

    OrderbookSnapshot()
        : best_bid(0.0)
        , best_ask(0.0)
        , mid_price(0.0)
        , spread_pct(0.0)
        , bid_notional(0.0)
        , ask_notional(0.0)
        , imbalance(0.0)
        , vwap_buy(0.0)
        , vwap_sell(0.0)
        , target_notional_krw(0.0)
        , valid(false)
    {}
};

class OrderbookAnalyzer {
public:
    static OrderbookSnapshot analyze(
        const nlohmann::json& orderbook_units,
        double target_notional_krw,
        int depth_limit = 10
    );

    static double estimateVWAPForNotional(
        const nlohmann::json& orderbook_units,
        double target_notional_krw,
        bool is_buy,
        int depth_limit = 20
    );

    static double estimateSlippagePctForNotional(
        const nlohmann::json& orderbook_units,
        double target_notional_krw,
        bool is_buy,
        double reference_price,
        int depth_limit = 20
    );
};

} // namespace analytics
} // namespace autolife
