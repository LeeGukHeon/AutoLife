#pragma once

#include <string>
#include <map>
#include <optional>
#include <nlohmann/json.hpp>

namespace autolife {
namespace network {

struct HttpResponse {
    int status_code;
    std::string body;
    std::map<std::string, std::string> headers;
    
    bool isSuccess() const { return status_code >= 200 && status_code < 300; }
    bool isRateLimited() const { return status_code == 429; }
    bool isBlocked() const { return status_code == 418; }
    
    nlohmann::json json() const {
        return nlohmann::json::parse(body);
    }
};

class IHttpClient {
public:
    virtual ~IHttpClient() = default;
    
    // GET 요청
    virtual HttpResponse get(
        const std::string& endpoint,
        const std::map<std::string, std::string>& query_params = {}
    ) = 0;
    
    // POST 요청
    virtual HttpResponse post(
        const std::string& endpoint,
        const nlohmann::json& body
    ) = 0;
    
    // DELETE 요청
    virtual HttpResponse del(
        const std::string& endpoint,
        const std::map<std::string, std::string>& query_params = {}
    ) = 0;
};

} // namespace network
} // namespace autolife
