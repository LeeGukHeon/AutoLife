#include "backtest/DataHistory.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <algorithm>
#include "common/Logger.h"

namespace autolife {
namespace backtest {

std::vector<Candle> DataHistory::loadCSV(const std::string& file_path) {
    std::vector<Candle> candles;
    std::ifstream file(file_path);
    
    if (!file.is_open()) {
        LOG_ERROR("Failed to open CSV file: {}", file_path);
        return candles;
    }

    std::string line;
    // Skip header if it exists (heuristic: check if first char is alpha)
    if (file.peek() != std::ifstream::traits_type::eof()) {
        char c = file.peek();
        if (std::isalpha(c)) {
            std::getline(file, line);
        }
    }

    while (std::getline(file, line)) {
        std::stringstream ss(line);
        std::string cell;
        std::vector<std::string> row;

        while (std::getline(ss, cell, ',')) {
            row.push_back(cell);
        }

        if (row.size() < 6) continue;

        try {
            Candle candle;
            // Assuming format: timestamp, open, high, low, close, volume
            candle.timestamp = std::stoll(row[0]);
            candle.open = std::stod(row[1]);
            candle.high = std::stod(row[2]);
            candle.low = std::stod(row[3]);
            candle.close = std::stod(row[4]);
            candle.volume = std::stod(row[5]);
            candles.push_back(candle);
        } catch (const std::exception& e) {
            LOG_WARN("Error parsing row: {} - {}", line, e.what());
        }
    }
    
    LOG_INFO("Loaded {} candles from {}", candles.size(), file_path);
    return candles;
}

std::vector<Candle> DataHistory::loadJSON(const std::string& file_path) {
    std::vector<Candle> candles;
    std::ifstream file(file_path);
    
    if (!file.is_open()) {
        LOG_ERROR("Failed to open JSON file: {}", file_path);
        return candles;
    }

    nlohmann::json j;
    try {
        file >> j;
        for (const auto& item : j) {
            Candle candle;
            // Upbit format often uses: timestamp, opening_price, high_price, low_price, trade_price, candle_acc_trade_volume
            // We need to adapt based on actual fields. Assuming standard keys for now.
            if (item.contains("timestamp")) candle.timestamp = item["timestamp"];
            else if (item.contains("t")) candle.timestamp = item["t"];
            
            if (item.contains("open")) candle.open = item["open"];
            else if (item.contains("o")) candle.open = item["o"];
            
            if (item.contains("high")) candle.high = item["high"];
            else if (item.contains("h")) candle.high = item["h"];
            
            if (item.contains("low")) candle.low = item["low"];
            else if (item.contains("l")) candle.low = item["l"];
            
            if (item.contains("close")) candle.close = item["close"];
            else if (item.contains("c")) candle.close = item["c"];
            
            if (item.contains("volume")) candle.volume = item["volume"];
            else if (item.contains("v")) candle.volume = item["v"];
            
            candles.push_back(candle);
        }
        // Ensure sorted by timestamp ascending
        std::sort(candles.begin(), candles.end(), [](const Candle& a, const Candle& b) {
            return a.timestamp < b.timestamp;
        });
        
    } catch (const std::exception& e) {
        LOG_ERROR("Error parsing JSON file: {} - {}", file_path, e.what());
    }

    LOG_INFO("Loaded {} candles from {}", candles.size(), file_path);
    return candles;
}

std::vector<Candle> DataHistory::filterByDate(const std::vector<Candle>& candles, 
                                            const std::string& start_date, 
                                            const std::string& end_date) {
    // Placeholder for date filtering logic
    // This requires converting string dates to timestamps
    return candles;
}

} // namespace backtest
} // namespace autolife
