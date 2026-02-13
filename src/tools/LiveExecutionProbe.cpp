#include "common/Config.h"
#include "common/Logger.h"
#include "common/PathUtils.h"
#include "common/TickSizeHelper.h"
#include "execution/OrderManager.h"
#include "network/UpbitHttpClient.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace {

bool tryParseDouble(const std::string& text, double& value) {
    try {
        value = std::stod(text);
        return true;
    } catch (...) {
        return false;
    }
}

bool tryParseInt(const std::string& text, int& value) {
    try {
        value = std::stoi(text);
        return true;
    } catch (...) {
        return false;
    }
}

bool extractBestBidPrice(const nlohmann::json& orderbook, double& best_bid_price) {
    nlohmann::json units;
    if (orderbook.is_array() && !orderbook.empty() && orderbook[0].contains("orderbook_units")) {
        units = orderbook[0]["orderbook_units"];
    } else if (orderbook.is_object() && orderbook.contains("orderbook_units")) {
        units = orderbook["orderbook_units"];
    } else {
        return false;
    }

    if (!units.is_array() || units.empty() || !units[0].contains("bid_price")) {
        return false;
    }

    if (units[0]["bid_price"].is_string()) {
        best_bid_price = std::stod(units[0]["bid_price"].get<std::string>());
    } else {
        best_bid_price = units[0]["bid_price"].get<double>();
    }
    return best_bid_price > 0.0;
}

bool artifactContainsOrderId(const std::filesystem::path& artifact_path, const std::string& order_id) {
    if (order_id.empty() || !std::filesystem::exists(artifact_path)) {
        return false;
    }

    std::ifstream in(artifact_path, std::ios::binary);
    if (!in.is_open()) {
        return false;
    }

    std::string line;
    while (std::getline(in, line)) {
        if (line.find(order_id) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void printUsage() {
    std::cout << "Usage: AutoLifeLiveExecutionProbe [--market KRW-BTC] [--notional-krw 5100] "
              << "[--discount-pct 2.0] [--cancel-delay-ms 1500]\n";
}

} // namespace

int main(int argc, char* argv[]) {
    std::string market = "KRW-BTC";
    double notional_krw = 5100.0;
    double discount_pct = 2.0;
    int cancel_delay_ms = 1500;
    const std::string strategy_name = "Stage7ParityProbe";

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--market" && i + 1 < argc) {
            market = argv[++i];
            continue;
        }
        if (arg == "--notional-krw" && i + 1 < argc) {
            if (!tryParseDouble(argv[++i], notional_krw) || notional_krw <= 0.0) {
                std::cerr << "Invalid --notional-krw value\n";
                return 1;
            }
            continue;
        }
        if (arg == "--discount-pct" && i + 1 < argc) {
            if (!tryParseDouble(argv[++i], discount_pct) || discount_pct < 0.0 || discount_pct > 50.0) {
                std::cerr << "Invalid --discount-pct value\n";
                return 1;
            }
            continue;
        }
        if (arg == "--cancel-delay-ms" && i + 1 < argc) {
            if (!tryParseInt(argv[++i], cancel_delay_ms) || cancel_delay_ms < 0 || cancel_delay_ms > 120000) {
                std::cerr << "Invalid --cancel-delay-ms value\n";
                return 1;
            }
            continue;
        }
        if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        }

        std::cerr << "Unknown argument: " << arg << "\n";
        printUsage();
        return 1;
    }

    try {
        autolife::Logger::getInstance().initialize("logs");

        auto& cfg = autolife::Config::getInstance();
        cfg.load("config/config.json");

        const std::string access_key = cfg.getAccessKey();
        const std::string secret_key = cfg.getSecretKey();
        if (access_key.empty() || secret_key.empty() || access_key == "YOUR_ACCESS_KEY" || secret_key == "YOUR_SECRET_KEY") {
            std::cerr << "Missing or placeholder API keys in config/config.json\n";
            return 1;
        }

        auto http_client = std::make_shared<autolife::network::UpbitHttpClient>(access_key, secret_key);

        const auto orderbook = http_client->getOrderBook(market);
        double best_bid_price = 0.0;
        if (!extractBestBidPrice(orderbook, best_bid_price)) {
            std::cerr << "Failed to read best bid from orderbook for " << market << "\n";
            return 1;
        }

        double limit_price = best_bid_price * (1.0 - (discount_pct / 100.0));
        limit_price = autolife::common::roundDownToTickSize(limit_price);
        if (limit_price <= 0.0) {
            std::cerr << "Calculated limit price is invalid\n";
            return 1;
        }

        const double volume = notional_krw / limit_price;
        if (volume <= 0.0 || !std::isfinite(volume)) {
            std::cerr << "Calculated volume is invalid\n";
            return 1;
        }

        autolife::execution::OrderManager order_manager(http_client, false);

        std::string order_id;
        const bool submitted = order_manager.submitOrder(
            market,
            autolife::OrderSide::BUY,
            limit_price,
            volume,
            strategy_name,
            0.0,
            0.0,
            0.0,
            0.0,
            0.0,
            &order_id
        );

        if (!submitted || order_id.empty()) {
            std::cerr << "Probe order submission failed\n";
            return 1;
        }

        std::cout << "Submitted probe order: " << order_id << " (market=" << market
                  << ", price=" << limit_price << ", volume=" << volume << ")\n";

        if (cancel_delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(cancel_delay_ms));
        }

        bool cancel_ok = order_manager.cancelOrder(order_id);
        if (!cancel_ok) {
            auto status = http_client->getOrder(order_id);
            const std::string state = status.value("state", "");
            if (state != "done" && state != "cancel") {
                std::cerr << "Probe order is not terminal after cancel attempt. state=" << state << "\n";
                return 1;
            }
            std::cout << "Cancel returned false but order is terminal (state=" << state << ")\n";
        } else {
            std::cout << "Cancelled probe order: " << order_id << "\n";
        }

        const auto artifact_path = autolife::utils::PathUtils::resolveRelativePath("logs/execution_updates_live.jsonl");
        if (!artifactContainsOrderId(artifact_path, order_id)) {
            std::cerr << "Execution artifact missing probe order id: " << artifact_path.string() << "\n";
            return 1;
        }

        std::cout << "Execution artifact updated: " << artifact_path.string() << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Probe failed: " << e.what() << "\n";
        return 1;
    }
}
