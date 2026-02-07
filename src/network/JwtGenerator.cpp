#include "network/JwtGenerator.h"
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>
#include <sstream>
#include <iomanip>
#include <random>
#include <chrono>

namespace autolife {
namespace network {

// Base64 URL Encoding
std::string JwtGenerator::base64UrlEncode(const std::string& data) {
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO* bio = BIO_new(BIO_s_mem());
    bio = BIO_push(b64, bio);
    
    BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(bio, data.c_str(), static_cast<int>(data.length()));
    BIO_flush(bio);
    
    BUF_MEM* buffer_ptr;
    BIO_get_mem_ptr(bio, &buffer_ptr);
    
    std::string result(buffer_ptr->data, buffer_ptr->length);
    BIO_free_all(bio);
    
    // URL safe로 변환
    std::replace(result.begin(), result.end(), '+', '-');
    std::replace(result.begin(), result.end(), '/', '_');
    result.erase(std::remove(result.begin(), result.end(), '='), result.end());
    
    return result;
}

std::string JwtGenerator::generate(
    const std::string& access_key,
    const std::string& secret_key,
    const std::map<std::string, std::string>& query_params
) {
    // JWT Header
    nlohmann::json header;
    header["alg"] = "HS256";
    header["typ"] = "JWT";
    
    std::string header_b64 = base64UrlEncode(header.dump());
    
    // JWT Payload
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    ).count();
    
    nlohmann::json payload;
    payload["access_key"] = access_key;
    payload["nonce"] = generateUUID();
    payload["timestamp"] = timestamp;
    
    // Query params가 있으면 query_hash 추가
    if (!query_params.empty()) {
        std::string query_hash = createQueryHash(query_params);
        payload["query_hash"] = query_hash;
        payload["query_hash_alg"] = "SHA512";
    }
    
    std::string payload_b64 = base64UrlEncode(payload.dump());
    
    // Signature 생성 (HMAC-SHA256)
    std::string message = header_b64 + "." + payload_b64;
    
    unsigned char signature[EVP_MAX_MD_SIZE];
    unsigned int signature_len;
    
    HMAC(EVP_sha256(),
         secret_key.c_str(), static_cast<int>(secret_key.length()),
         reinterpret_cast<const unsigned char*>(message.c_str()), message.length(),
         signature, &signature_len);
    
    std::string signature_str(reinterpret_cast<char*>(signature), signature_len);
    std::string signature_b64 = base64UrlEncode(signature_str);
    
    // JWT 토큰 조립
    return header_b64 + "." + payload_b64 + "." + signature_b64;
}

std::string JwtGenerator::generateUUID() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dis;
    
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    
    uint64_t part1 = dis(gen);
    uint64_t part2 = dis(gen);
    
    oss << std::setw(8) << (part1 >> 32)
        << "-" << std::setw(4) << ((part1 >> 16) & 0xFFFF)
        << "-4" << std::setw(3) << (part1 & 0xFFF)
        << "-" << std::setw(4) << (((part2 >> 48) & 0x3FFF) | 0x8000)
        << "-" << std::setw(12) << (part2 & 0xFFFFFFFFFFFF);
    
    return oss.str();
}

std::string JwtGenerator::createQueryHash(const std::map<std::string, std::string>& params) {
    std::ostringstream query_stream;
    bool first = true;
    for (const auto& [key, value] : params) {
        if (!first) query_stream << "&";
        query_stream << key << "=" << value;
        first = false;
    }
    std::string query_string = query_stream.str();
    
    unsigned char hash[SHA512_DIGEST_LENGTH];
    SHA512(reinterpret_cast<const unsigned char*>(query_string.c_str()),
           query_string.length(), hash);
    
    std::ostringstream hex_stream;
    hex_stream << std::hex << std::setfill('0');
    for (int i = 0; i < SHA512_DIGEST_LENGTH; ++i) {
        hex_stream << std::setw(2) << static_cast<int>(hash[i]);
    }
    
    return hex_stream.str();
}

} // namespace network
} // namespace autolife
