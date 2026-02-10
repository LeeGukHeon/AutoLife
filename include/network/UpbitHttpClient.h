#pragma once

#include "network/IHttpClient.h"
#include "network/JwtGenerator.h"
#include "execution/RateLimiter.h"
#include <curl/curl.h>
#include <memory>
#include <mutex>

namespace autolife {
namespace network {

class UpbitHttpClient : public IHttpClient {
public:
    UpbitHttpClient(const std::string& access_key, const std::string& secret_key);
    ~UpbitHttpClient();
    
    HttpResponse get(
        const std::string& endpoint,
        const std::map<std::string, std::string>& query_params = {}
    ) override;
    
    HttpResponse post(
        const std::string& endpoint,
        const nlohmann::json& body
    ) override;
    
    HttpResponse del(
        const std::string& endpoint,
        const std::map<std::string, std::string>& query_params = {}
    ) override;
    
    // 업비트 전용 API 래퍼
    nlohmann::json getAccounts();
    nlohmann::json getMarkets();
    nlohmann::json getTicker(const std::string& market);
    nlohmann::json getOrderBook(const std::string& market);
    nlohmann::json getCandles(const std::string& market, const std::string& unit, int count);
    nlohmann::json getCandlesDays(const std::string& market, int count);
    
    // 배치 요청 (여러 마켓 한번에)
    nlohmann::json getTickerBatch(const std::vector<std::string>& markets);
    nlohmann::json getOrderBookBatch(const std::vector<std::string>& markets);
    
    // 주문 관련
    nlohmann::json placeOrder(
        const std::string& market,
        const std::string& side,
        const std::string& volume,
        const std::string& price = "",
        const std::string& ord_type = "limit"
    );
    nlohmann::json cancelOrder(const std::string& uuid);
    nlohmann::json getOrder(const std::string& uuid);
    
private:
    std::string access_key_;
    std::string secret_key_;
    std::string base_url_;
    CURL* curl_;
    std::mutex mutex_;
    std::shared_ptr<execution::RateLimiter> rate_limiter_;
    
    HttpResponse performRequest(
        const std::string& method,
        const std::string& url,
        const std::string& body_data,
        const std::map<std::string, std::string>& headers
    );
    
    static size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp);
    static size_t headerCallback(char* buffer, size_t size, size_t nitems, void* userdata);
    
    std::string buildQueryString(const std::map<std::string, std::string>& params);
};

} // namespace network
} // namespace autolife
