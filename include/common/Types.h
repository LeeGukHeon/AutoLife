#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <optional>

namespace autolife {

using Timestamp = std::chrono::system_clock::time_point;
using Price = double;
using Volume = double;
using Amount = double;

enum class OrderSide { BUY, SELL };
enum class OrderType { LIMIT, MARKET, STOP_LOSS, TAKE_PROFIT };
enum class OrderStatus { PENDING, SUBMITTED, FILLED, PARTIALLY_FILLED, CANCELLED, REJECTED };
enum class Signal { STRONG_BUY, BUY, HOLD, SELL, STRONG_SELL, NONE };

struct Order {
    std::string order_id;
    std::string market;
    OrderSide side;
    OrderType type;
    Price price;
    Volume volume;
    OrderStatus status;
    Timestamp created_at;
    std::string strategy_name;
};

struct Candle {
    double open;
    double high;
    double low;
    double close;
    double volume;
    long long timestamp;
    
    Candle() : open(0), high(0), low(0), close(0), volume(0), timestamp(0) {}
    
    Candle(double o, double h, double l, double c, double v, long long t)
        : open(o), high(h), low(l), close(c), volume(v), timestamp(t) {}
};

struct Position {
    std::string market;
    Volume size;
    Price avg_entry_price;
    Amount unrealized_pnl;
    Amount realized_pnl;
};

} // namespace autolife
