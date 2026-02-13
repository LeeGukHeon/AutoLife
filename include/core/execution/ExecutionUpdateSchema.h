#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "core/model/PlaneTypes.h"

namespace autolife {
namespace core {
namespace execution {

inline const char* orderStatusToString(OrderStatus status) {
    switch (status) {
        case OrderStatus::PENDING: return "PENDING";
        case OrderStatus::SUBMITTED: return "SUBMITTED";
        case OrderStatus::FILLED: return "FILLED";
        case OrderStatus::PARTIALLY_FILLED: return "PARTIALLY_FILLED";
        case OrderStatus::CANCELLED: return "CANCELLED";
        case OrderStatus::REJECTED: return "REJECTED";
    }
    return "UNKNOWN";
}

inline const char* orderSideToString(OrderSide side) {
    return (side == OrderSide::BUY) ? "BUY" : "SELL";
}

inline bool isTerminalStatus(OrderStatus status) {
    return status == OrderStatus::FILLED ||
           status == OrderStatus::CANCELLED ||
           status == OrderStatus::REJECTED;
}

inline ExecutionUpdate makeExecutionUpdate(
    const std::string& source,
    const std::string& event,
    const std::string& order_id,
    const std::string& market,
    OrderSide side,
    OrderStatus status,
    double filled_volume,
    double order_volume,
    double avg_price,
    const std::string& strategy_name,
    bool terminal,
    long long ts_ms
) {
    ExecutionUpdate update;
    update.order_id = order_id;
    update.market = market;
    update.side = side;
    update.status = status;
    update.filled_volume = filled_volume;
    update.order_volume = order_volume;
    update.avg_price = avg_price;
    update.strategy_name = strategy_name;
    update.source = source;
    update.event = event;
    update.terminal = terminal;
    update.ts_ms = ts_ms;
    return update;
}

inline nlohmann::json toJson(const ExecutionUpdate& update) {
    nlohmann::json line;
    line["ts_ms"] = update.ts_ms;
    line["source"] = update.source;
    line["event"] = update.event;
    line["order_id"] = update.order_id;
    line["market"] = update.market;
    line["side"] = orderSideToString(update.side);
    line["status"] = orderStatusToString(update.status);
    line["filled_volume"] = update.filled_volume;
    line["order_volume"] = update.order_volume;
    line["avg_price"] = update.avg_price;
    line["strategy_name"] = update.strategy_name;
    line["terminal"] = update.terminal;
    return line;
}

} // namespace execution
} // namespace core
} // namespace autolife
