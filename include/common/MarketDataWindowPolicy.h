#pragma once

#include "common/Types.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace autolife::common {

struct TimeframeWindowRule {
    std::string_view timeframe_key;
    size_t target_bars;
    size_t min_required_bars;
    bool required_for_live_equivalent;
};

inline constexpr std::array<TimeframeWindowRule, 6> kTimeframeWindowRules = {{
    {"1m", 200, 120, true},
    {"5m", 120, 72, true},
    {"15m", 120, 48, true},
    {"1h", 120, 24, true},
    {"4h", 90, 12, true},
    {"1d", 60, 2, false},
}};

inline const TimeframeWindowRule* findTimeframeWindowRule(std::string_view timeframe_key) {
    for (const auto& rule : kTimeframeWindowRules) {
        if (rule.timeframe_key == timeframe_key) {
            return &rule;
        }
    }
    return nullptr;
}

inline size_t targetBarsForTimeframe(std::string_view timeframe_key, size_t fallback = 0) {
    const auto* rule = findTimeframeWindowRule(timeframe_key);
    return (rule != nullptr) ? rule->target_bars : fallback;
}

inline size_t minRequiredBarsForTimeframe(std::string_view timeframe_key, size_t fallback = 0) {
    const auto* rule = findTimeframeWindowRule(timeframe_key);
    return (rule != nullptr) ? rule->min_required_bars : fallback;
}

inline void trimCandlesByPolicy(std::map<std::string, std::vector<Candle>>& candles_by_tf) {
    for (auto& item : candles_by_tf) {
        const auto* rule = findTimeframeWindowRule(item.first);
        if (rule == nullptr) {
            continue;
        }
        if (item.second.size() > rule->target_bars) {
            item.second.erase(
                item.second.begin(),
                item.second.end() - static_cast<std::ptrdiff_t>(rule->target_bars));
        }
    }
}

struct DataWindowCheckResult {
    bool pass = true;
    std::vector<std::string> missing_timeframes;
    std::map<std::string, std::pair<size_t, size_t>> insufficient_timeframes;  // observed, required
};

inline DataWindowCheckResult checkLiveEquivalentWindow(
    const std::map<std::string, std::vector<Candle>>& candles_by_tf
) {
    DataWindowCheckResult out;
    for (const auto& rule : kTimeframeWindowRules) {
        if (!rule.required_for_live_equivalent) {
            continue;
        }
        const auto it = candles_by_tf.find(std::string(rule.timeframe_key));
        if (it == candles_by_tf.end() || it->second.empty()) {
            out.pass = false;
            out.missing_timeframes.push_back(std::string(rule.timeframe_key));
            continue;
        }
        if (it->second.size() < rule.min_required_bars) {
            out.pass = false;
            out.insufficient_timeframes[std::string(rule.timeframe_key)] =
                {it->second.size(), rule.min_required_bars};
        }
    }
    return out;
}

inline bool hasLiveEquivalentCompanionSet(
    const std::map<std::string, std::vector<Candle>>& candles_by_tf
) {
    for (const auto tf : {"5m", "1h", "4h"}) {
        const auto it = candles_by_tf.find(tf);
        if (it == candles_by_tf.end() || it->second.empty()) {
            return false;
        }
    }
    return true;
}

inline std::string buildWindowCheckSummary(const DataWindowCheckResult& result) {
    if (result.pass) {
        return "ok";
    }

    std::ostringstream oss;
    bool wrote = false;

    if (!result.missing_timeframes.empty()) {
        oss << "missing=";
        for (size_t i = 0; i < result.missing_timeframes.size(); ++i) {
            if (i > 0) {
                oss << ",";
            }
            oss << result.missing_timeframes[i];
        }
        wrote = true;
    }

    if (!result.insufficient_timeframes.empty()) {
        if (wrote) {
            oss << " ";
        }
        oss << "insufficient=";
        size_t idx = 0;
        for (const auto& item : result.insufficient_timeframes) {
            if (idx++ > 0) {
                oss << ",";
            }
            oss << item.first << "(" << item.second.first << "/" << item.second.second << ")";
        }
    }

    return oss.str();
}

}  // namespace autolife::common
