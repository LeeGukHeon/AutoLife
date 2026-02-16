#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace autolife {
namespace execution {

struct RateLimitConfig {
    std::string group_name;
    int max_per_second;
    int current_count;
    std::chrono::steady_clock::time_point window_start;

    RateLimitConfig(const std::string& name, int max_req)
        : group_name(name)
        , max_per_second(max_req)
        , current_count(0)
        , window_start(std::chrono::steady_clock::now()) {}
};

class RateLimiter {
public:
    RateLimiter();

    bool tryAcquire(const std::string& group);
    void acquire(const std::string& group);
    int getRemainingRequests(const std::string& group);

    void updateFromHeader(const std::string& remaining_req_header);
    void handleRateLimitError(int status_code);

    // Persist HTTP compliance telemetry and runtime summary.
    void recordHttpOutcome(
        const std::string& group,
        const std::string& source_tag,
        int status_code,
        const std::optional<std::string>& remaining_req_header
    );

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
    std::condition_variable cv_;

    int total_requests_;
    int rejected_requests_;
    int forced_waits_;
    std::chrono::milliseconds total_wait_time_;

    std::filesystem::path compliance_telemetry_path_;
    std::filesystem::path compliance_summary_path_;
    std::uint64_t compliance_request_count_;
    std::uint64_t compliance_http_success_count_;
    std::uint64_t compliance_rate_limit_429_count_;
    std::uint64_t compliance_rate_limit_418_count_;
    std::uint64_t compliance_retry_count_;
    std::uint64_t compliance_backoff_sleep_ms_total_;
    std::uint64_t compliance_throttle_event_count_;
    std::uint64_t compliance_recover_event_count_;
    bool last_response_rate_limited_;
    std::string first_event_utc_;
    std::string last_event_utc_;

    bool is_blocked_;
    std::chrono::steady_clock::time_point block_end_time_;

    void resetWindowIfNeeded(RateLimitConfig& config);
};

} // namespace execution
} // namespace autolife
