#pragma once

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <memory>
#include "common/Types.h"
#include "network/UpbitHttpClient.h"

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
    double take_profit;
    double breakeven_trigger;
    double trailing_start;
    
    // Smart Routing State
    bool is_chasing;
    double last_chase_price;
    long long last_update_ms;
};

class OrderManager {
public:
    OrderManager(std::shared_ptr<network::UpbitHttpClient> http_client);

    // Submit a new order
    // returns true if successfully sent to API
    bool submitOrder(const std::string& market, OrderSide side, double price, double volume,
                     const std::string& strategy_name, double sl, double tp, double be, double ts);

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

    // Retrieve and remove filled orders (for processing by Engine)
    std::vector<ActiveOrder> getFilledOrders();

private:
    std::shared_ptr<network::UpbitHttpClient> http_client_;
    
    mutable std::recursive_mutex orders_mutex_;
    std::map<std::string, ActiveOrder> active_orders_; // Key: order_id

    // Config for Limit Chase
    const long long CHASE_INTERVAL_MS = 5000; // 5 seconds
    const int MAX_CHASE_ATTEMPTS = 5;

    void checkLimitChase(ActiveOrder& order);
    bool replaceOrder(ActiveOrder& order, double new_price);
};

} // namespace execution
} // namespace autolife
