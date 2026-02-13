#pragma once

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <memory>
#include "common/Types.h"
#include "network/UpbitHttpClient.h"
#include "network/UpbitMyOrderWebSocketClient.h"

namespace autolife {
namespace execution {

struct ActiveOrder {
    std::string order_id;
    std::string market;
    OrderSide side;
    double price;
    double volume;
    double filled_volume;
    long long created_at_ms;
    int retry_count;
    
    // Strategy Metadata (passed for async position tracking)
    std::string strategy_name;
    double stop_loss;
    double take_profit_1;
    double take_profit_2;
    double breakeven_trigger;
    double trailing_start;
    
    // Smart Routing State
    bool is_chasing;
    double last_chase_price;
    long long last_update_ms;

    // Exchange synchronization state
    OrderStatus status;
    long long last_state_sync_ms;
};

class OrderManager {
public:
    OrderManager(std::shared_ptr<network::UpbitHttpClient> http_client, bool enable_my_order_ws = true);
    ~OrderManager();

    // Submit a new order
    // returns true if successfully sent to API
    bool submitOrder(const std::string& market, OrderSide side, double price, double volume,
                     const std::string& strategy_name, double sl, double tp1, double tp2, double be, double ts,
                     std::string* submitted_order_id = nullptr);

    // Cancel a specific order
    bool cancelOrder(const std::string& order_id);
    
    // Check if there is an active order for a market
    bool hasActiveOrder(const std::string& market) const;

    // Monitor and update all active orders (called every tick)
    // - Syncs status with API
    // - Performs Limit Chase if needed
    void monitorOrders();

    // Get number of active orders
    size_t getActiveOrderCount() const;

    // Get number of active BUY orders (for position limit check)
    size_t getActiveBuyOrderCount() const;

    // Retrieve and remove filled orders (for processing by Engine)
    std::vector<ActiveOrder> getFilledOrders();

private:
    std::shared_ptr<network::UpbitHttpClient> http_client_;
    std::unique_ptr<network::UpbitMyOrderWebSocketClient> my_order_ws_client_;
    bool my_order_ws_enabled_ = false;
    
    mutable std::recursive_mutex orders_mutex_;
    std::map<std::string, ActiveOrder> active_orders_; // Key: order_id

    const long long REST_SYNC_INTERVAL_MS = 15000;
    const long long WS_STALE_THRESHOLD_MS = 45000;

    // Config for Limit Chase
    const long long CHASE_INTERVAL_MS = 5000; // 5 seconds
    const int MAX_CHASE_ATTEMPTS = 5;

    bool submitMarketFallback(const ActiveOrder& order);
    void syncOrderFillFromExchange(const std::string& order_id);
    void checkLimitChase(ActiveOrder& order);
    bool replaceOrder(ActiveOrder& order, double new_price);
    void onMyOrderEvent(const nlohmann::json& message);
    void applyExchangeOrderState(const nlohmann::json& status, bool from_ws);
    bool shouldUseRestSync(const ActiveOrder& order, long long now_ms) const;
    static double parseJsonNumber(const nlohmann::json& json, const char* key);
    static bool isTerminalState(OrderStatus status);
};

} // namespace execution
} // namespace autolife
