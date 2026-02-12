#pragma once

#include <string>
#include <vector>
#include <map>
#include "common/Types.h"

namespace autolife {
namespace backtest {

class DataHistory {
public:
    // Load candles from a CSV file
    // Expected format: timestamp,open,high,low,close,volume
    static std::vector<Candle> loadCSV(const std::string& file_path);

    // Load candles from a JSON file (Upbit format)
    static std::vector<Candle> loadJSON(const std::string& file_path);

    // Filter candles by time range
    static std::vector<Candle> filterByDate(const std::vector<Candle>& candles, 
                                          const std::string& start_date, 
                                          const std::string& end_date);
};

} // namespace backtest
} // namespace autolife
