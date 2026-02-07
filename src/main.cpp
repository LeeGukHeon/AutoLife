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
        std::cout << "   자동 스캘핑 트레이딩 시스템" << std::endl;
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
        
        // 엔진 설정
        std::cout << "========================================" << std::endl;
        std::cout << "   거래 설정" << std::endl;
        std::cout << "========================================" << std::endl;
        
        engine::EngineConfig engine_config;
        engine_config.mode = engine::TradingMode::PAPER;  // 모의 거래
        engine_config.dry_run = true;  
        engine_config.initial_capital = 1000000;          // 100만원
        engine_config.scan_interval_seconds = 60;         // 1분마다 스캔
        engine_config.min_volume_krw = 1000000000LL;  // 10억 (5배 완화) TEST용 1억으로 완화
        engine_config.max_positions = 5;                    // ✅ 3 → 5로 증가
        engine_config.max_daily_trades = 20;                // ✅ 10 → 20으로 증가
        engine_config.max_drawdown = 0.10;                // 최대 10% 손실
        engine_config.enabled_strategies = {"scalping", "momentum" , "breakout", "mean_reversion", "grid_trading"};
        
        std::cout << "거래 모드:       " 
                  << (engine_config.mode == engine::TradingMode::LIVE ? "🔴 실전" : "🟢 모의") 
                  << std::endl;
        std::cout << "초기 자본:       " << engine_config.initial_capital / 10000 << "만원" << std::endl;
        std::cout << "스캔 주기:       " << engine_config.scan_interval_seconds << "초" << std::endl;
        std::cout << "최소 거래량:     " << engine_config.min_volume_krw / 100000000 << "억" << std::endl;  // ✅ 10억 표시
        std::cout << "최대 포지션:     " << engine_config.max_positions << "개" << std::endl;
        std::cout << "일일 거래 한도:  " << engine_config.max_daily_trades << "회" << std::endl;
        std::cout << "최대 손실률:     " << (engine_config.max_drawdown * 100) << "%" << std::endl;
        std::cout << "활성 전략:       Scalping, Momentum, Breakout, Meanreversion, Grid" << std::endl;  // ✅ 수정
        std::cout << "========================================\n" << std::endl;
        
        // 엔진 생성
        std::cout << "⚙️  거래 엔진 초기화 중..." << std::endl;
        g_engine = std::make_unique<engine::TradingEngine>(
            engine_config,
            http_client
        );
        
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
