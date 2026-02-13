#include "network/UpbitHttpClient.h"
#include "common/Logger.h"
#include <algorithm>
#include <sstream>
#include <thread>
#include <chrono>
#include <set>
#include <cctype>

namespace autolife {
namespace network {
namespace {
bool isSensitiveKey(const std::string& key) {
    static const std::set<std::string> kKeys = {
        "access_key", "secret_key", "authorization", "bearer",
        "jwt", "token", "api_key", "signature", "query_hash"
    };
    std::string lower = key;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return kKeys.find(lower) != kKeys.end();
}

void maskSensitiveJson(nlohmann::json& node) {
    if (node.is_object()) {
        for (auto it = node.begin(); it != node.end(); ++it) {
            if (isSensitiveKey(it.key())) {
                it.value() = "***";
            } else {
                maskSensitiveJson(it.value());
            }
        }
        return;
    }
    if (node.is_array()) {
        for (auto& item : node) {
            maskSensitiveJson(item);
        }
    }
}

std::string sanitizeForLog(const std::string& text) {
    try {
        auto j = nlohmann::json::parse(text);
        maskSensitiveJson(j);
        return j.dump();
    } catch (...) {
        return text;
    }
}
}

UpbitHttpClient::UpbitHttpClient(const std::string& access_key, const std::string& secret_key)
    : access_key_(access_key)
    , secret_key_(secret_key)
    , offline_mode_(access_key == "BACKTEST_KEY" || secret_key == "BACKTEST_SECRET")
    , base_url_("https://api.upbit.com")
    , rate_limiter_(std::make_shared<execution::RateLimiter>())
{
    curl_global_init(CURL_GLOBAL_ALL);
    curl_ = curl_easy_init();
    
    if (!curl_) {
        throw std::runtime_error("Failed to initialize CURL");
    }
}

UpbitHttpClient::~UpbitHttpClient() {
    if (curl_) {
        curl_easy_cleanup(curl_);
    }
    curl_global_cleanup();
}

HttpResponse UpbitHttpClient::get(
    const std::string& endpoint,
    const std::map<std::string, std::string>& query_params
) {
    std::string group = "default";
    if (endpoint.find("/v1/market/") != std::string::npos) group = "market";
    else if (endpoint.find("/v1/candles/") != std::string::npos) group = "candle";
    else if (endpoint.find("/v1/ticker") != std::string::npos) group = "ticker";
    else if (endpoint.find("/v1/orderbook") != std::string::npos) group = "orderbook";
    else if (endpoint.find("/v1/trades/") != std::string::npos) group = "trade";
    else if (endpoint.find("/v1/accounts") != std::string::npos) group = "accounts";
    else if (endpoint.find("/v1/order") != std::string::npos ||
             endpoint.find("/v1/orders") != std::string::npos) group = "order";
    
    rate_limiter_->acquire(group);
    
    std::string url = base_url_ + endpoint;
    
    if (!query_params.empty()) {
        url += "?" + buildQueryString(query_params);
    }
    
    std::string jwt_token = JwtGenerator::generate(access_key_, secret_key_, query_params);
    
    std::map<std::string, std::string> headers;
    headers["Authorization"] = "Bearer " + jwt_token;
    headers["Content-Type"] = "application/json";
    
    auto response = performRequest("GET", url, "", headers);
    
    if (response.headers.find("Remaining-Req") != response.headers.end()) {
        rate_limiter_->updateFromHeader(response.headers["Remaining-Req"]);
    }
    
    if (response.isRateLimited() || response.isBlocked()) {
        rate_limiter_->handleRateLimitError(response.status_code);
    }
    
    return response;
}

HttpResponse UpbitHttpClient::post(
    const std::string& endpoint,
    const nlohmann::json& body
) {
    std::string group = "order";
    if (endpoint.find("/v1/orderbook") != std::string::npos) group = "orderbook";
    else if (endpoint.find("/v1/ticker") != std::string::npos) group = "ticker";
    else if (endpoint.find("/v1/candles/") != std::string::npos) group = "candle";

    rate_limiter_->acquire(group);
    
    std::string url = base_url_ + endpoint;
    
    // [핵심 수정] JSON을 순회하며 단순 문자열 맵으로 변환
    // dump()를 쓰지 않고 타입을 체크하여 변환해야 안전함
    std::map<std::string, std::string> query_params;
    
    for (auto& [key, value] : body.items()) {
        if (value.is_string()) {
            query_params[key] = value.get<std::string>(); // 따옴표 없는 순수 문자열
        } else {
            query_params[key] = value.dump(); // 숫자 등은 문자열로 변환
        }
    }
    
    // 이 map을 그대로 JwtGenerator에 넘김 (순수 데이터)
    std::string jwt_token = JwtGenerator::generate(access_key_, secret_key_, query_params);
    
    std::map<std::string, std::string> headers;
    headers["Authorization"] = "Bearer " + jwt_token;
    headers["Content-Type"] = "application/json";
    
    // Body는 원본 JSON 그대로 전송
    auto response = performRequest("POST", url, body.dump(), headers);

    if (response.headers.find("Remaining-Req") != response.headers.end()) {
        rate_limiter_->updateFromHeader(response.headers["Remaining-Req"]);
    }

    if (response.isRateLimited() || response.isBlocked()) {
        rate_limiter_->handleRateLimitError(response.status_code);
    }

    return response;
}

HttpResponse UpbitHttpClient::del(
    const std::string& endpoint,
    const std::map<std::string, std::string>& query_params
) {
    rate_limiter_->acquire("order");
    
    std::string url = base_url_ + endpoint;
    
    if (!query_params.empty()) {
        url += "?" + buildQueryString(query_params);
    }
    
    std::string jwt_token = JwtGenerator::generate(access_key_, secret_key_, query_params);
    
    std::map<std::string, std::string> headers;
    headers["Authorization"] = "Bearer " + jwt_token;
    
    auto response = performRequest("DELETE", url, "", headers);
    
    if (response.headers.find("Remaining-Req") != response.headers.end()) {
        rate_limiter_->updateFromHeader(response.headers["Remaining-Req"]);
    }
    
    if (response.isRateLimited() || response.isBlocked()) {
        rate_limiter_->handleRateLimitError(response.status_code);
    }
    
    return response;
}

nlohmann::json UpbitHttpClient::getAccounts() {
    if (offline_mode_) {
        return nlohmann::json::array();
    }
    auto response = get("/v1/accounts");
    if (response.isSuccess()) {
        return response.json();
    }
    throw std::runtime_error("Failed to get accounts: " + response.body);
}

nlohmann::json UpbitHttpClient::getMarkets() {
    if (offline_mode_) {
        return nlohmann::json::array();
    }
    auto response = get("/v1/market/all");
    if (response.isSuccess()) {
        return response.json();
    }
    throw std::runtime_error("Failed to get markets: " + response.body);
}

nlohmann::json UpbitHttpClient::getTicker(const std::string& market) {
    if (offline_mode_) {
        (void)market;
        return nlohmann::json::array();
    }
    std::map<std::string, std::string> params;
    params["markets"] = market;
    
    auto response = get("/v1/ticker", params);
    if (response.isSuccess()) {
        return response.json();
    }
    throw std::runtime_error("Failed to get ticker: " + response.body);
}

nlohmann::json UpbitHttpClient::getOrderBook(const std::string& market) {
    if (offline_mode_) {
        (void)market;
        return nlohmann::json::array();
    }
    std::map<std::string, std::string> params;
    params["markets"] = market;
    
    auto response = get("/v1/orderbook", params);
    if (response.isSuccess()) {
        return response.json();
    }
    throw std::runtime_error("Failed to get orderbook: " + response.body);
}

nlohmann::json UpbitHttpClient::getCandles(
    const std::string& market,
    const std::string& unit,
    int count
) {
    if (offline_mode_) {
        (void)market;
        (void)unit;
        (void)count;
        return nlohmann::json::array();
    }
    std::map<std::string, std::string> params;
    params["market"] = market;
    params["count"] = std::to_string(count);
    
    std::string endpoint = "/v1/candles/minutes/" + unit;
    auto response = get(endpoint, params);
    
    if (response.isSuccess()) {
        return response.json();
    }
    throw std::runtime_error("Failed to get candles: " + response.body);
}

nlohmann::json UpbitHttpClient::getCandlesDays(
    const std::string& market,
    int count
) {
    if (offline_mode_) {
        (void)market;
        (void)count;
        return nlohmann::json::array();
    }
    std::map<std::string, std::string> params;
    params["market"] = market;
    params["count"] = std::to_string(count);

    auto response = get("/v1/candles/days", params);

    if (response.isSuccess()) {
        return response.json();
    }
    throw std::runtime_error("Failed to get day candles: " + response.body);
}

// ========== 배치 요청 구현 ==========

nlohmann::json UpbitHttpClient::getTickerBatch(const std::vector<std::string>& markets) {
    if (offline_mode_) {
        (void)markets;
        return nlohmann::json::array();
    }
    std::map<std::string, std::string> params;
    
    // 마켓 리스트를 콤마로 연결
    std::ostringstream oss;
    for (size_t i = 0; i < markets.size(); ++i) {
        if (i > 0) oss << ",";
        oss << markets[i];
    }
    params["markets"] = oss.str();
    
    auto response = get("/v1/ticker", params);
    if (response.isSuccess()) {
        return response.json();
    }
    throw std::runtime_error("Failed to get ticker batch: " + response.body);
}

nlohmann::json UpbitHttpClient::getOrderBookBatch(const std::vector<std::string>& markets) {
    if (offline_mode_) {
        (void)markets;
        return nlohmann::json::array();
    }
    std::map<std::string, std::string> params;
    
    std::ostringstream oss;
    for (size_t i = 0; i < markets.size(); ++i) {
        if (i > 0) oss << ",";
        oss << markets[i];
    }
    params["markets"] = oss.str();
    
    auto response = get("/v1/orderbook", params);
    if (response.isSuccess()) {
        return response.json();
    }
    throw std::runtime_error("Failed to get orderbook batch: " + response.body);
}

nlohmann::json UpbitHttpClient::placeOrder(
    const std::string& market,
    const std::string& side,
    const std::string& volume,
    const std::string& price,
    const std::string& ord_type
) {
    if (offline_mode_) {
        throw std::runtime_error("placeOrder unavailable in offline backtest mode");
    }
    nlohmann::json body;
    body["market"] = market;
    body["side"] = side;
    body["ord_type"] = ord_type;

    if (!volume.empty()) {
        body["volume"] = volume;
    }
    if (!price.empty()) {
        body["price"] = price;
    }
    
    auto response = post("/v1/orders", body);
    
    if (response.isSuccess()) {
        LOG_INFO("주문 성공: {} {} {}", market, side, volume);
        return response.json();
    }
    
    const std::string safe_body = sanitizeForLog(response.body);
    LOG_ERROR("주문 실패: {}", safe_body);
    throw std::runtime_error("Failed to place order: " + safe_body);
}

nlohmann::json UpbitHttpClient::getOrder(const std::string& uuid) {
    if (offline_mode_) {
        (void)uuid;
        return nlohmann::json::object();
    }
    std::map<std::string, std::string> params;
    params["uuid"] = uuid;
    
    auto response = get("/v1/order", params);
    
    if (response.isSuccess()) {
        return response.json();
    }
    
    const std::string safe_body = sanitizeForLog(response.body);
    LOG_ERROR("주문 조회 실패: {} - {}", uuid, safe_body);
    throw std::runtime_error("Failed to get order: " + safe_body);
}


nlohmann::json UpbitHttpClient::cancelOrder(const std::string& uuid) {
    if (offline_mode_) {
        (void)uuid;
        return nlohmann::json::object();
    }
    std::map<std::string, std::string> params;
    params["uuid"] = uuid;
    
    auto response = del("/v1/order", params);
    
    if (response.isSuccess()) {
        return response.json();
    }
    throw std::runtime_error("Failed to cancel order: " + sanitizeForLog(response.body));
}

HttpResponse UpbitHttpClient::performRequest(
    const std::string& method,
    const std::string& url,
    const std::string& body_data,
    const std::map<std::string, std::string>& headers
) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    HttpResponse response;
    std::string response_body;
    std::map<std::string, std::string> response_headers;
    
    curl_easy_reset(curl_);
    curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl_, CURLOPT_HEADERFUNCTION, headerCallback);
    curl_easy_setopt(curl_, CURLOPT_HEADERDATA, &response_headers);
    curl_easy_setopt(curl_, CURLOPT_TIMEOUT, 30L);
    
    if (method == "POST") {
        curl_easy_setopt(curl_, CURLOPT_POST, 1L);
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, body_data.c_str());
    } else if (method == "DELETE") {
        curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, "DELETE");
    }
    
    struct curl_slist* header_list = nullptr;
    for (const auto& [key, value] : headers) {
        std::string header_line = key + ": " + value;
        header_list = curl_slist_append(header_list, header_line.c_str());
    }
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, header_list);
    
    CURLcode res = curl_easy_perform(curl_);
    
    if (res != CURLE_OK) {
        curl_slist_free_all(header_list);
        throw std::runtime_error("CURL error: " + std::string(curl_easy_strerror(res)));
    }
    
    long http_code = 0;
    curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_code);
    
    curl_slist_free_all(header_list);
    
    response.status_code = static_cast<int>(http_code);
    response.body = response_body;
    response.headers = response_headers;
    
    return response;
}

size_t UpbitHttpClient::writeCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total_size = size * nmemb;
    std::string* response_body = static_cast<std::string*>(userp);
    response_body->append(static_cast<char*>(contents), total_size);
    return total_size;
}

size_t UpbitHttpClient::headerCallback(char* buffer, size_t size, size_t nitems, void* userdata) {
    size_t total_size = size * nitems;
    std::string header_line(buffer, total_size);
    
    size_t colon_pos = header_line.find(':');
    if (colon_pos != std::string::npos) {
        std::string key = header_line.substr(0, colon_pos);
        std::string value = header_line.substr(colon_pos + 1);
        
        value.erase(0, value.find_first_not_of(" \t\r\n"));
        value.erase(value.find_last_not_of(" \t\r\n") + 1);
        
        auto* headers = static_cast<std::map<std::string, std::string>*>(userdata);
        (*headers)[key] = value;
    }
    
    return total_size;
}

std::string UpbitHttpClient::buildQueryString(const std::map<std::string, std::string>& params) {
    std::ostringstream oss;
    bool first = true;
    for (const auto& [key, value] : params) {
        if (!first) oss << "&";
        oss << key << "=" << value;
        first = false;
    }
    return oss.str();
}

} // namespace network
} // namespace autolife
