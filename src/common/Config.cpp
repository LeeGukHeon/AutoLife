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
        
        // 3. 거래 설정 파싱 (trading 섹션)
        if (j.contains("trading")) {
            auto& t = j["trading"];

            // [기존 멤버 변수 업데이트] (호환성 유지)
            initial_capital_ = t.value("initial_capital", 50000.0);
            max_drawdown_ = t.value("max_drawdown", 0.15);
            position_size_ratio_ = t.value("position_size_ratio", 0.01);
            log_level_ = t.value("log_level", "info");

            // [✅ EngineConfig 구조체 업데이트]
            // 기본값은 생성자에서 이미 세팅되어 있으므로, 읽은 값만 덮어씀
            
            // 모드 설정
            std::string mode_str = t.value("mode", "PAPER");
            engine_config_.mode = (mode_str == "LIVE") ? engine::TradingMode::LIVE : engine::TradingMode::PAPER;
            
            engine_config_.dry_run = t.value("dry_run", false);
            engine_config_.initial_capital = initial_capital_; // 위에서 읽은 값 사용
            
            engine_config_.scan_interval_seconds = t.value("scan_interval_seconds", 60);
            engine_config_.min_volume_krw = t.value("min_volume_krw", 1000000000LL); // long long
            
            engine_config_.max_positions = t.value("max_positions", 10);
            engine_config_.max_daily_trades = t.value("max_daily_trades", 50);
            engine_config_.max_drawdown = max_drawdown_; // 위에서 읽은 값 사용
            
            // 안전 설정
            engine_config_.max_daily_loss_krw = t.value("max_daily_loss_krw", 50000.0);
            engine_config_.max_order_krw = t.value("max_order_krw", 500000.0);
            engine_config_.min_order_krw = t.value("min_order_krw", 5000.0);
            
            // 포트폴리오 노출 비율 설정
            engine_config_.max_exposure_pct = t.value("max_exposure_pct", 0.85); // 기본값 85%

            // 전략 목록
            if (t.contains("enabled_strategies")) {
                engine_config_.enabled_strategies = t["enabled_strategies"].get<std::vector<std::string>>();
            }
        }
        
        std::cout << "설정 파일 로드 완료" << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "설정 로드 오류: " << e.what() << std::endl;
    }
}

} // namespace autolife
