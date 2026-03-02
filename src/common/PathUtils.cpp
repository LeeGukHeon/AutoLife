#include "common/PathUtils.h"
#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <stdexcept>

namespace autolife {
namespace utils {

namespace {

std::filesystem::path g_run_dir_override;
bool g_has_run_dir_override = false;

std::string trimCopy(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string lowerCopy(std::string value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool startsWithLogsComponent(const std::filesystem::path& relative_path) {
    auto it = relative_path.begin();
    if (it == relative_path.end()) {
        return false;
    }
    return lowerCopy(it->string()) == "logs";
}

std::filesystem::path suffixAfterFirstComponent(const std::filesystem::path& relative_path) {
    std::filesystem::path suffix;
    auto it = relative_path.begin();
    if (it == relative_path.end()) {
        return suffix;
    }
    ++it;
    for (; it != relative_path.end(); ++it) {
        suffix /= *it;
    }
    return suffix;
}

}  // namespace

std::filesystem::path PathUtils::getExecutableDir() {
    char buffer[MAX_PATH];
    GetModuleFileNameA(NULL, buffer, MAX_PATH);
    std::filesystem::path exe_path(buffer);
    return exe_path.parent_path();
}

std::filesystem::path PathUtils::resolveRelativePath(const std::string& relative_path) {
    std::filesystem::path path_value(relative_path);
    if (path_value.empty()) {
        return getExecutableDir();
    }
    if (path_value.is_absolute()) {
        return path_value.lexically_normal();
    }
    if (startsWithLogsComponent(path_value)) {
        const auto suffix = suffixAfterFirstComponent(path_value);
        const auto run_root = getRunDir();
        if (suffix.empty()) {
            return run_root.lexically_normal();
        }
        return (run_root / suffix).lexically_normal();
    }
    return (getExecutableDir() / path_value).lexically_normal();
}

std::filesystem::path PathUtils::getConfigDir() {
    return getExecutableDir() / "config";
}

std::filesystem::path PathUtils::getLogsDir() {
    return getRunDir();
}

void PathUtils::setRunDir(const std::string& run_dir) {
    const std::string raw = trimCopy(run_dir);
    if (raw.empty()) {
        g_has_run_dir_override = false;
        g_run_dir_override.clear();
        return;
    }

    std::filesystem::path path_value(raw);
    if (!path_value.is_absolute()) {
        path_value = std::filesystem::absolute(path_value);
    }
    path_value = path_value.lexically_normal();

    std::error_code ec;
    std::filesystem::create_directories(path_value, ec);
    if (ec) {
        throw std::runtime_error(
            "Failed to create run directory: " + path_value.string() + " (" + ec.message() + ")"
        );
    }

    g_run_dir_override = path_value;
    g_has_run_dir_override = true;
}

std::filesystem::path PathUtils::getRunDir() {
    if (g_has_run_dir_override && !g_run_dir_override.empty()) {
        return g_run_dir_override;
    }
    return (getExecutableDir() / "logs").lexically_normal();
}

bool PathUtils::hasRunDirOverride() {
    return g_has_run_dir_override && !g_run_dir_override.empty();
}

} // namespace utils
} // namespace autolife
