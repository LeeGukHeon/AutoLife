#include "common/Logger.h"
#include "common/PathUtils.h"
#include "execution/RateLimiter.h"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace {

std::uint64_t countNonEmptyLines(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return 0;
    }

    std::uint64_t count = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) {
            ++count;
        }
    }
    return count;
}

bool requireFieldEq(
    const nlohmann::json& summary,
    const char* key,
    std::uint64_t expected,
    std::string& error
) {
    if (!summary.contains(key) || !summary[key].is_number_unsigned()) {
        error = std::string("missing/invalid field: ") + key;
        return false;
    }
    const auto actual = summary[key].get<std::uint64_t>();
    if (actual != expected) {
        error = std::string("unexpected value: ") + key + " expected=" + std::to_string(expected)
            + " actual=" + std::to_string(actual);
        return false;
    }
    return true;
}

} // namespace

int main() {
    autolife::Logger::getInstance().initialize("logs");

    const auto telemetry_path =
        autolife::utils::PathUtils::resolveRelativePath("logs/upbit_compliance_telemetry.jsonl");
    const auto summary_path =
        autolife::utils::PathUtils::resolveRelativePath("logs/upbit_compliance_summary_runtime.json");

    const std::uint64_t line_count_before = countNonEmptyLines(telemetry_path);

    autolife::execution::RateLimiter limiter;
    limiter.recordHttpOutcome(
        "market",
        "/v1/market/all",
        429,
        std::optional<std::string>("group=market; min=57; sec=0")
    );
    limiter.recordHttpOutcome(
        "market",
        "/v1/market/all",
        200,
        std::optional<std::string>("group=market; min=57; sec=8")
    );

    const std::uint64_t line_count_after = countNonEmptyLines(telemetry_path);
    if (line_count_after < line_count_before + 2) {
        std::cerr << "telemetry jsonl append check failed: before=" << line_count_before
                  << " after=" << line_count_after << "\n";
        return 1;
    }

    std::ifstream summary_in(summary_path, std::ios::binary);
    if (!summary_in.is_open()) {
        std::cerr << "summary file missing: " << summary_path.string() << "\n";
        return 1;
    }

    nlohmann::json summary;
    try {
        summary_in >> summary;
    } catch (const std::exception& e) {
        std::cerr << "summary parse failed: " << e.what() << "\n";
        return 1;
    }

    std::string error;
    if (!requireFieldEq(summary, "request_count", 2, error)
        || !requireFieldEq(summary, "http_success_count", 1, error)
        || !requireFieldEq(summary, "rate_limit_429_count", 1, error)
        || !requireFieldEq(summary, "rate_limit_418_count", 0, error)
        || !requireFieldEq(summary, "throttle_event_count", 1, error)
        || !requireFieldEq(summary, "recover_event_count", 1, error)) {
        std::cerr << error << "\n";
        return 1;
    }

    if (!summary.contains("backoff_sleep_ms_total")
        || !summary["backoff_sleep_ms_total"].is_number_unsigned()
        || summary["backoff_sleep_ms_total"].get<std::uint64_t>() < 1000) {
        std::cerr << "invalid backoff_sleep_ms_total\n";
        return 1;
    }

    std::cout << "RateLimiter compliance telemetry test passed\n";
    return 0;
}
