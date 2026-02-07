#include "execution/RateLimiter.h"
#include "common/Logger.h"
#include <regex>
#include <algorithm>

namespace autolife {
namespace execution {

RateLimiter::RateLimiter()
    : total_requests_(0)
    , rejected_requests_(0)
    , forced_waits_(0)
    , total_wait_time_(std::chrono::milliseconds(0))
    , is_blocked_(false)
{
    // 업비트 공식 Rate Limit (Quotation API - IP당)
    configs_.emplace("market", RateLimitConfig("market", 10));    // 종목 조회
    configs_.emplace("candle", RateLimitConfig("candle", 10));    // 캔들 조회
    configs_.emplace("ticker", RateLimitConfig("ticker", 10));    // 현재가 조회
    configs_.emplace("orderbook", RateLimitConfig("orderbook", 10)); // 호가 조회
    configs_.emplace("trade", RateLimitConfig("trade", 10));      // 체결 내역
    
    // Exchange API (주문/자산 - Key당)
    configs_.emplace("accounts", RateLimitConfig("accounts", 30)); // 자산 조회
    configs_.emplace("order", RateLimitConfig("order", 8));        // 주문 요청 (초당 8회)
    configs_.emplace("default", RateLimitConfig("default", 30));   // 기타
    
    LOG_INFO("RateLimiter 초기화 - 업비트 공식 제한 적용 (Condition Variable 최적화)");
}

bool RateLimiter::tryAcquire(const std::string& group) {
    std::unique_lock<std::mutex> lock(mutex_); // unique_lock 사용
    
    // 1. 차단 상태 확인
    if (is_blocked_) {
        auto now = std::chrono::steady_clock::now();
        if (now < block_end_time_) {
            rejected_requests_++;
            return false;
        }
        // 차단 시간 지났으면 해제
        is_blocked_ = false;
        LOG_INFO("API 차단 자동 해제 (TryAcquire)");
        cv_.notify_all(); // 대기 중인 다른 스레드들도 깨움
    }
    
    // 2. 설정 가져오기
    auto it = configs_.find(group);
    if (it == configs_.end()) it = configs_.find("default");
    auto& config = it->second;
    
    // 3. 윈도우 리셋 체크
    resetWindowIfNeeded(config);
    
    // 4. 토큰 확인
    if (config.current_count < config.max_per_second) {
        config.current_count++;
        total_requests_++;
        return true;
    }
    
    rejected_requests_++;
    return false;
}

void RateLimiter::acquire(const std::string& group) {
    std::unique_lock<std::mutex> lock(mutex_);
    
    auto it = configs_.find(group);
    if (it == configs_.end()) it = configs_.find("default");
    auto& config = it->second;
    
    // 조건이 충족될 때까지 효율적으로 대기 (Busy waiting 없음)
    while (true) {
        // 1. 차단 상태면 풀릴 때까지 대기
        if (is_blocked_) {
            auto status = cv_.wait_until(lock, block_end_time_);
            if (status == std::cv_status::timeout) {
                is_blocked_ = false; // 타임아웃 되면 차단 해제
            } else {
                continue; // 누군가 깨웠지만 아직 차단 중일 수 있으므로 다시 체크
            }
        }

        // 2. 윈도우 리셋 및 토큰 체크
        resetWindowIfNeeded(config);
        
        if (config.current_count < config.max_per_second) {
            // 자원 획득 성공!
            config.current_count++;
            total_requests_++;
            return; // 함수 종료
        }
        
        // 3. 자원 부족 -> 다음 윈도우 시작 지점까지 대기
        // window_start + 1초 + 아주 약간의 여유(1ms)
        auto wake_time = config.window_start + std::chrono::seconds(1) + std::chrono::milliseconds(1);
        
        forced_waits_++;
        auto wait_start = std::chrono::steady_clock::now();
        
        // 지정된 시간까지 sleep (도중에 윈도우가 리셋되어 notify가 오면 즉시 깨어남)
        cv_.wait_until(lock, wake_time);
        
        total_wait_time_ += std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - wait_start
        );
        // 루프 다시 돌면서 자원 획득 재시도
    }
}

int RateLimiter::getRemainingRequests(const std::string& group) {
    std::unique_lock<std::mutex> lock(mutex_);
    
    auto it = configs_.find(group);
    if (it == configs_.end()) it = configs_.find("default");
    
    resetWindowIfNeeded(it->second);
    
    return std::max(0, it->second.max_per_second - it->second.current_count);
}

void RateLimiter::updateFromHeader(const std::string& remaining_req_header) {
    // 예: "group=market; min=57; sec=9"
    std::regex group_regex("group=([^;]+)");
    std::regex sec_regex("sec=(\\d+)");
    
    std::smatch match;
    std::string group_name = "default";
    int remaining = -1;
    
    if (std::regex_search(remaining_req_header, match, group_regex)) {
        group_name = match[1];
    }
    
    if (std::regex_search(remaining_req_header, match, sec_regex)) {
        remaining = std::stoi(match[1]);
    }
    
    if (remaining >= 0) {
        std::unique_lock<std::mutex> lock(mutex_);
        
        auto it = configs_.find(group_name);
        if (it != configs_.end()) {
            // 서버 기준 잔여량이 로컬보다 적다면 보정 (보수적 접근)
            int used_remote = it->second.max_per_second - remaining;
            if (used_remote > it->second.current_count) {
                it->second.current_count = used_remote;
            }
        }
    }
}

void RateLimiter::handleRateLimitError(int status_code) {
    std::unique_lock<std::mutex> lock(mutex_);
    
    if (status_code == 429) {
        LOG_WARN("⚠️ 429 Too Many Requests 발생! (1초간 전체 일시정지)");
        
        forced_waits_++;
        is_blocked_ = true;
        block_end_time_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        
        // 여기서 락을 풀고 대기하는 것이 중요 (다른 스레드들이 acquire 진입 시 is_blocked_ 확인하게 함)
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::seconds(1));
        lock.lock();
        
        is_blocked_ = false;
        cv_.notify_all(); // 대기 중이던 모든 스레드 깨움
        
    } else if (status_code == 418) {
        LOG_ERROR("🚫 418 IP 차단 감지! (1분간 전체 정지)");
        
        is_blocked_ = true;
        block_end_time_ = std::chrono::steady_clock::now() + std::chrono::minutes(1);
        
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::minutes(1));
        lock.lock();
        
        is_blocked_ = false;
        cv_.notify_all();
    }
}

RateLimiter::Stats RateLimiter::getStats() const {
    std::unique_lock<std::mutex> lock(mutex_);
    
    Stats stats;
    stats.total_requests = total_requests_;
    stats.rejected_requests = rejected_requests_;
    stats.forced_waits = forced_waits_;
    stats.total_wait_time = total_wait_time_;
    
    return stats;
}

void RateLimiter::resetWindowIfNeeded(RateLimitConfig& config) {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - config.window_start
    );
    
    // 1초(1000ms)가 지났으면 카운트 리셋
    if (elapsed.count() >= 1000) {
        config.current_count = 0;
        config.window_start = now;
        
        // 윈도우가 리셋되었으므로 대기 중인 스레드들에게 알림
        // (이 부분이 Busy Waiting을 없애는 핵심)
        cv_.notify_all(); 
    }
}

} // namespace execution
} // namespace autolife