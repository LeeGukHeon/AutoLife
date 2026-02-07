#pragma once

#include <string>
#include <map>
#include <chrono>
#include <mutex>
#include <queue>
#include <thread>
#include <condition_variable> // [추가] 필수

namespace autolife {
namespace execution {

// Rate Limit 그룹별 설정
struct RateLimitConfig {
    std::string group_name;
    int max_per_second;           // 초당 최대 요청 수
    int current_count;            // 현재 초의 요청 수
    std::chrono::steady_clock::time_point window_start;
    
    RateLimitConfig(const std::string& name, int max_req)
        : group_name(name)
        , max_per_second(max_req)
        , current_count(0)
        , window_start(std::chrono::steady_clock::now())
    {}
};

// Rate Limiter - 업비트 공식 제한 준수 (Thread-Safe & Efficient)
class RateLimiter {
public:
    RateLimiter();
    
    // 요청 전 호출 - 가능하면 true, 대기 필요하면 false (Non-blocking)
    bool tryAcquire(const std::string& group);
    
    // 요청 전 호출 - 필요시 자동으로 대기 (Blocking, Efficient)
    void acquire(const std::string& group);
    
    // 남은 요청 가능 횟수 확인
    int getRemainingRequests(const std::string& group);
    
    // Remaining-Req 헤더 파싱 및 업데이트
    void updateFromHeader(const std::string& remaining_req_header);
    
    // 긴급 상황 (429, 418 에러) 대응
    void handleRateLimitError(int status_code);
    
    // 통계 조회
    struct Stats {
        int total_requests;
        int rejected_requests;
        int forced_waits;
        std::chrono::milliseconds total_wait_time;
    };
    Stats getStats() const;
    
private:
    std::map<std::string, RateLimitConfig> configs_;
    mutable std::mutex mutex_;
    std::condition_variable cv_; // [핵심 추가] 효율적인 대기 관리
    
    // 통계
    int total_requests_;
    int rejected_requests_;
    int forced_waits_;
    std::chrono::milliseconds total_wait_time_;
    
    // 차단 상태
    bool is_blocked_;
    std::chrono::steady_clock::time_point block_end_time_;
    
    // 윈도우 리셋 체크
    void resetWindowIfNeeded(RateLimitConfig& config);
};

} // namespace execution
} // namespace autolife