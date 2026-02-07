#pragma once

#include <string>
#include <filesystem>

namespace autolife {
namespace utils {

class PathUtils {
public:
    // 실행 파일의 디렉토리 경로 반환
    static std::filesystem::path getExecutableDir();
    
    // 실행 파일 기준 상대 경로를 절대 경로로 변환
    static std::filesystem::path resolveRelativePath(const std::string& relative_path);
    
    // config 디렉토리 경로
    static std::filesystem::path getConfigDir();
    
    // logs 디렉토리 경로
    static std::filesystem::path getLogsDir();
};

} // namespace utils
} // namespace autolife
