#include "core/adapters/UpbitComplianceAdapter.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <regex>
#include <string>
#include <utility>

#include "common/Logger.h"
#include "common/TickSizeHelper.h"

namespace autolife {
namespace core {
namespace {

constexpr auto kChanceCacheTtl = std::chrono::seconds(30);
constexpr auto kChanceStaleGrace = std::chrono::minutes(3);
constexpr auto kInstrumentCacheTtl = std::chrono::minutes(10);
constexpr auto kMaxNoTradeDuration = std::chrono::minutes(5);
constexpr int kRemainingLowWatermark = 1;

std::string toLowerCopy(std::string value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); }
    );
    return value;
}

bool hasStringValue(const nlohmann::json& node, const std::string& expected) {
    if (!node.is_array()) {
        return false;
    }

    const std::string target = toLowerCopy(expected);
    for (const auto& item : node) {
        if (item.is_string() && toLowerCopy(item.get<std::string>()) == target) {
            return true;
        }
    }
    return false;
}

} // namespace

UpbitComplianceAdapter::UpbitComplianceAdapter(
    std::shared_ptr<network::UpbitHttpClient> http_client,
    risk::RiskManager& risk_manager,
    const engine::EngineConfig& config
)
    : http_client_(std::move(http_client))
    , risk_manager_(risk_manager)
    , config_(config)
    , no_trade_until_(std::chrono::steady_clock::time_point::min()) {}

PreTradeCheck UpbitComplianceAdapter::validateEntry(
    const ExecutionRequest& request,
    const strategy::Signal& signal
) {
    if (request.market.empty() || request.price <= 0.0 || request.volume <= 0.0) {
        return {false, "invalid_request"};
    }

    if (signal.position_size <= 0.0) {
        return {false, "invalid_position_size"};
    }

    std::string no_trade_reason;
    if (isNoTradeDegraded(no_trade_reason)) {
        return {false, "no_trade_degrade:" + no_trade_reason};
    }

    const bool allowed = risk_manager_.canEnterPosition(
        request.market,
        request.price,
        signal.position_size,
        signal.strategy_name
    );
    if (!allowed) {
        return {false, "risk_rejected"};
    }

    if (config_.mode != engine::TradingMode::LIVE) {
        return {true, "ok"};
    }

    if (!http_client_) {
        return {false, "http_client_unavailable"};
    }

    std::string reason;
    auto chance = getChanceCachedOrFetch(request.market, reason);
    if (!chance.has_value()) {
        return {false, reason.empty() ? "chance_unavailable" : reason};
    }

    reason.clear();
    if (!validateChanceConstraints(request, chance.value(), reason)) {
        triggerNoTradeDegrade("chance_violation", std::chrono::seconds(15));
        return {false, reason.empty() ? "chance_violation" : reason};
    }

    reason.clear();
    const double tick_size = getInstrumentTickSize(request.market, request.price, reason);
    if (tick_size > 0.0 && !isTickSizeAligned(request.price, tick_size)) {
        triggerNoTradeDegrade("tick_size_violation", std::chrono::seconds(15));
        return {false, "invalid_tick_size"};
    }
    if (tick_size <= 0.0 && !reason.empty()) {
        return {false, reason};
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (consecutive_violation_count_ > 0) {
            --consecutive_violation_count_;
        }
    }

    return {true, "ok"};
}

PreTradeCheck UpbitComplianceAdapter::validateExit(
    const std::string& market,
    const risk::Position& position,
    double exit_price
) {
    if (market.empty() || exit_price <= 0.0) {
        return {false, "invalid_exit_request"};
    }

    if (position.quantity <= 0.0) {
        return {false, "empty_position"};
    }

    return {true, "ok"};
}

std::optional<nlohmann::json> UpbitComplianceAdapter::getChanceCachedOrFetch(
    const std::string& market,
    std::string& failure_reason
) {
    const auto now = std::chrono::steady_clock::now();
    std::optional<nlohmann::json> stale_cache;
    std::chrono::steady_clock::time_point stale_time = now;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = chance_cache_.find(market);
        if (it != chance_cache_.end()) {
            const auto age = now - it->second.fetched_at;
            if (age <= kChanceCacheTtl) {
                return it->second.payload;
            }
            stale_cache = it->second.payload;
            stale_time = it->second.fetched_at;
        }
    }

    network::HttpResponse response;
    try {
        response = http_client_->get("/v1/orders/chance", {{"market", market}});
    } catch (const std::exception& e) {
        if (stale_cache.has_value() && (now - stale_time) <= kChanceStaleGrace) {
            failure_reason = "chance_fetch_exception_stale_fallback";
            return stale_cache.value();
        }
        triggerNoTradeDegrade("chance_fetch_exception", std::chrono::seconds(20));
        failure_reason = std::string("chance_fetch_exception:") + e.what();
        return std::nullopt;
    }

    observeRateLimitResponse(response, "orders/chance");

    if (!response.isSuccess()) {
        if (stale_cache.has_value() && (now - stale_time) <= kChanceStaleGrace) {
            failure_reason = "chance_http_error_stale_fallback";
            return stale_cache.value();
        }
        triggerNoTradeDegrade("chance_http_error", std::chrono::seconds(20));
        failure_reason = "chance_http_error";
        return std::nullopt;
    }

    try {
        auto payload = response.json();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            chance_cache_[market] = ChanceCacheEntry{payload, now};
        }
        return payload;
    } catch (const std::exception& e) {
        triggerNoTradeDegrade("chance_parse_error", std::chrono::seconds(20));
        failure_reason = std::string("chance_parse_error:") + e.what();
        return std::nullopt;
    }
}

bool UpbitComplianceAdapter::validateChanceConstraints(
    const ExecutionRequest& request,
    const nlohmann::json& chance,
    std::string& failure_reason
) {
    if (!chance.is_object()) {
        failure_reason = "chance_invalid_payload";
        return false;
    }

    if (chance.contains("error")) {
        failure_reason = "chance_error_payload";
        return false;
    }

    if (!chance.contains("market") || !chance["market"].is_object()) {
        failure_reason = "chance_missing_market";
        return false;
    }

    const auto& market = chance["market"];
    const std::string side_key = request.side == OrderSide::BUY ? "bid" : "ask";
    const std::string side_name = request.side == OrderSide::BUY ? "bid" : "ask";

    if (market.contains("state")) {
        const std::string state = toLowerCopy(market.value("state", ""));
        if (!state.empty() && state != "active") {
            failure_reason = "market_not_active";
            return false;
        }
    }

    if (market.contains("order_sides") && !hasStringValue(market["order_sides"], side_name)) {
        failure_reason = "side_not_supported";
        return false;
    }

    if (market.contains("order_types") && !hasStringValue(market["order_types"], "limit")) {
        failure_reason = "limit_order_not_supported";
        return false;
    }

    const char* type_key = request.side == OrderSide::BUY ? "bid_types" : "ask_types";
    if (market.contains(type_key) && !hasStringValue(market[type_key], "limit")) {
        failure_reason = "side_limit_order_not_supported";
        return false;
    }

    double min_total = 0.0;
    if (market.contains(side_key) && market[side_key].is_object()) {
        min_total = readJsonNumber(market[side_key], "min_total");
    }

    if (min_total > 0.0) {
        const double notional = request.price * request.volume;
        if (notional + 1e-9 < min_total) {
            failure_reason = "below_min_total";
            return false;
        }
    }

    return true;
}

double UpbitComplianceAdapter::getInstrumentTickSize(
    const std::string& market,
    double reference_price,
    std::string& failure_reason
) {
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = instrument_cache_.find(market);
        if (it != instrument_cache_.end() && (now - it->second.fetched_at) <= kInstrumentCacheTtl) {
            return it->second.tick_size;
        }
    }

    if (!http_client_) {
        failure_reason = "http_client_unavailable";
        return 0.0;
    }

    network::HttpResponse response;
    try {
        response = http_client_->get("/v1/orderbook/instruments", {{"markets", market}});
    } catch (const std::exception&) {
        // fall back to local KRW tick policy
        const double fallback = common::getTickSize(reference_price);
        if (fallback > 0.0) {
            std::lock_guard<std::mutex> lock(mutex_);
            instrument_cache_[market] = InstrumentCacheEntry{fallback, false, now};
            return fallback;
        }
        failure_reason = "instrument_fetch_exception";
        return 0.0;
    }

    observeRateLimitResponse(response, "orderbook/instruments");

    if (!response.isSuccess()) {
        const double fallback = common::getTickSize(reference_price);
        if (fallback > 0.0) {
            std::lock_guard<std::mutex> lock(mutex_);
            instrument_cache_[market] = InstrumentCacheEntry{fallback, false, now};
            return fallback;
        }
        failure_reason = "instrument_http_error";
        return 0.0;
    }

    try {
        const auto payload = response.json();
        double tick_size = extractTickSizeFromInstrumentPayload(payload, market);
        bool from_exchange = tick_size > 0.0;
        if (tick_size <= 0.0) {
            tick_size = common::getTickSize(reference_price);
            from_exchange = false;
        }

        if (tick_size <= 0.0) {
            failure_reason = "instrument_tick_missing";
            return 0.0;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        instrument_cache_[market] = InstrumentCacheEntry{tick_size, from_exchange, now};
        return tick_size;
    } catch (const std::exception& e) {
        failure_reason = std::string("instrument_parse_error:") + e.what();
        return 0.0;
    }
}

bool UpbitComplianceAdapter::isTickSizeAligned(double price, double tick_size) const {
    if (price <= 0.0 || tick_size <= 0.0) {
        return false;
    }

    const double normalized = price / tick_size;
    const double nearest = std::round(normalized);
    const double tolerance = std::max(1e-8, std::abs(normalized) * 1e-10);
    return std::abs(normalized - nearest) <= tolerance;
}

void UpbitComplianceAdapter::observeRateLimitResponse(
    const network::HttpResponse& response,
    const std::string& source_tag
) {
    auto header_it = response.headers.find("Remaining-Req");
    if (header_it != response.headers.end()) {
        auto parsed = parseRemainingReq(header_it->second);
        if (parsed.has_value()) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                remaining_req_cache_[parsed->first] = RemainingReqSnapshot{
                    parsed->second,
                    std::chrono::steady_clock::now()
                };
            }

            if (parsed->second <= kRemainingLowWatermark) {
                triggerNoTradeDegrade("remaining_req_low:" + parsed->first, std::chrono::seconds(3));
                LOG_WARN("Compliance rate-limit pressure detected (source={}, group={}, sec={})",
                         source_tag, parsed->first, parsed->second);
            }
        }
    }

    if (response.status_code == 429) {
        triggerNoTradeDegrade("http_429", std::chrono::seconds(10));
    } else if (response.status_code == 418) {
        triggerNoTradeDegrade("http_418", std::chrono::seconds(60));
    }
}

void UpbitComplianceAdapter::triggerNoTradeDegrade(
    const std::string& reason,
    std::chrono::seconds base_duration
) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    ++consecutive_violation_count_;

    int exponent = consecutive_violation_count_ - 1;
    if (exponent < 0) {
        exponent = 0;
    }
    exponent = std::min(exponent, 5);

    auto duration = base_duration;
    for (int i = 0; i < exponent; ++i) {
        duration *= 2;
    }
    if (duration > kMaxNoTradeDuration) {
        duration = kMaxNoTradeDuration;
    }

    const auto candidate_until = now + duration;
    if (candidate_until > no_trade_until_) {
        no_trade_until_ = candidate_until;
    }
    no_trade_reason_ = reason;

    LOG_WARN("Compliance no-trade degrade activated: reason={}, duration={}s, violations={}",
             reason,
             std::chrono::duration_cast<std::chrono::seconds>(duration).count(),
             consecutive_violation_count_);
}

bool UpbitComplianceAdapter::isNoTradeDegraded(std::string& reason_out) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    if (now < no_trade_until_) {
        reason_out = no_trade_reason_.empty() ? "compliance_guard" : no_trade_reason_;
        return true;
    }

    if (no_trade_until_ != std::chrono::steady_clock::time_point::min()) {
        no_trade_until_ = std::chrono::steady_clock::time_point::min();
        no_trade_reason_.clear();
    }
    return false;
}

std::optional<std::pair<std::string, int>> UpbitComplianceAdapter::parseRemainingReq(
    const std::string& remaining_req_header
) {
    std::regex group_regex("group=([^;]+)");
    std::regex sec_regex("sec=([0-9]+)");

    std::smatch match;
    std::string group;
    int sec_remaining = -1;

    if (std::regex_search(remaining_req_header, match, group_regex) && match.size() > 1) {
        group = match[1];
    }

    if (std::regex_search(remaining_req_header, match, sec_regex) && match.size() > 1) {
        sec_remaining = std::stoi(match[1]);
    }

    if (group.empty() || sec_remaining < 0) {
        return std::nullopt;
    }
    return std::make_pair(group, sec_remaining);
}

double UpbitComplianceAdapter::readJsonNumber(
    const nlohmann::json& node,
    const char* key
) {
    if (!node.contains(key) || node[key].is_null()) {
        return 0.0;
    }
    if (node[key].is_number()) {
        return node[key].get<double>();
    }
    if (node[key].is_string()) {
        const auto value = node[key].get<std::string>();
        if (value.empty()) {
            return 0.0;
        }
        try {
            return std::stod(value);
        } catch (...) {
            return 0.0;
        }
    }
    return 0.0;
}

double UpbitComplianceAdapter::extractTickSizeFromInstrumentPayload(
    const nlohmann::json& payload,
    const std::string& market
) {
    auto extract_from_object = [](const nlohmann::json& obj) -> double {
        static const std::array<const char*, 3> kKeys = {"tick_size", "tickSize", "price_unit"};
        for (const auto* key : kKeys) {
            if (!obj.contains(key) || obj[key].is_null()) {
                continue;
            }
            if (obj[key].is_number()) {
                return obj[key].get<double>();
            }
            if (obj[key].is_string()) {
                try {
                    return std::stod(obj[key].get<std::string>());
                } catch (...) {
                    continue;
                }
            }
        }
        return 0.0;
    };

    auto pick_market_item = [&](const nlohmann::json& arr) -> nlohmann::json {
        if (!arr.is_array() || arr.empty()) {
            return nlohmann::json();
        }
        for (const auto& item : arr) {
            if (!item.is_object()) {
                continue;
            }
            if (!market.empty() && item.contains("market") && item["market"].is_string()) {
                if (item["market"].get<std::string>() == market) {
                    return item;
                }
            }
        }
        return arr.front();
    };

    if (payload.is_object()) {
        if (const double tick = extract_from_object(payload); tick > 0.0) {
            return tick;
        }
        if (payload.contains("data")) {
            const auto selected = pick_market_item(payload["data"]);
            if (selected.is_object()) {
                return extract_from_object(selected);
            }
        }
    }

    if (payload.is_array()) {
        const auto selected = pick_market_item(payload);
        if (selected.is_object()) {
            return extract_from_object(selected);
        }
    }

    return 0.0;
}

} // namespace core
} // namespace autolife
