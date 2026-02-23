#include "execution/RateLimiter.h"

#include <algorithm>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
#include <random>
#include <regex>
#include <sstream>
#include <thread>

#include <nlohmann/json.hpp>

#include "common/Logger.h"
#include "common/PathUtils.h"

namespace autolife {
namespace execution {
namespace {

constexpr int kBackoff429BaseMs = 1000;
constexpr int kBackoff429MaxMs = 16000;
constexpr int kBackoff418Ms = 60 * 1000;
constexpr int kSecZeroJitterMaxMs = 50;

int boundedExp2BackoffMs(int base_ms, int attempt, int cap_ms) {
    const int safe_base = std::max(0, base_ms);
    const int safe_attempt = std::max(0, std::min(attempt, 10));
    std::uint64_t backoff = static_cast<std::uint64_t>(safe_base) << safe_attempt;
    const std::uint64_t cap = static_cast<std::uint64_t>(std::max(0, cap_ms));
    if (cap > 0) {
        backoff = std::min(backoff, cap);
    }
    return static_cast<int>(std::min<std::uint64_t>(
        backoff,
        static_cast<std::uint64_t>(std::numeric_limits<int>::max())));
}

int computeSecZeroThrottleMs() {
    const auto now_system = std::chrono::system_clock::now();
    const auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now_system.time_since_epoch()).count();
    const auto mod = static_cast<int>(epoch_ms % 1000);
    const int to_next_second_ms = mod == 0 ? 1000 : (1000 - mod);
    thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> jitter_dist(0, kSecZeroJitterMaxMs);
    const int jitter_ms = jitter_dist(rng);
    return std::max(0, to_next_second_ms) + std::max(0, jitter_ms);
}

std::string toUtcIso8601(std::chrono::system_clock::time_point tp) {
    const std::time_t as_time_t = std::chrono::system_clock::to_time_t(tp);
    std::tm utc_tm{};
#if defined(_WIN32)
    gmtime_s(&utc_tm, &as_time_t);
#else
    gmtime_r(&as_time_t, &utc_tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&utc_tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

nlohmann::json parseRemainingReqTokens(const std::string& header) {
    nlohmann::json result = nlohmann::json::object();
    std::stringstream ss(header);
    std::string token;
    while (std::getline(ss, token, ';')) {
        auto eq_pos = token.find('=');
        if (eq_pos == std::string::npos) {
            continue;
        }
        std::string key = token.substr(0, eq_pos);
        std::string value = token.substr(eq_pos + 1);

        key.erase(0, key.find_first_not_of(" \t\r\n"));
        key.erase(key.find_last_not_of(" \t\r\n") + 1);
        value.erase(0, value.find_first_not_of(" \t\r\n"));
        value.erase(value.find_last_not_of(" \t\r\n") + 1);

        if (key.empty()) {
            continue;
        }

        if (key == "sec" || key == "min") {
            try {
                result[key] = std::stoi(value);
            } catch (...) {
                result[key] = value;
            }
        } else {
            result[key] = value;
        }
    }
    return result;
}

bool appendJsonlLine(const std::filesystem::path& path, const nlohmann::json& payload) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out.is_open()) {
        return false;
    }
    out << payload.dump() << "\n";
    return static_cast<bool>(out);
}

bool writeJsonFile(const std::filesystem::path& path, const nlohmann::json& payload) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }
    out << payload.dump(2);
    return static_cast<bool>(out);
}

} // namespace

RateLimiter::RateLimiter()
    : total_requests_(0)
    , rejected_requests_(0)
    , forced_waits_(0)
    , total_wait_time_(std::chrono::milliseconds(0))
    , compliance_telemetry_path_(
          utils::PathUtils::resolveRelativePath("logs/upbit_compliance_telemetry.jsonl"))
    , compliance_summary_path_(
          utils::PathUtils::resolveRelativePath("logs/upbit_compliance_summary_runtime.json"))
    , compliance_request_count_(0)
    , compliance_http_success_count_(0)
    , compliance_rate_limit_429_count_(0)
    , compliance_rate_limit_418_count_(0)
    , compliance_retry_count_(0)
    , compliance_backoff_sleep_ms_total_(0)
    , compliance_throttle_event_count_(0)
    , compliance_recover_event_count_(0)
    , compliance_sec_zero_throttle_count_(0)
    , compliance_sec_zero_sleep_ms_total_(0)
    , last_response_rate_limited_(false)
    , rate_limit_429_streak_(0)
    , last_applied_backoff_ms_(0)
    , pending_sec_zero_throttle_ms_(0)
    , is_blocked_(false) {
    // Upbit official limits.
    configs_.emplace("market", RateLimitConfig("market", 10));
    configs_.emplace("candle", RateLimitConfig("candle", 10));
    configs_.emplace("ticker", RateLimitConfig("ticker", 10));
    configs_.emplace("orderbook", RateLimitConfig("orderbook", 10));
    configs_.emplace("trade", RateLimitConfig("trade", 10));
    configs_.emplace("accounts", RateLimitConfig("accounts", 30));
    configs_.emplace("order", RateLimitConfig("order", 8));
    configs_.emplace("default", RateLimitConfig("default", 30));

    LOG_INFO("RateLimiter initialized with official Upbit limits");
}

bool RateLimiter::tryAcquire(const std::string& group) {
    std::unique_lock<std::mutex> lock(mutex_);

    if (is_blocked_) {
        const auto now = std::chrono::steady_clock::now();
        if (now < block_end_time_) {
            ++rejected_requests_;
            return false;
        }
        is_blocked_ = false;
        cv_.notify_all();
    }

    auto it = configs_.find(group);
    if (it == configs_.end()) {
        it = configs_.find("default");
    }
    auto& config = it->second;
    resetWindowIfNeeded(config);

    if (config.current_count < config.max_per_second) {
        ++config.current_count;
        ++total_requests_;
        return true;
    }

    ++rejected_requests_;
    return false;
}

void RateLimiter::acquire(const std::string& group) {
    std::unique_lock<std::mutex> lock(mutex_);

    while (true) {
        if (is_blocked_) {
            const auto now = std::chrono::steady_clock::now();
            if (now < block_end_time_) {
                ++forced_waits_;
                total_wait_time_ += std::chrono::duration_cast<std::chrono::milliseconds>(block_end_time_ - now);
                cv_.wait_until(lock, block_end_time_);
            }
            if (std::chrono::steady_clock::now() >= block_end_time_) {
                is_blocked_ = false;
            } else {
                continue;
            }
        }

        auto it = configs_.find(group);
        if (it == configs_.end()) {
            it = configs_.find("default");
        }
        auto& config = it->second;
        resetWindowIfNeeded(config);

        if (config.current_count < config.max_per_second) {
            ++config.current_count;
            ++total_requests_;
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        const auto wake_time = config.window_start + std::chrono::seconds(1) + std::chrono::milliseconds(1);
        if (wake_time > now) {
            ++forced_waits_;
            total_wait_time_ += std::chrono::duration_cast<std::chrono::milliseconds>(wake_time - now);
            cv_.wait_until(lock, wake_time);
        }
    }
}

int RateLimiter::getRemainingRequests(const std::string& group) {
    std::unique_lock<std::mutex> lock(mutex_);

    auto it = configs_.find(group);
    if (it == configs_.end()) {
        it = configs_.find("default");
    }
    resetWindowIfNeeded(it->second);
    return std::max(0, it->second.max_per_second - it->second.current_count);
}

void RateLimiter::updateFromHeader(const std::string& remaining_req_header) {
    std::regex group_regex("group=([^;]+)");
    std::regex sec_regex("sec=([0-9]+)");

    std::smatch match;
    std::string group_name = "default";
    int remaining = -1;

    if (std::regex_search(remaining_req_header, match, group_regex) && match.size() > 1) {
        group_name = match[1].str();
    }
    if (std::regex_search(remaining_req_header, match, sec_regex) && match.size() > 1) {
        remaining = std::stoi(match[1].str());
    }

    if (remaining < 0) {
        return;
    }

    std::unique_lock<std::mutex> lock(mutex_);
    auto it = configs_.find(group_name);
    if (it == configs_.end()) {
        it = configs_.find("default");
    }
    if (it != configs_.end()) {
        const int used_remote = it->second.max_per_second - remaining;
        if (used_remote > it->second.current_count) {
            it->second.current_count = used_remote;
        }
    }

    pending_sec_zero_throttle_ms_ = 0;
    if (remaining == 0) {
        const int throttle_ms = computeSecZeroThrottleMs();
        pending_sec_zero_throttle_ms_ = throttle_ms;
        is_blocked_ = true;
        const auto candidate_end = std::chrono::steady_clock::now() + std::chrono::milliseconds(throttle_ms);
        if (candidate_end > block_end_time_) {
            block_end_time_ = candidate_end;
        }
        cv_.notify_all();
    }
}

void RateLimiter::handleRateLimitError(int status_code) {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    int backoff_ms = 0;

    if (status_code == 429) {
        rate_limit_429_streak_ = std::min(rate_limit_429_streak_ + 1, 10);
        backoff_ms = boundedExp2BackoffMs(
            kBackoff429BaseMs,
            rate_limit_429_streak_ - 1,
            kBackoff429MaxMs);
        LOG_WARN("429 Too Many Requests detected (backoff={}ms, streak={})",
                 backoff_ms, rate_limit_429_streak_);
        is_blocked_ = true;
    } else if (status_code == 418) {
        rate_limit_429_streak_ = 0;
        backoff_ms = kBackoff418Ms;
        LOG_ERROR("418 blocked detected (backoff={}ms)", backoff_ms);
        is_blocked_ = true;
    } else {
        rate_limit_429_streak_ = 0;
        last_applied_backoff_ms_ = 0;
        return;
    }

    if (backoff_ms > 0) {
        last_applied_backoff_ms_ = backoff_ms;
        const auto candidate_end = now + std::chrono::milliseconds(backoff_ms);
        if (candidate_end > block_end_time_) {
            block_end_time_ = candidate_end;
        }
    } else {
        last_applied_backoff_ms_ = 0;
    }
    cv_.notify_all();
}

void RateLimiter::recordHttpOutcome(
    const std::string& group,
    const std::string& source_tag,
    int status_code,
    const std::optional<std::string>& remaining_req_header
) {
    nlohmann::json event = nlohmann::json::object();
    nlohmann::json summary = nlohmann::json::object();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string now_utc = toUtcIso8601(std::chrono::system_clock::now());
        const int sec_zero_throttle_ms = pending_sec_zero_throttle_ms_;
        pending_sec_zero_throttle_ms_ = 0;
        if (first_event_utc_.empty()) {
            first_event_utc_ = now_utc;
        }
        last_event_utc_ = now_utc;

        ++compliance_request_count_;

        event["ts_utc"] = now_utc;
        event["group"] = group.empty() ? "default" : group;
        event["source"] = source_tag;
        event["status_code"] = status_code;
        if (remaining_req_header.has_value() && !remaining_req_header->empty()) {
            event["remaining_req"] = parseRemainingReqTokens(remaining_req_header.value());
        }

        if (status_code >= 200 && status_code < 300) {
            event["event"] = "http_success";
            rate_limit_429_streak_ = 0;
            ++compliance_http_success_count_;
            if (sec_zero_throttle_ms > 0) {
                event["remaining_req_sec0_throttle_ms"] = sec_zero_throttle_ms;
                ++compliance_throttle_event_count_;
                ++compliance_sec_zero_throttle_count_;
                compliance_sec_zero_sleep_ms_total_ += static_cast<std::uint64_t>(sec_zero_throttle_ms);
                compliance_backoff_sleep_ms_total_ += static_cast<std::uint64_t>(sec_zero_throttle_ms);
            }
            if (last_response_rate_limited_) {
                ++compliance_recover_event_count_;
                event["recovered_after_rate_limit"] = true;
                last_response_rate_limited_ = false;
            }
        } else if (status_code == 429 || status_code == 418) {
            event["event"] = "rate_limit_error";
            const int backoff_ms = last_applied_backoff_ms_ > 0
                ? last_applied_backoff_ms_
                : (status_code == 429 ? kBackoff429BaseMs : kBackoff418Ms);
            ++compliance_retry_count_;
            event["backoff_ms"] = backoff_ms;
            ++compliance_throttle_event_count_;
            compliance_backoff_sleep_ms_total_ += static_cast<std::uint64_t>(backoff_ms);
            if (status_code == 429) {
                ++compliance_rate_limit_429_count_;
            } else {
                ++compliance_rate_limit_418_count_;
            }
            last_response_rate_limited_ = true;
        } else {
            event["event"] = "http_error";
            last_response_rate_limited_ = false;
        }
        last_applied_backoff_ms_ = 0;

        summary["generated_at"] = now_utc;
        summary["source"] = "runtime";
        summary["request_count"] = compliance_request_count_;
        summary["http_success_count"] = compliance_http_success_count_;
        summary["rate_limit_429_count"] = compliance_rate_limit_429_count_;
        summary["rate_limit_418_count"] = compliance_rate_limit_418_count_;
        summary["retry_count"] = compliance_retry_count_;
        summary["backoff_sleep_ms_total"] = compliance_backoff_sleep_ms_total_;
        summary["throttle_event_count"] = compliance_throttle_event_count_;
        summary["recover_event_count"] = compliance_recover_event_count_;
        summary["sec_zero_throttle_count"] = compliance_sec_zero_throttle_count_;
        summary["sec_zero_sleep_ms_total"] = compliance_sec_zero_sleep_ms_total_;
        summary["telemetry_jsonl"] = compliance_telemetry_path_.string();
        summary["first_event_utc"] = first_event_utc_;
        summary["last_event_utc"] = last_event_utc_;
    }

    if (!appendJsonlLine(compliance_telemetry_path_, event)) {
        LOG_WARN("Failed to append compliance telemetry JSONL: {}", compliance_telemetry_path_.string());
    }
    if (!writeJsonFile(compliance_summary_path_, summary)) {
        LOG_WARN("Failed to write runtime compliance summary: {}", compliance_summary_path_.string());
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
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - config.window_start);
    if (elapsed.count() < 1000) {
        return;
    }
    config.current_count = 0;
    config.window_start = now;
    cv_.notify_all();
}

} // namespace execution
} // namespace autolife
