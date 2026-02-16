#include "common/Config.h"
#include "common/Logger.h"
#include "common/PathUtils.h"
#include "network/UpbitHttpClient.h"

#include <iostream>
#include <string>

namespace {

void printUsage() {
    std::cout << "Usage: AutoLifeLiveQuotationProbe [--market KRW-BTC] [--candles 5]\n";
}

bool tryParseInt(const std::string& text, int& value) {
    try {
        value = std::stoi(text);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

int main(int argc, char* argv[]) {
    std::string market = "KRW-BTC";
    int candles = 5;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--market" && i + 1 < argc) {
            market = argv[++i];
            continue;
        }
        if (arg == "--candles" && i + 1 < argc) {
            if (!tryParseInt(argv[++i], candles) || candles <= 0 || candles > 200) {
                std::cerr << "Invalid --candles value (1~200)\n";
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
        if (access_key.empty() || secret_key.empty()
            || access_key == "YOUR_ACCESS_KEY" || secret_key == "YOUR_SECRET_KEY") {
            std::cerr << "Missing API keys. Set env or config before probe.\n";
            return 1;
        }

        auto http_client = std::make_shared<autolife::network::UpbitHttpClient>(access_key, secret_key);

        const auto markets = http_client->getMarkets();
        const auto ticker = http_client->getTicker(market);
        const auto orderbook = http_client->getOrderBook(market);
        const auto candle_1m = http_client->getCandles(market, "1", candles);

        std::cout << "Quotation probe success\n";
        std::cout << "market=" << market << "\n";
        std::cout << "markets_count=" << (markets.is_array() ? markets.size() : 0) << "\n";
        std::cout << "ticker_items=" << (ticker.is_array() ? ticker.size() : 0) << "\n";
        std::cout << "orderbook_items=" << (orderbook.is_array() ? orderbook.size() : 0) << "\n";
        std::cout << "candles_1m_items=" << (candle_1m.is_array() ? candle_1m.size() : 0) << "\n";
        std::cout << "telemetry=" << autolife::utils::PathUtils::resolveRelativePath(
                         "logs/upbit_compliance_telemetry.jsonl").string()
                  << "\n";
        std::cout << "runtime_summary=" << autolife::utils::PathUtils::resolveRelativePath(
                         "logs/upbit_compliance_summary_runtime.json").string()
                  << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Quotation probe failed: " << e.what() << "\n";
        return 1;
    }
}
