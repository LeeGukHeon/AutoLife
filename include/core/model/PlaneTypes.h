#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "analytics/RegimeDetector.h"
#include "common/Types.h"
#include "engine/AdaptivePolicyController.h"
#include "strategy/IStrategy.h"

namespace autolife {
namespace core {

enum class JournalEventType {
    ORDER_SUBMITTED,
    ORDER_UPDATED,
    FILL_APPLIED,
    POSITION_OPENED,
    POSITION_REDUCED,
    POSITION_CLOSED,
    POLICY_CHANGED
};

struct JournalEvent {
    std::uint64_t seq = 0;
    long long ts_ms = 0;
    JournalEventType type = JournalEventType::ORDER_UPDATED;
    std::string market;
    std::string entity_id;
    nlohmann::json payload;
};

struct PolicyContext {
    bool small_seed_mode = false;
    int max_new_orders_per_scan = 1;
    analytics::MarketRegime dominant_regime = analytics::MarketRegime::UNKNOWN;
};

struct PolicyDecisionBatch {
    std::vector<strategy::Signal> selected_candidates;
    int dropped_by_policy = 0;
    std::vector<engine::PolicyDecisionRecord> decisions;
};

struct PreTradeCheck {
    bool allowed = false;
    std::string reason;
};

struct ExecutionRequest {
    std::string market;
    OrderSide side = OrderSide::BUY;
    double price = 0.0;
    double volume = 0.0;
    std::string strategy_name;
    double stop_loss = 0.0;
    double take_profit_1 = 0.0;
    double take_profit_2 = 0.0;
    double breakeven_trigger = 0.0;
    double trailing_start = 0.0;
};

struct ExecutionUpdate {
    std::string order_id;
    std::string market;
    OrderSide side = OrderSide::BUY;
    OrderStatus status = OrderStatus::PENDING;
    double filled_volume = 0.0;
    double order_volume = 0.0;
    double avg_price = 0.0;
    std::string strategy_name;
    std::string source;
    std::string event;
    bool terminal = false;
    long long ts_ms = 0;
};

} // namespace core
} // namespace autolife
