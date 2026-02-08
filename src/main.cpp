#include "common/Logger.h"
#include "common/Config.h"
#include "network/UpbitHttpClient.h"
#include "engine/TradingEngine.h"
#include <iostream>
#include <csignal>
#include <memory>
#include <Windows.h>

using namespace autolife;

// 전역 엔진 (Ctrl+C 처리용)
std::unique_ptr<engine::TradingEngine> g_engine;

void signalHandler(int signal) {
    if (signal == SIGINT) {
        LOG_INFO("\n종료 신호 수신 (Ctrl+C)");
        if (g_engine) {
            g_engine->stop();
        }
    }
}

int main() {
    try {
        // 콘솔 UTF-8 설정
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
        
        // 로거 초기화
        Logger::getInstance().initialize("logs");
        
        std::cout << "========================================" << std::endl;
        std::cout << "   AutoLife Trading Bot v1.0" << std::endl;
        std::cout << "   자동 트레이딩 시스템" << std::endl;
        std::cout << "========================================\n" << std::endl;
        
        LOG_INFO("========================================");
        LOG_INFO("AutoLife Trading Bot v1.0");
        LOG_INFO("========================================");
        
        // 설정 로드
        Config::getInstance().load("config/config.json");
        auto& config = Config::getInstance();
        
        std::string access_key = config.getAccessKey();
        std::string secret_key = config.getSecretKey();
        
        if (access_key.empty() || secret_key.empty()) {
            std::cout << "⚠️  API 키가 설정되지 않았습니다" << std::endl;
            std::cout << "config/config.json 파일을 확인하세요" << std::endl;
            LOG_ERROR("API 키가 설정되지 않았습니다");
            
            std::cout << "\n엔터를 눌러 종료..." << std::endl;
            std::cin.get();
            return 1;
        }
        
        // 2. 완성된 엔진 설정 가져오기 (이 한 줄로 끝!)
        engine::EngineConfig engine_config = config.getEngineConfig();

        // HTTP 클라이언트 생성
        auto http_client = std::make_shared<network::UpbitHttpClient>(
            access_key,
            secret_key
        );
        
        // 연결 테스트
        std::cout << "📡 업비트 API 연결 테스트..." << std::endl;
        LOG_INFO("업비트 API 연결 테스트...");
        
        // getMarkets() 호출 후 KRW 마켓만 필터링
        auto all_markets = http_client->getMarkets();
        int krw_count = 0;
        
        if (all_markets.is_array()) {
            for (const auto& market : all_markets) {
                if (market.contains("market")) {
                    std::string market_name = market["market"].get<std::string>();
                    if (market_name.substr(0, 3) == "KRW") {
                        krw_count++;
                    }
                }
            }
        }
        
        std::cout << "✅ 연결 성공! KRW 마켓: " << krw_count << "개\n" << std::endl;
        LOG_INFO("KRW 마켓: {}개", krw_count);
        
        // 4. 설정 정보 출력
        std::cout << "========================================" << std::endl;
        std::cout << "   거래 설정 (Config 클래스 로드됨)" << std::endl;
        std::cout << "========================================" << std::endl;
        std::cout << "모드:          " << (engine_config.mode == engine::TradingMode::LIVE ? "🔴 LIVE" : "🟢 PAPER") << std::endl;
        std::cout << "Dry Run:       " << (engine_config.dry_run ? "ON" : "OFF") << std::endl;
        std::cout << "초기 자본:     " << (long long)engine_config.initial_capital << " KRW" << std::endl;
        // ... (나머지 출력) ...
        std::cout << "========================================\n" << std::endl;
        
        // 5. 엔진 생성 및 시작
        g_engine = std::make_unique<engine::TradingEngine>(engine_config, http_client);
        
        // Ctrl+C 핸들러 등록
        std::signal(SIGINT, signalHandler);
        
        // 엔진 시작
        std::cout << "\n🚀 거래 엔진 시작!" << std::endl;
        std::cout << "⏸️  중지하려면 Ctrl+C를 누르세요\n" << std::endl;
        std::cout << "========================================\n" << std::endl;
        
        if (!g_engine->start()) {
            LOG_ERROR("엔진 시작 실패");
            std::cout << "❌ 엔진 시작 실패" << std::endl;
            std::cin.get();
            return 1;
        }
        
        // 6. 실전 잔고 확인 (엔진 시작 후)
        if (engine_config.mode == engine::TradingMode::LIVE) {
            auto metrics = g_engine->getMetrics();
            std::cout << "\n💰 [실제 계좌 연동 완료]" << std::endl;
            std::cout << "   보유 현금: " << (long long)metrics.total_capital << " KRW" << std::endl;
        }
        
        // 메인 스레드 대기 (Ctrl+C까지)
        while (g_engine->isRunning()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
        std::cout << "\n========================================" << std::endl;
        std::cout << "   프로그램 종료" << std::endl;
        std::cout << "========================================" << std::endl;
        LOG_INFO("프로그램 종료");
        
        std::cout << "\n엔터를 눌러 종료..." << std::endl;
        std::cin.get();
        
    } catch (const std::exception& e) {
        LOG_ERROR("치명적 오류: {}", e.what());
        std::cout << "\n❌ 오류 발생: " << e.what() << std::endl;
        std::cout << "엔터를 눌러 종료..." << std::endl;
        std::cin.get();
        return 1;
    }
    
    return 0;
}
