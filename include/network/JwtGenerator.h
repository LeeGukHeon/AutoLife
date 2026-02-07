#pragma once

#include <string>
#include <map>
#include <nlohmann/json.hpp>

namespace autolife {
namespace network {

class JwtGenerator {
public:
    // JWT 토큰 생성 (업비트 인증용)
    static std::string generate(
        const std::string& access_key,
        const std::string& secret_key,
        const std::map<std::string, std::string>& query_params = {}
    );
    
    // UUID 생성
    static std::string generateUUID();
    
    // Query Hash 생성 (POST 요청용)
    static std::string createQueryHash(const std::map<std::string, std::string>& params);
    
private:
    static std::string base64UrlEncode(const std::string& data);
};

} // namespace network
} // namespace autolife
