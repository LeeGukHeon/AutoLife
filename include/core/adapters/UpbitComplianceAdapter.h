#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "core/contracts/IRiskCompliancePlane.h"
#include "engine/EngineConfig.h"
#include "network/UpbitHttpClient.h"
#include "risk/RiskManager.h"

namespace autolife {
namespace core {

class UpbitComplianceAdapter : public IRiskCompliancePlane {
public:
    UpbitComplianceAdapter(
        std::shared_ptr<network::UpbitHttpClient> http_client,
        risk::RiskManager& risk_manager,
        const engine::EngineConfig& config
    );

    PreTradeCheck validateEntry(
        const ExecutionRequest& request,
        const strategy::Signal& signal
    ) override;

    PreTradeCheck validateExit(
        const std::string& market,
        const risk::Position& position,
        double exit_price
    ) override;

private:
    struct ChanceCacheEntry {
        nlohmann::json payload;
        std::chrono::steady_clock::time_point fetched_at;
    };

    struct InstrumentCacheEntry {
        double tick_size = 0.0;
        bool from_exchange = false;
        std::chrono::steady_clock::time_point fetched_at;
    };

    struct RemainingReqSnapshot {
        int sec_remaining = -1;
        std::chrono::steady_clock::time_point updated_at;
    };

    std::optional<nlohmann::json> getChanceCachedOrFetch(
        const std::string& market,
        std::string& failure_reason
    );

    bool validateChanceConstraints(
        const ExecutionRequest& request,
        const nlohmann::json& chance,
        std::string& failure_reason
    );

    double getInstrumentTickSize(
        const std::string& market,
        double reference_price,
        std::string& failure_reason
    );

    bool isTickSizeAligned(double price, double tick_size) const;

    void observeRateLimitResponse(
        const network::HttpResponse& response,
        const std::string& source_tag
    );

    void triggerNoTradeDegrade(
        const std::string& reason,
        std::chrono::seconds base_duration
    );

    bool isNoTradeDegraded(std::string& reason_out);

    static std::optional<std::pair<std::string, int>> parseRemainingReq(
        const std::string& remaining_req_header
    );

    static double readJsonNumber(
        const nlohmann::json& node,
        const char* key
    );

    static double extractTickSizeFromInstrumentPayload(
        const nlohmann::json& payload,
        const std::string& market
    );

    std::shared_ptr<network::UpbitHttpClient> http_client_;
    risk::RiskManager& risk_manager_;
    const engine::EngineConfig& config_;

    std::mutex mutex_;
    std::unordered_map<std::string, ChanceCacheEntry> chance_cache_;
    std::unordered_map<std::string, InstrumentCacheEntry> instrument_cache_;
    std::unordered_map<std::string, RemainingReqSnapshot> remaining_req_cache_;
    int consecutive_violation_count_ = 0;
    std::chrono::steady_clock::time_point no_trade_until_;
    std::string no_trade_reason_;
};

} // namespace core
} // namespace autolife
