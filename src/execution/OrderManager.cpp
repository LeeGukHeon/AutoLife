#include "execution/OrderManager.h"
#include "execution/OrderStateMapper.h"

#include "common/Logger.h"
#include "common/PathUtils.h"
#include "common/TickSizeHelper.h"
#include "core/execution/ExecutionUpdateSchema.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <set>

namespace {
long long getCurrentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

bool isSensitiveKey(const std::string& key) {
    static const std::set<std::string> kKeys = {
        "access_key", "secret_key", "authorization", "bearer",
        "jwt", "token", "api_key", "signature", "query_hash"
    };
    std::string lower = key;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return kKeys.find(lower) != kKeys.end();
}

void maskSensitiveJson(nlohmann::json& node) {
    if (node.is_object()) {
        for (auto it = node.begin(); it != node.end(); ++it) {
            if (isSensitiveKey(it.key())) {
                it.value() = "***";
            } else {
                maskSensitiveJson(it.value());
            }
        }
        return;
    }
    if (node.is_array()) {
        for (auto& item : node) {
            maskSensitiveJson(item);
        }
    }
}

std::string safeDumpJson(const nlohmann::json& j) {
    try {
        nlohmann::json copy = j;
        maskSensitiveJson(copy);
        return copy.dump();
    } catch (...) {
        return "<json_dump_failed>";
    }
}

void appendExecutionUpdateArtifact(const autolife::core::ExecutionUpdate& update) {
    static const auto kArtifactPath = autolife::utils::PathUtils::resolveRelativePath("logs/execution_updates_live.jsonl");
    static std::mutex file_mutex;

    std::lock_guard<std::mutex> lock(file_mutex);
    std::filesystem::create_directories(kArtifactPath.parent_path());
    std::ofstream out(kArtifactPath, std::ios::app | std::ios::binary);
    if (!out.is_open()) {
        LOG_WARN("Execution update artifact open failed: {}", kArtifactPath.string());
        return;
    }
    out << autolife::core::execution::toJson(update).dump() << "\n";
}

void logExecutionLifecycle(
    const char* source,
    const std::string& event,
    const autolife::execution::ActiveOrder& order
) {
    const bool terminal = autolife::core::execution::isTerminalStatus(order.status);

    LOG_INFO(
        "Execution lifecycle: source={}, event={}, order_id={}, market={}, side={}, status={}, filled={:.8f}, volume={:.8f}, terminal={}",
        source,
        event,
        order.order_id,
        order.market,
        autolife::core::execution::orderSideToString(order.side),
        autolife::core::execution::orderStatusToString(order.status),
        order.filled_volume,
        order.volume,
        terminal ? "true" : "false"
    );

    appendExecutionUpdateArtifact(
        autolife::core::execution::makeExecutionUpdate(
            source,
            event,
            order.order_id,
            order.market,
            order.side,
            order.status,
            order.filled_volume,
            order.volume,
            order.price,
            order.strategy_name,
            terminal,
            getCurrentTimeMs()
        )
    );
}
}

namespace autolife {
namespace execution {

OrderManager::OrderManager(std::shared_ptr<network::UpbitHttpClient> http_client, bool enable_my_order_ws)
    : http_client_(std::move(http_client))
    , my_order_ws_enabled_(enable_my_order_ws) {
    if (my_order_ws_enabled_) {
        my_order_ws_client_ = std::make_unique<network::UpbitMyOrderWebSocketClient>(
            http_client_->getAccessKey(),
            http_client_->getSecretKey()
        );
        const bool started = my_order_ws_client_->start([this](const nlohmann::json& message) {
            onMyOrderEvent(message);
        });
        if (started) {
            LOG_INFO("myOrder WS listener started");
        } else {
            LOG_WARN("myOrder WS listener did not start");
            my_order_ws_enabled_ = false;
        }
    }
}

OrderManager::~OrderManager() {
    if (my_order_ws_client_) {
        my_order_ws_client_->stop();
    }
}

bool OrderManager::submitOrder(
    const std::string& market,
    OrderSide side,
    double price,
    double volume,
    const std::string& strategy_name,
    double sl,
    double tp1,
    double tp2,
    double be,
    double ts,
    std::string* submitted_order_id
) {
    if (volume <= 0.0 || price <= 0.0) {
        return false;
    }

    try {
        const std::string side_str = (side == OrderSide::BUY) ? "bid" : "ask";
        const std::string vol_str = common::priceToString(volume);
        const std::string price_str = common::priceToString(price);

        LOG_INFO("Submitting Order: {} {} @ {} (Vol: {})", market, side_str, price, vol_str);

        auto response = http_client_->placeOrder(market, side_str, vol_str, price_str, "limit");
        if (response.is_null() || !response.contains("uuid")) {
            LOG_ERROR("Order placement failed: {}", safeDumpJson(response));
            return false;
        }

        ActiveOrder order;
        order.order_id = response["uuid"].get<std::string>();
        order.market = market;
        order.side = side;
        order.price = price;
        order.volume = volume;
        order.filled_volume = 0.0;
        order.created_at_ms = getCurrentTimeMs();
        order.retry_count = 0;

        order.strategy_name = strategy_name;
        order.stop_loss = sl;
        order.take_profit_1 = tp1;
        order.take_profit_2 = tp2;
        order.breakeven_trigger = be;
        order.trailing_start = ts;

        order.is_chasing = true;
        order.last_chase_price = price;
        order.last_update_ms = getCurrentTimeMs();
        order.status = OrderStatus::SUBMITTED;
        order.last_state_sync_ms = order.last_update_ms;
        logExecutionLifecycle("live_submit", "submitted", order);

        std::lock_guard<std::recursive_mutex> lock(orders_mutex_);
        active_orders_[order.order_id] = order;
        if (submitted_order_id != nullptr) {
            *submitted_order_id = order.order_id;
        }
        LOG_INFO("Order placed successfully. ID: {}", order.order_id);
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("submitOrder exception: {}", e.what());
        return false;
    } catch (...) {
        LOG_ERROR("submitOrder unknown fatal error");
        return false;
    }
}

bool OrderManager::hasActiveOrder(const std::string& market) const {
    std::lock_guard<std::recursive_mutex> lock(orders_mutex_);
    for (const auto& pair : active_orders_) {
        if (pair.second.market == market) {
            return true;
        }
    }
    return false;
}

double OrderManager::parseJsonNumber(const nlohmann::json& json, const char* key) {
    if (!json.contains(key) || json[key].is_null()) {
        return 0.0;
    }
    if (json[key].is_string()) {
        const auto value = json[key].get<std::string>();
        if (value.empty()) {
            return 0.0;
        }
        return std::stod(value);
    }
    return json[key].get<double>();
}

bool OrderManager::isTerminalState(OrderStatus status) {
    return status == OrderStatus::FILLED || status == OrderStatus::CANCELLED || status == OrderStatus::REJECTED;
}

void OrderManager::applyExchangeOrderState(const nlohmann::json& status, bool from_ws) {
    if (!status.contains("uuid")) {
        return;
    }

    const std::string order_id = status["uuid"].get<std::string>();

    std::lock_guard<std::recursive_mutex> lock(orders_mutex_);
    auto it = active_orders_.find(order_id);
    if (it == active_orders_.end()) {
        return;
    }

    auto& order = it->second;
    const std::string state = status.value("state", "");
    const double executed = parseJsonNumber(status, "executed_volume");
    const double remaining = parseJsonNumber(status, "remaining_volume");

    const auto mapped = OrderStateMapper::map(
        state,
        order.filled_volume,
        order.volume,
        executed,
        remaining
    );
    order.filled_volume = mapped.filled_volume;
    order.status = mapped.status;
    if (mapped.terminal) {
        order.is_chasing = false;
    }

    const double avg_price = parseJsonNumber(status, "avg_price");
    if (avg_price > 0.0) {
        order.price = avg_price;
    }

    order.last_state_sync_ms = getCurrentTimeMs();
    logExecutionLifecycle(from_ws ? "live_ws" : "live_rest", state, order);

    if (from_ws && mapped.terminal) {
        LOG_INFO("Order {} reached terminal state from myOrder WS: {}", order.order_id, state);
    }
}

void OrderManager::onMyOrderEvent(const nlohmann::json& message) {
    if (!message.is_object()) {
        return;
    }

    if (message.contains("type")) {
        const std::string type = message.value("type", "");
        if (type != "myOrder") {
            return;
        }
    }

    applyExchangeOrderState(message, true);
}

void OrderManager::syncOrderFillFromExchange(const std::string& order_id) {
    try {
        auto status = http_client_->getOrder(order_id);
        applyExchangeOrderState(status, false);
    } catch (const std::exception& e) {
        LOG_WARN("Failed to sync order fill state for {}: {}", order_id, e.what());
    }
}

bool OrderManager::cancelOrder(const std::string& order_id) {
    try {
        LOG_INFO("Cancelling Order: {}", order_id);
        auto response = http_client_->cancelOrder(order_id);

        if (!response.is_null() && response.contains("uuid")) {
            std::lock_guard<std::recursive_mutex> lock(orders_mutex_);
            auto it = active_orders_.find(order_id);
            if (it != active_orders_.end()) {
                it->second.status = OrderStatus::CANCELLED;
                logExecutionLifecycle("live_cancel", "cancelled", it->second);
            }
            active_orders_.erase(order_id);
            return true;
        }

        if (!response.is_null() && response.contains("error")) {
            const auto err = response["error"];
            const std::string name = err.value("name", "");
            if (name == "done_order" || name == "invalid_ord_uuid") {
                LOG_WARN("Cancel skipped (already done/missing): {}", name);
                syncOrderFillFromExchange(order_id);
                return false;
            }
            LOG_ERROR("Cancel error response: {}", safeDumpJson(response));
            return false;
        }

        LOG_WARN("Unexpected cancel response (order kept active): {}", safeDumpJson(response));
        return false;
    } catch (const std::exception& e) {
        const std::string what = e.what();
        if (what.find("done_order") != std::string::npos || what.find("invalid_ord_uuid") != std::string::npos) {
            LOG_WARN("Cancel skipped (already done/missing): {}", what);
            syncOrderFillFromExchange(order_id);
            return false;
        }
        LOG_ERROR("Exception in cancelOrder: {} - OrderID: {}", what, order_id);
        return false;
    } catch (...) {
        LOG_ERROR("Unknown fatal error in cancelOrder - OrderID: {}", order_id);
        return false;
    }
}

std::vector<ActiveOrder> OrderManager::getFilledOrders() {
    std::lock_guard<std::recursive_mutex> lock(orders_mutex_);
    std::vector<ActiveOrder> filled;

    auto it = active_orders_.begin();
    while (it != active_orders_.end()) {
        const bool has_fill = it->second.filled_volume > 1e-8;
        const bool fully_filled = (it->second.filled_volume >= it->second.volume - 1e-8) && it->second.volume > 0.0;
        const bool terminal_cancel_or_reject =
            (it->second.status == OrderStatus::CANCELLED || it->second.status == OrderStatus::REJECTED);

        if (fully_filled) {
            filled.push_back(it->second);
            it = active_orders_.erase(it);
        } else if (terminal_cancel_or_reject) {
            // Partial fill can exist when an order is canceled/rejected after some execution.
            // Emit that fill once before dropping terminal order from active set.
            if (has_fill) {
                filled.push_back(it->second);
            }
            it = active_orders_.erase(it);
        } else {
            ++it;
        }
    }
    return filled;
}

bool OrderManager::submitMarketFallback(const ActiveOrder& order) {
    try {
        const std::string side_str = (order.side == OrderSide::BUY) ? "bid" : "ask";

        nlohmann::json response;
        if (order.side == OrderSide::BUY) {
            const double spend_krw = order.volume * order.price;
            if (spend_krw <= 0.0) {
                LOG_ERROR("Market fallback BUY aborted: invalid spend amount ({})", spend_krw);
                return false;
            }
            const std::string spend_str = common::priceToString(spend_krw);
            response = http_client_->placeOrder(order.market, side_str, "", spend_str, "price");
        } else {
            const std::string vol_str = common::priceToString(order.volume);
            response = http_client_->placeOrder(order.market, side_str, vol_str, "", "market");
        }

        if (response.is_null() || !response.contains("uuid")) {
            LOG_ERROR("Market fallback response missing uuid: {}", safeDumpJson(response));
            return false;
        }

        ActiveOrder fallback = order;
        fallback.order_id = response["uuid"].get<std::string>();
        fallback.retry_count = 0;
        fallback.price = 0.0;
        fallback.is_chasing = false;
        fallback.last_update_ms = getCurrentTimeMs();
        fallback.status = OrderStatus::SUBMITTED;
        fallback.last_state_sync_ms = fallback.last_update_ms;
        logExecutionLifecycle("live_fallback", "submitted", fallback);

        std::lock_guard<std::recursive_mutex> lock(orders_mutex_);
        active_orders_[fallback.order_id] = fallback;
        LOG_INFO("Market fallback submitted: {} -> {}", order.market, fallback.order_id);
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Market fallback failed: {}", e.what());
        return false;
    }
}

bool OrderManager::shouldUseRestSync(const ActiveOrder& order, long long now_ms) const {
    if (!my_order_ws_client_ || !my_order_ws_client_->isConnected()) {
        return true;
    }

    const long long ws_lag_ms = now_ms - my_order_ws_client_->getLastMessageTimeMs();
    if (ws_lag_ms >= WS_STALE_THRESHOLD_MS) {
        return true;
    }

    if (isTerminalState(order.status)) {
        return false;
    }

    return (now_ms - order.last_state_sync_ms) >= REST_SYNC_INTERVAL_MS;
}

void OrderManager::monitorOrders() {
    std::lock_guard<std::recursive_mutex> lock(orders_mutex_);
    const long long now = getCurrentTimeMs();

    auto it = active_orders_.begin();
    while (it != active_orders_.end()) {
        auto current_it = it++;
        auto& order = current_it->second;

        try {
            if (shouldUseRestSync(order, now)) {
                auto status = http_client_->getOrder(order.order_id);
                applyExchangeOrderState(status, false);
            }

            if (order.status == OrderStatus::FILLED) {
                continue;
            }

            if (order.status == OrderStatus::CANCELLED || order.status == OrderStatus::REJECTED) {
                continue;
            }

            if (order.filled_volume >= order.volume - 1e-8) {
                order.status = OrderStatus::FILLED;
                continue;
            }

            checkLimitChase(order);

            if (order.retry_count >= MAX_CHASE_ATTEMPTS && order.is_chasing) {
                order.is_chasing = false;
                LOG_WARN("Limit chase exhausted ({} attempts), switching to market fallback: {} ({})",
                         MAX_CHASE_ATTEMPTS, order.market, order.order_id);

                ActiveOrder fallback_order = order;
                const bool cancelled = cancelOrder(fallback_order.order_id);
                if (cancelled) {
                    submitMarketFallback(fallback_order);
                }
            }
        } catch (const std::exception& e) {
            LOG_ERROR("Error monitoring order {}: {}", order.order_id, e.what());
        }
    }
}

void OrderManager::checkLimitChase(ActiveOrder& order) {
    const long long now = getCurrentTimeMs();
    if (now - order.last_update_ms < CHASE_INTERVAL_MS) {
        return;
    }
    order.last_update_ms = now;

    if (!order.is_chasing || order.retry_count >= MAX_CHASE_ATTEMPTS) {
        return;
    }

    auto orderbook = http_client_->getOrderBook(order.market);
    if (orderbook.empty()) {
        return;
    }

    try {
        nlohmann::json units;
        if (orderbook.is_array() && !orderbook.empty()) {
            units = orderbook[0]["orderbook_units"];
        } else if (orderbook.contains("orderbook_units")) {
            units = orderbook["orderbook_units"];
        } else {
            return;
        }

        if (units.empty()) {
            return;
        }

        const double best_price = (order.side == OrderSide::BUY)
            ? units[0]["bid_price"].get<double>()
            : units[0]["ask_price"].get<double>();

        if (std::abs(order.price - best_price) <= 1e-6) {
            return;
        }

        LOG_INFO("Limit Chase: Order {} Price {:.0f} != Best {:.0f}. Replacing...",
                 order.order_id, order.price, best_price);

        if (!replaceOrder(order, best_price)) {
            LOG_WARN("Limit chase replace failed: {}", order.order_id);
        }
    } catch (const std::exception& e) {
        LOG_WARN("Limit Chase error: {}", e.what());
    }
}

bool OrderManager::replaceOrder(ActiveOrder& order, double new_price) {
    const std::string market = order.market;
    const OrderSide side = order.side;
    const double volume = order.volume;
    const std::string old_id = order.order_id;
    const int next_retry_count = order.retry_count + 1;

    const std::string strategy_name = order.strategy_name;
    const double stop_loss = order.stop_loss;
    const double take_profit_1 = order.take_profit_1;
    const double take_profit_2 = order.take_profit_2;
    const double breakeven_trigger = order.breakeven_trigger;
    const double trailing_start = order.trailing_start;

    if (!cancelOrder(old_id)) {
        return false;
    }

    if (!submitOrder(market, side, new_price, volume,
                     strategy_name, stop_loss, take_profit_1, take_profit_2,
                     breakeven_trigger, trailing_start)) {
        return false;
    }

    long long latest_created = std::numeric_limits<long long>::min();
    std::string latest_id;
    for (const auto& pair : active_orders_) {
        if (pair.second.market == market && pair.second.created_at_ms > latest_created) {
            latest_created = pair.second.created_at_ms;
            latest_id = pair.first;
        }
    }

    if (!latest_id.empty()) {
        auto it = active_orders_.find(latest_id);
        if (it != active_orders_.end()) {
            it->second.retry_count = next_retry_count;
            it->second.last_chase_price = new_price;
        }
    }

    return true;
}

size_t OrderManager::getActiveOrderCount() const {
    std::lock_guard<std::recursive_mutex> lock(orders_mutex_);
    return active_orders_.size();
}

size_t OrderManager::getActiveBuyOrderCount() const {
    std::lock_guard<std::recursive_mutex> lock(orders_mutex_);
    size_t count = 0;
    for (const auto& pair : active_orders_) {
        if (pair.second.side == OrderSide::BUY) {
            ++count;
        }
    }
    return count;
}

} // namespace execution
} // namespace autolife
