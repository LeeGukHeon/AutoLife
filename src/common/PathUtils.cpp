#include "common/PathUtils.h"
#include <Windows.h>

namespace autolife {
namespace utils {

std::filesystem::path PathUtils::getExecutableDir() {
    char buffer[MAX_PATH];
    GetModuleFileNameA(NULL, buffer, MAX_PATH);
    std::filesystem::path exe_path(buffer);
    return exe_path.parent_path();
}

std::filesystem::path PathUtils::resolveRelativePath(const std::string& relative_path) {
    return getExecutableDir() / relative_path;
}

std::filesystem::path PathUtils::getConfigDir() {
    return getExecutableDir() / "config";
}

std::filesystem::path PathUtils::getLogsDir() {
    return getExecutableDir() / "logs";
}

} // namespace utils
} // namespace autolife
