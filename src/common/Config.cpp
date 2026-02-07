#include "common/Config.h"
#include "common/PathUtils.h"
#include <fstream>
#include <iostream>

namespace autolife {

Config& Config::getInstance() {
    static Config instance;
    return instance;
}

void Config::load(const std::string& path) {
    try {
        // 실행 파일 기준으로 경로 해석
        std::filesystem::path config_path;
        
        // 절대 경로인 경우 그대로 사용
        if (std::filesystem::path(path).is_absolute()) {
            config_path = path;
        } else {
            // 상대 경로인 경우 실행 파일 기준
            config_path = utils::PathUtils::resolveRelativePath(path);
        }
        
        std::cout << "설정 파일 경로: " << config_path << std::endl;
        
        // 파일 존재 확인
        if (!std::filesystem::exists(config_path)) {
            std::cout << "경고: 설정 파일을 찾을 수 없습니다: " << config_path << std::endl;
            std::cout << "기본값을 사용합니다." << std::endl;
            return;
        }
        
        std::ifstream file(config_path);
        if (!file.is_open()) {
            std::cout << "경고: 설정 파일을 열 수 없습니다." << std::endl;
            return;
        }
        
        nlohmann::json j;
        file >> j;
        
        if (j.contains("api")) {
            access_key_ = j["api"].value("access_key", "");
            secret_key_ = j["api"].value("secret_key", "");
        }
        
        if (j.contains("trading")) {
            initial_capital_ = j["trading"].value("initial_capital", 1000000.0);
            max_drawdown_ = j["trading"].value("max_drawdown", 0.15);
            position_size_ratio_ = j["trading"].value("position_size_ratio", 0.01);
        }
        
        std::cout << "설정 파일 로드 완료" << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "설정 로드 오류: " << e.what() << std::endl;
    }
}

} // namespace autolife
